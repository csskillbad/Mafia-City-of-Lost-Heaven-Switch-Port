#!/usr/bin/env python3
"""Re-encode the game's videos for the all-in-one Switch NSP: H.265 (x265)
video and Opus sound in MKV, ~10x smaller than the original Indeo 5 / Bink
files at no visible loss (CRF 21 on sources that are themselves lossy).
The port plays name.mkv before name.avi / name.bik (runtime/movie.c).

    python3 tools/encode_videos.py GAME_DIR [OUT_DIR] [--preset placebo] [--crf 21]

Needs ffmpeg with libx265 and libopus on this PC. Files already encoded
from the same source (same size and time) are kept.
"""
import argparse, json, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VIDEOS = ("logo1", "logo3", "logo2", "intro")      # what Game.exe plays (PlayLogo)


def ffmpeg_ok():
    ff = shutil.which("ffmpeg")
    if not ff:
        return None
    enc = subprocess.run([ff, "-hide_banner", "-encoders"], capture_output=True, text=True).stdout
    return ff if "libx265" in enc and "libopus" in enc else None


def sources(game):
    """name -> the file the game would play: name.avi, else name.bik (any case)"""
    by = {p.name.lower(): p for p in Path(game).iterdir() if p.is_file()}
    out = {}
    for n in VIDEOS:
        for ext in ("avi", "bik"):
            if f"{n}.{ext}" in by:
                out[n] = by[f"{n}.{ext}"]
                break
    return out


def encode(game, out_dir, preset="placebo", crf=21, log=print):
    ff = ffmpeg_ok()
    if not ff:
        log("videos: ffmpeg with libx265 + libopus not found, the original videos are used")
        return {}
    out_dir = Path(out_dir); out_dir.mkdir(parents=True, exist_ok=True)
    done = {}
    for name, src in sources(game).items():
        dst = out_dir / f"{src.stem}.mkv"
        stamp = out_dir / f"{src.stem}.src.json"
        key = {"src": src.name, "size": src.stat().st_size, "mtime": int(src.stat().st_mtime), "preset": preset, "crf": crf}
        if dst.exists() and stamp.exists() and json.loads(stamp.read_text()) == key:
            done[src.name] = dst
            continue
        has_audio = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a", "-show_entries", "stream=index",
                                    "-of", "csv=p=0", str(src)], capture_output=True, text=True).stdout.strip()
        log(f"videos: {src.name} -> H.265 ({preset}, CRF {crf}), this can take a while")
        tmp = dst.with_suffix(".part.mkv")
        cmd = [ff, "-hide_banner", "-loglevel", "error", "-y", "-i", str(src), "-map", "0:v:0",
               "-c:v", "libx265", "-preset", preset, "-crf", str(crf), "-pix_fmt", "yuv420p",
               "-x265-params", "log-level=error"]
        if has_audio:
            cmd += ["-map", "0:a:0", "-c:a", "libopus", "-b:a", "112k", "-ac", "2"]
        r = subprocess.run(cmd + [str(tmp)])
        if r.returncode or not tmp.exists():
            log(f"videos: {src.name} failed, the original is used")
            tmp.unlink(missing_ok=True)
            continue
        tmp.replace(dst)
        stamp.write_text(json.dumps(key))
        log(f"videos: {src.name} {src.stat().st_size / 2**20:.1f} MB -> {dst.stat().st_size / 2**20:.1f} MB")
        done[src.name] = dst
    return done


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("game"); ap.add_argument("out", nargs="?", default=str(ROOT / "build/videos"))
    ap.add_argument("--preset", default="placebo"); ap.add_argument("--crf", type=int, default=21)
    a = ap.parse_args()
    r = encode(a.game, a.out, a.preset, a.crf)
    sys.exit(0 if r else 1)
