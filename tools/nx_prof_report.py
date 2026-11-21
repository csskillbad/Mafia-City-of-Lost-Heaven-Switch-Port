#!/usr/bin/env python3
"""Turn sd:/switch/mafia/prof.txt (switch/nx_prof.c) into a function profile.

    python3 tools/nx_prof_report.py prof.txt build/switch/familia.elf [--src DIR] [-n 40]

Symbols come from `nm` on the ELF (devkitA64's aarch64-none-elf-nm, or any
nm that reads AArch64 ELF). Recompiled guest functions (recomp_<mod>_<VA>)
are also named from the decompilation's "// FUNCTION: <MOD> 0x<va>" comments
when --src points at it.
"""
import argparse, bisect, collections, os, re, shutil, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument("prof"); ap.add_argument("elf")
ap.add_argument("--src", help="decompilation source tree (FUNCTION: comments)")
ap.add_argument("--nm", default=None)
ap.add_argument("-n", type=int, default=40)
a = ap.parse_args()

nm = a.nm or next((x for x in ("aarch64-none-elf-nm", "llvm-nm", "nm") if shutil.which(x)), "nm")
syms = []
for line in subprocess.run([nm, "-n", "--defined-only", a.elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) >= 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [s[0] for s in syms]
anchor_elf = next(v for v, n in syms if n == "familia_main")

names = {}
if a.src:
    rx = re.compile(r"FUNCTION:\s*(\w+)\s+0x([0-9a-fA-F]+)")
    for root, _, files in os.walk(a.src):
        for f in files:
            if not f.endswith((".cpp", ".h", ".c")): continue
            lines = open(os.path.join(root, f), errors="replace").read().splitlines()
            for i, l in enumerate(lines):
                m = rx.search(l)
                if not m: continue
                for nxt in lines[i + 1:i + 6]:   # the definition follows the comment(s)
                    d = re.search(r"([\w:~]+)\s*\(", nxt)
                    if d and "FUNCTION:" not in nxt:
                        names[(m.group(1).lower(), int(m.group(2), 16))] = d.group(1); break

def pretty(sym):
    m = re.match(r"recomp_(\w+?)_([0-9A-Fa-f]{6,8})(_orig|_probed)?$", sym)
    if m:
        n = names.get((m.group(1).lower(), int(m.group(2), 16)))
        if n: return f"{sym}  [{n}]"
    return sym

threads, samples, slide = {}, collections.defaultdict(collections.Counter), 0
for line in open(a.prof):
    p = line.split()
    if p[0] == "anchor": slide = int(p[2], 16) - anchor_elf
    elif p[0] == "thread": threads[int(p[1])] = (p[2], int(p[4]))
    elif p[0] == "s":
        pc = int(p[2], 16) - slide
        i = bisect.bisect_right(addrs, pc) - 1
        name = syms[i][1] if i >= 0 and pc - syms[i][0] < 1 << 22 else f"?{pc:x} (outside the ELF: kernel/other module)"
        samples[int(p[1])][name] += int(p[3])

for t, (tname, total) in sorted(threads.items()):
    if not total: continue
    print(f"\n=== thread {t} {tname}: {total} samples ===")
    groups = collections.Counter()
    for name, n in samples[t].items():
        g = "recompiled game code" if name.startswith("recomp_") else \
            "libc / memcpy / malloc" if re.match(r"(mem|str|__aarch64|_?malloc|_?free|calloc|realloc|_int_)", name) else \
            "Mesa / GL driver" if re.match(r"(nv50|nvc0|nouveau|st_|_mesa|nir_|util_|u_|vbo_|glsl|gl[A-Z]|pipe_|tc_|draw_|_glapi)", name) else \
            "kernel / outside ELF" if name.startswith("?") else "runtime / other"
        groups[g] += n
    for g, n in groups.most_common():
        print(f"  {100.0 * n / total:5.1f}%  {g}")
    print("  --- top functions ---")
    for name, n in samples[t].most_common(a.n):
        print(f"  {100.0 * n / total:5.1f}%  {pretty(name)}")
