#!/usr/bin/env python3
"""The Familia - Switch NSP builder (GUI, Windows and Linux).

Builds a Switch NSP of the Mafia port from YOUR OWN copy of Mafia 1.2:

  1. translate Game.exe and the engine DLLs to C       (container)
  2. compile them with the port's runtime for Switch    (container)
  3. stage ExeFS / Control / RomFS                      (container)
  4. pack and sign with hacBrewPack and your prod.keys  (this PC)
  5. optionally check every NCA hash with hactool       (container)

Nothing is downloaded except the container image's base layers. Your
prod.keys never enter the container for steps 1-3; a filtered temporary copy
is used for packing/verifying and deleted afterwards. The NSP you make
contains code derived from your game (and, all-in-one, the game itself):
keep it for your own console, never share it.

  python3 tools/nsp_builder.py            # GUI
  python3 tools/nsp_builder.py --cli --game DIR --keys prod.keys [--full] [--jobs N]
"""
import argparse, os, platform, queue, re, shutil, subprocess, sys, tempfile, threading, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = "familia-switch:1"
STAGE = ROOT / "build/switch/nsp_stage"
IS_WIN = platform.system() == "Windows"
STEPS = ["image", "translate", "compile", "stage", "pack", "verify"]


# ---------------------------------------------------------------- helpers
def find_engine():
    for e in ("podman", "docker"):
        if shutil.which(e):
            return e
    return None


def ram_gb():
    try:
        if IS_WIN:
            import ctypes
            class M(ctypes.Structure):
                _fields_ = [("l", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                            ("avail", ctypes.c_ulonglong)] + [(f"x{i}", ctypes.c_ulonglong) for i in range(5)]
            m = M(); m.l = ctypes.sizeof(M)
            ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
            return m.total / 2**30
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / 2**30
    except Exception:
        return 16.0


def default_jobs():
    # the largest generated C files need up to ~11 GB of RAM each while
    # compiling; keep ~6 GB for the system and the desktop
    return max(1, min(os.cpu_count() or 1, int((ram_gb() - 6) // 13)))


def find_hacbrewpack():
    for n in ("hacbrewpack", "hacBrewPack", "hacbrewpack.exe", "hacBrewPack.exe"):
        p = shutil.which(n)
        if p:
            return p
    for p in (ROOT / "tools/bin/hacbrewpack.exe", ROOT / "tools/bin/hacbrewpack",
              Path.home() / ".local/bin/hacbrewpack"):
        if p.exists():
            return str(p)
    return ""


def game_exe(game):
    for n in ("Game.exe", "game.exe", "GAME.EXE"):
        if (Path(game) / n).is_file():
            return Path(game) / n
    return None


def filtered_keys(src, dst):
    """Only plain name = 32/64-hex lines: hacBrewPack 3.05 rejects newer formats."""
    n = 0
    with open(src, encoding="utf-8", errors="replace") as f, open(dst, "w") as o:
        for line in f:
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            v = re.sub(r"\s", "", v)
            if len(v) in (32, 64) and re.fullmatch(r"[0-9a-fA-F]+", v):
                o.write(f"{k.strip()} = {v}\n"); n += 1
    if IS_WIN is False:
        os.chmod(dst, 0o600)
    return n


def mount(host, guest, ro=False):
    return ["-v", f"{Path(host).resolve()}:{guest}" + (":ro" if ro else "")]


class Cancelled(Exception):
    pass


# ---------------------------------------------------------------- the build
class Builder:
    def __init__(self, cfg, emit):
        self.cfg, self.emit = cfg, emit          # emit(kind, value)
        self.proc = None
        self.cancelled = False

    def log(self, s):
        self.emit("log", s)

    def run(self, cmd, cwd=None, parse=False):
        if self.cancelled:
            raise Cancelled()
        self.proc = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, encoding="utf-8", errors="replace", bufsize=1)
        total, done = 0, 0
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if parse and line.startswith("@@"):
                tag, _, rest = line[2:].partition(" ")
                if tag == "STEP":
                    self.emit("step", STEPS[int(rest.split()[0])])
                elif tag == "TOTAL":
                    total, done = int(rest or 0), 0
                    self.emit("progress", (0, total))
                    self.log(f"compiling {total} files")
                elif tag == "TICK":
                    done += 1
                    self.emit("progress", (done, total))
                    self.log(f"[{done}/{total}] {rest.rsplit('/', 1)[-1]}")
                elif tag == "ERROR":
                    self.log("ERROR: " + rest)
                elif tag == "DONE":
                    pass
                continue
            self.log(line)
        rc = self.proc.wait()
        self.proc = None
        if self.cancelled:
            raise Cancelled()
        if rc:
            raise RuntimeError(f"{Path(cmd[0]).name} failed (exit {rc})")

    def engine_run_args(self):
        e = self.cfg["engine"]
        a = [e, "run", "--rm"]
        if not IS_WIN:
            a += ["--security-opt", "label=disable"]
            if Path(e).name == "docker":   # rootful docker: keep files owned by you
                a += ["-u", f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/tmp"]
        return a

    def check(self):
        c, errs = self.cfg, []
        if not c["engine"] or not shutil.which(c["engine"]):
            errs.append("Docker or Podman not found (install Docker Desktop on Windows, docker/podman on Linux).")
        if not game_exe(c["game"]):
            errs.append("The game folder has no Game.exe (use an installed, 1.2-patched, unprotected Mafia).")
        else:
            g = Path(c["game"])
            for f in ("LS3DF.dll", "rw_data.dll", "IJoy.dll"):
                if not any((g / n).is_file() for n in (f, f.lower(), f.upper())):
                    errs.append(f"The game folder has no {f}.")
        if not Path(c["keys"]).is_file():
            errs.append("prod.keys not found (dump it from your own console with Lockpick_RCM).")
        if not c["hbp"] or not Path(c["hbp"]).is_file():
            errs.append("hacBrewPack not found (see docs/SWITCH_GUIDE.md).")
        if not (Path(c["pcrecomp"]) / "tools/lift/lift32.py").is_file():
            errs.append("PCRecomp not found: run  git submodule update --init  in the repository.")
        free = shutil.disk_usage(ROOT).free / 2**30
        need = 25 + (6 if c["full"] else 0)
        if free < need:
            errs.append(f"Only {free:.0f} GB free next to the repository; about {need} GB is needed.")
        return errs

    def build(self):
        c = self.cfg
        t0 = time.time()
        errs = self.check()
        if errs:
            raise RuntimeError("\n".join(errs))
        e = c["engine"]
        # 0. image
        self.emit("step", "image")
        if subprocess.run([e, "image", "inspect", IMAGE], capture_output=True).returncode:
            self.log(f"building the {IMAGE} image (first time only, a few minutes)")
            self.run([e, "build", "-t", IMAGE, "-f", str(ROOT / "docker/Dockerfile.switch"), str(ROOT / "docker")])
        else:
            self.log(f"image {IMAGE} present")
        # 1-3. translate, compile, stage in the container
        cmd = self.engine_run_args() + mount(ROOT, "/src") + mount(c["game"], "/game", True) \
            + mount(c["pcrecomp"], "/pcrecomp", True) \
            + ["-e", f"JOBS={c['jobs']}", "-e", f"RETRANSLATE={1 if c['retranslate'] else 0}",
               "-w", "/src", IMAGE, "bash", "switch/container_build.sh"]
        self.run(cmd, parse=True)
        # 4. pack on this PC
        self.emit("step", "pack")
        out_dir = Path(c["out"]); out_dir.mkdir(parents=True, exist_ok=True)
        nsp_out = out_dir / ("Mafia_full.nsp" if c["full"] else "Mafia.nsp")
        romfs_data = STAGE / "romfs/data"
        if romfs_data.exists():
            shutil.rmtree(romfs_data)
        if c["full"]:
            videos = {}
            if c.get("hevc", True):
                sys.path.insert(0, str(ROOT / "tools"))
                import encode_videos
                videos = encode_videos.encode(c["game"], ROOT / "build/videos", log=self.log)
            self.stage_game(Path(c["game"]), romfs_data, videos)
        else:
            (STAGE / "romfs/README.txt").write_text("Game data is read from sdmc:/switch/mafia/data\n")
        tmp = Path(tempfile.mkdtemp(prefix="familia_"))
        try:
            keys = tmp / "keys.txt"
            n = filtered_keys(c["keys"], keys)
            self.log(f"prod.keys: {n} usable entries (temporary copy, deleted afterwards)")
            work = STAGE.parent / "nsp_work"
            if work.exists():
                shutil.rmtree(work)
            for d in ("hac_tmp", "nca", "nsp"):
                (work / d).mkdir(parents=True)
            self.run([c["hbp"], "-k", str(keys), "--exefsdir", str(STAGE / "exefs"),
                      "--romfsdir", str(STAGE / "romfs"), "--controldir", str(STAGE / "control"),
                      "--tempdir", str(work / "hac_tmp"), "--ncadir", str(work / "nca"),
                      "--nspdir", str(work / "nsp"), "--nologo"], cwd=str(tmp))
            built = sorted((work / "nsp").glob("*.nsp"))
            if not built:
                raise RuntimeError("hacBrewPack produced no NSP (check that prod.keys is from your console)")
            shutil.move(str(built[0]), nsp_out)
            shutil.rmtree(work, ignore_errors=True)
            self.log(f"NSP: {nsp_out} ({nsp_out.stat().st_size / 2**20:.0f} MB)")
            # 5. verify
            if c["verify"]:
                self.emit("step", "verify")
                self.verify(nsp_out, keys)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
            if romfs_data.exists():
                shutil.rmtree(romfs_data, ignore_errors=True)
        self.log(f"finished in {(time.time() - t0) / 60:.0f} min: {nsp_out}")
        return nsp_out

    def stage_game(self, src, dst, videos=None):
        """the game folder into romfs/data. Left out: videos replaced by their
        H.265 re-encode (videos: original name -> .mkv), and name.bik where
        name.avi exists (the game plays the .avi; the .bik is a spare copy)."""
        videos = videos or {}
        files = [p for p in src.rglob("*") if p.is_file()]
        top = {p.name.lower() for p in files if p.parent == src}
        def left_out(p):
            if p.parent != src:
                return False
            if p.name in videos:
                return True
            return p.suffix.lower() == ".bik" and (p.stem.lower() + ".avi") in top
        dropped = [p for p in files if left_out(p)]
        files = [p for p in files if not left_out(p)]
        if dropped:
            self.log("left out (re-encoded or unused): " + ", ".join(p.name for p in dropped) +
                     f" ({sum(p.stat().st_size for p in dropped) / 2**20:.0f} MB)")
        for v in videos.values():
            dst.mkdir(parents=True, exist_ok=True)
            shutil.copy2(v, dst / Path(v).name)
        total = sum(p.stat().st_size for p in files) or 1
        self.log(f"adding your game files to the NSP: {len(files)} files, {total / 2**30:.1f} GB")
        done, last = 0, 0
        for p in files:
            if self.cancelled:
                raise Cancelled()
            q = dst / p.relative_to(src)
            q.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.link(p, q)              # same drive: no copy needed
            except OSError:
                shutil.copy2(p, q)
            done += p.stat().st_size
            if done - last > total / 100:
                last = done
                self.emit("progress", (done, total))
        self.emit("progress", (total, total))

    def verify(self, nsp, keys):
        """hactool (in the image): unpack the PFS0, check every NCA's hashes."""
        tmp = Path(tempfile.mkdtemp(prefix="familia_v_", dir=str(STAGE.parent)))
        try:
            # hactool's [WARN] lines repeat key values: they are dropped here and
            # nothing else from its output is shown except hash verdicts.
            # Nintendo's signature fields are always empty in a self-built NSP
            # (Atmosphere + sigpatches skip them), so only content hashes count.
            script = ("set -e; cd /v; hactool -t pfs0 --outdir=nsp /nsp/" + nsp.name + " >/dev/null 2>&1; "
                      "for f in nsp/*.nca; do "
                      "r=$(hactool -k /keys/keys.txt -y \"$f\" 2>&1 | grep -v '^\\[WARN\\]' | grep -E '\\((GOOD|FAIL)\\)' "
                      "| grep -Ev 'Fixed-Key Signature|^ *Signature \\(' || true); "
                      "g=$(echo \"$r\" | grep -c '(GOOD)' || true); "
                      "if echo \"$r\" | grep -q '(FAIL)'; then echo \"BAD $(basename $f): hash check failed\"; exit 1; fi; "
                      "[ \"$g\" -gt 0 ] || { echo \"BAD $(basename $f): could not be read (wrong prod.keys?)\"; exit 1; }; "
                      "echo \"ok  $(basename $f): $g hashes good, $(stat -c %s $f) bytes\"; done")
            self.run(self.engine_run_args() + mount(tmp, "/v") + mount(nsp.parent, "/nsp", True)
                     + mount(keys.parent, "/keys", True) + [IMAGE, "bash", "-c", script])
            self.log("verify: every NCA hash checks out")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def cancel(self):
        self.cancelled = True
        if self.proc:
            self.proc.terminate()


# ---------------------------------------------------------------- GUI
def gui():
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    root = tk.Tk()
    root.title("The Familia - Switch NSP builder")
    root.minsize(760, 560)
    q = queue.Queue()
    st = {"builder": None}
    v = {
        "game": tk.StringVar(), "keys": tk.StringVar(), "hbp": tk.StringVar(value=find_hacbrewpack()),
        "pcrecomp": tk.StringVar(value=str(ROOT / "third_party/pcrecomp-src")),
        "out": tk.StringVar(value=str(ROOT / "build/switch")), "engine": tk.StringVar(value=find_engine() or ""),
        "jobs": tk.IntVar(value=default_jobs()), "full": tk.BooleanVar(value=True),
        "verify": tk.BooleanVar(value=True), "retranslate": tk.BooleanVar(value=False),
    }
    f = ttk.Frame(root, padding=10); f.pack(fill="both", expand=True)
    f.columnconfigure(1, weight=1)

    def row(r, label, var, kind):
        ttk.Label(f, text=label).grid(row=r, column=0, sticky="w", pady=2)
        ttk.Entry(f, textvariable=var).grid(row=r, column=1, sticky="ew", padx=4)
        def pick():
            p = filedialog.askdirectory() if kind == "dir" else filedialog.askopenfilename()
            if p:
                var.set(p)
        ttk.Button(f, text="Browse...", command=pick).grid(row=r, column=2)

    row(0, "Mafia 1.2 game folder", v["game"], "dir")
    row(1, "prod.keys (your console)", v["keys"], "file")
    row(2, "hacBrewPack", v["hbp"], "file")
    row(3, "PCRecomp folder", v["pcrecomp"], "dir")
    row(4, "Output folder", v["out"], "dir")
    opt = ttk.Frame(f); opt.grid(row=5, column=0, columnspan=3, sticky="w", pady=6)
    ttk.Label(opt, text="Container engine").pack(side="left")
    ttk.Combobox(opt, textvariable=v["engine"], values=[e for e in ("docker", "podman") if shutil.which(e)],
                 width=8).pack(side="left", padx=4)
    ttk.Label(opt, text="   Parallel compile jobs").pack(side="left")
    ttk.Spinbox(opt, from_=1, to=os.cpu_count() or 8, textvariable=v["jobs"], width=4).pack(side="left", padx=4)
    ttk.Label(opt, text=f"(up to ~12 GB RAM each; this PC has {ram_gb():.0f} GB)").pack(side="left")
    opt2 = ttk.Frame(f); opt2.grid(row=6, column=0, columnspan=3, sticky="w")
    ttk.Checkbutton(opt2, text="All-in-one NSP (game files inside, nothing to copy to the SD card)",
                    variable=v["full"]).pack(anchor="w")
    ttk.Checkbutton(opt2, text="Verify the NSP with hactool after packing", variable=v["verify"]).pack(anchor="w")
    ttk.Checkbutton(opt2, text="Translate again even if the game files are unchanged",
                    variable=v["retranslate"]).pack(anchor="w")
    stepl = ttk.Label(f, text="Ready. The first build takes 1-3 hours; later builds reuse the work.")
    stepl.grid(row=7, column=0, columnspan=3, sticky="w", pady=(8, 2))
    bar = ttk.Progressbar(f, maximum=1000); bar.grid(row=8, column=0, columnspan=3, sticky="ew")
    logw = tk.Text(f, height=16, wrap="none", font=("Courier", 9)); logw.grid(row=9, column=0, columnspan=3, sticky="nsew", pady=6)
    f.rowconfigure(9, weight=1)
    btns = ttk.Frame(f); btns.grid(row=10, column=0, columnspan=3, sticky="e")
    names = {"image": "0/5 Preparing the build image", "translate": "1/5 Translating Game.exe to C (10-30 min)",
             "compile": "2/5 Compiling for Switch (the long part)", "stage": "3/5 Staging the NSP contents",
             "pack": "4/5 Packing and signing with hacBrewPack", "verify": "5/5 Verifying the NSP"}

    def cfg():
        return {k: x.get() for k, x in v.items()}

    def check():
        errs = Builder(cfg(), lambda *a: None).check()
        if errs:
            messagebox.showwarning("Setup", "\n\n".join(errs))
        else:
            messagebox.showinfo("Setup", "Everything is in place.")

    def start():
        if st["builder"]:
            return
        b = Builder(cfg(), lambda k, val: q.put((k, val)))
        st["builder"] = b
        go.config(state="disabled"); stop.config(state="normal")
        logw.delete("1.0", "end")

        def work():
            try:
                q.put(("done", str(b.build())))
            except Cancelled:
                q.put(("fail", "Cancelled."))
            except Exception as ex:
                q.put(("fail", str(ex)))
        threading.Thread(target=work, daemon=True).start()

    def poll():
        try:
            while True:
                k, val = q.get_nowait()
                if k == "log":
                    logw.insert("end", val + "\n"); logw.see("end")
                elif k == "step":
                    stepl.config(text=names.get(val, val)); bar.config(value=0)
                elif k == "progress":
                    d, t = val
                    bar.config(value=1000 * d / t if t else 0)
                elif k in ("done", "fail"):
                    st["builder"] = None
                    go.config(state="normal"); stop.config(state="disabled")
                    bar.config(value=1000 if k == "done" else 0)
                    if k == "done":
                        stepl.config(text="Done: " + val)
                        messagebox.showinfo("Done", f"NSP ready:\n{val}\n\nInstall it with DBI, Tinfoil, etc. "
                                            "It contains code made from your game: keep it for your own console.")
                    else:
                        stepl.config(text="Stopped.")
                        logw.insert("end", val + "\n"); logw.see("end")
                        messagebox.showerror("Build stopped", val)
        except queue.Empty:
            pass
        root.after(100, poll)

    def cancel():
        if st["builder"]:
            st["builder"].cancel()

    ttk.Button(btns, text="Check setup", command=check).pack(side="left", padx=4)
    go = ttk.Button(btns, text="Build NSP", command=start); go.pack(side="left", padx=4)
    stop = ttk.Button(btns, text="Cancel", command=cancel, state="disabled"); stop.pack(side="left", padx=4)
    poll()
    root.mainloop()


def cli(a):
    c = {"game": a.game, "keys": a.keys, "hbp": a.hacbrewpack or find_hacbrewpack(),
         "pcrecomp": a.pcrecomp, "out": a.out, "engine": a.engine or find_engine(),
         "jobs": a.jobs, "full": a.full, "verify": not a.no_verify, "retranslate": a.retranslate,
         "hevc": not a.original_videos}
    last = [None]

    def emit(k, val):
        if k == "log":
            print(val, flush=True)
        elif k == "step":
            print(f"==== {val}", flush=True)
        elif k == "progress" and val[1] and last[0] != (pct := 100 * val[0] // val[1]) and pct % 10 == 0:
            last[0] = pct
    b = Builder(c, emit)
    try:
        print("NSP:", b.build())
    except Exception as ex:
        print("FAILED:", ex, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", action="store_true", help="no window, print progress")
    ap.add_argument("--game"); ap.add_argument("--keys"); ap.add_argument("--hacbrewpack")
    ap.add_argument("--pcrecomp", default=str(ROOT / "third_party/pcrecomp-src"))
    ap.add_argument("--out", default=str(ROOT / "build/switch"))
    ap.add_argument("--engine"); ap.add_argument("--jobs", type=int, default=default_jobs())
    ap.add_argument("--full", action="store_true", help="all-in-one NSP with the game inside")
    ap.add_argument("--no-verify", action="store_true")
    ap.add_argument("--retranslate", action="store_true")
    ap.add_argument("--original-videos", action="store_true", help="keep the original videos (no H.265 re-encode)")
    a = ap.parse_args()
    cli(a) if a.cli else gui()
