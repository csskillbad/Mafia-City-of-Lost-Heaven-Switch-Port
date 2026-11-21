#!/usr/bin/env python3
"""Compress the game's loose sounds (sounds/*.wav, ~1 GB of 16-bit PCM) to
Opus for the all-in-one Switch NSP: ~4x smaller, transparent at these rates
(80 kbit/s mono, 128 kbit/s stereo, for 22 kHz sources).

The port turns name.opus back into the exact WAV the game asked for when it
opens sounds/name.wav (switch/nx_opusfs.c): same sample rate, channels, bits
and length, stored in the Opus file's FAMILIA_WAV tag. Files under 16 KB and
8-bit ones stay WAV (no gain).

    python3 tools/encode_sounds.py GAME_DIR [OUT_DIR] [--jobs N]

Needs ffmpeg with libopus. OUT_DIR/sounds/<lowercase name>.opus; a file whose
source is unchanged is kept.
"""
import argparse, json, os, shutil, struct, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MIN_SIZE = 16384


def wav_info(p):
    """(rate, channels, bits, frames) of a plain PCM WAV, else None"""
    b = p.read_bytes()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        return None
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid, sz = b[pos:pos + 4], struct.unpack("<I", b[pos + 4:pos + 8])[0]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", b[pos + 8:pos + 24])
        elif cid == b"data":
            data = min(sz, len(b) - pos - 8)
        pos += 8 + sz + (sz & 1)
    if not fmt or data is None or fmt[0] != 1 or fmt[5] != 16 or fmt[1] not in (1, 2):
        return None
    return fmt[2], fmt[1], fmt[5], data // (fmt[1] * 2)


def encode(game, out_dir, jobs=None, log=print):
    ff = shutil.which("ffmpeg")
    if not ff or "libopus" not in subprocess.run([ff, "-hide_banner", "-encoders"], capture_output=True, text=True).stdout:
        log("sounds: ffmpeg with libopus not found, the WAV files are used")
        return {}
    src_dir = next((p for p in Path(game).iterdir() if p.is_dir() and p.name.lower() == "sounds"), None)
    if not src_dir:
        return {}
    out = Path(out_dir) / "sounds"
    out.mkdir(parents=True, exist_ok=True)
    stamp_file = Path(out_dir) / "sounds.json"
    stamps = json.loads(stamp_file.read_text()) if stamp_file.exists() else {}
    wavs = sorted(p for p in src_dir.iterdir() if p.is_file() and p.suffix.lower() == ".wav" and p.stat().st_size >= MIN_SIZE)
    log(f"sounds: {len(wavs)} WAV files to Opus")
    done, counter = {}, [0]

    def one(p):
        dst = out / (p.stem.lower() + ".opus")
        key = [p.stat().st_size, int(p.stat().st_mtime)]
        if dst.exists() and stamps.get(p.name) == key:
            return p, dst, key
        info = wav_info(p)
        if not info:
            return p, None, None
        rate, ch, bits, frames = info
        tmp = dst.with_suffix(".part.opus")
        r = subprocess.run([ff, "-hide_banner", "-loglevel", "error", "-y", "-i", str(p),
                            "-c:a", "libopus", "-b:a", "80k" if ch == 1 else "128k", "-vbr", "on",
                            "-compression_level", "10", "-application", "audio",
                            "-metadata", f"FAMILIA_WAV={rate},{ch},{bits},{frames}",
                            "-f", "ogg", str(tmp)])
        if r.returncode or not tmp.exists():
            tmp.unlink(missing_ok=True)
            return p, None, None
        tmp.replace(dst)
        return p, dst, key

    with ThreadPoolExecutor(jobs or os.cpu_count() or 4) as ex:
        for p, dst, key in ex.map(one, wavs):
            counter[0] += 1
            if dst:
                done[p.name] = dst
                stamps[p.name] = key
            if counter[0] % 500 == 0:
                log(f"sounds: {counter[0]}/{len(wavs)}")
    stamp_file.write_text(json.dumps(stamps))
    a = sum(p.stat().st_size for p in wavs if p.name in done)
    b = sum(d.stat().st_size for d in done.values())
    log(f"sounds: {len(done)} files, {a / 2**20:.0f} MB -> {b / 2**20:.0f} MB")
    return done


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("game"); ap.add_argument("out", nargs="?", default=str(ROOT / "build/compressed"))
    ap.add_argument("--jobs", type=int)
    a = ap.parse_args()
    sys.exit(0 if encode(a.game, a.out, a.jobs) else 1)
