# The Familia for Nintendo Switch

A native Switch (libnx) build of The Familia: Mafia 1.2's own x86 code,
statically recompiled to ARM64 C, with the Windows / Direct3D 8 / DirectInput /
DirectSound layer answered by SDL2 + OpenGL ES (Mesa) on Horizon OS.
**Contains no game files.** You need your own copy of Mafia 1.2.

## What runs where
- `switch/nx_platform.c`: entry point, SD-card layout, `config.ini`, CPU boost,
  stubs for Linux-only diagnostics.
- `third_party/pcrecomp/win32hle/guest_mem.c` (`__SWITCH__`): the guest's 4 GB
  address space is a reservation in the process's alias region; pages are
  backed on demand with `svcMapPhysicalMemory` (needs the NPDM's system
  resource, so the **NSP is the supported format**).
- `kernel32_ext.c` (`__SWITCH__`): file mappings are read into memory (no mmap).
- Rendering: the game's 640x480 back buffer is mapped onto a centred 4:3
  rectangle of the window, so the scene is rasterised at the display's native
  resolution: **960x720 in handheld, 1440x1080 docked** (4:3, pillarboxed), and
  it follows docking live.

## Build
```sh
# 1. translate your own Mafia 1.2 executables (see docs/BUILDING.md)
PCRECOMP=/path/to/pcrecomp python3 -P tools/translate.py /path/to/game
# 2. compile for Switch (devkitPro container)
podman run --rm -v "$PWD:/src" -w /src docker.io/devkitpro/devkita64:20251117 \
    bash -lc 'make -f switch/Makefile -j$(nproc)'
# 3. NSP (signs with your console's prod.keys; never stored)
PROD_KEYS=/path/to/prod.keys bash switch/make_nsp.sh
```
Outputs: `build/switch/familia.nro`, `build/switch/Mafia.nsp`.

## Install
1. Install `Mafia.nsp` with your installer of choice.
2. Copy your Mafia 1.2 game folder's files to `sd:/switch/mafia/data/`
   (`Game.exe`, `LS3DF.dll`, `rw_data.dll`, `IJoy.dll`, every `*.dta`, the
   `.avi`/`.bik` videos, `sounds/`, ...). Game.exe must be an unprotected 1.2 build.
3. Start Mafia from the HOME menu. Saves and settings go to
   `sd:/switch/mafia/userdata/`, the log of the last run to `sd:/switch/mafia/log.txt`.

## Settings: `sd:/switch/mafia/config.ini`
Created on first run. `LIMIT` (60/30/0), `VSYNC`, `FPS` counter, `LOOK_SENS`,
`TEXTURES` (32/16), `CPU_BOOST`, `STATS`, `MOVIES`. Any `FAMILIA_*` switch
from the main README works as `KEY=VALUE` here.

## Known limits
- The Bink logo (`Logo3.bik`) is skipped: devkitPro's FFmpeg has no Bink
  decoder. The `.avi` intros play.
- 4:3 only (the game's HUD and FOV are 4:3).
- Untested on hardware at the time of writing; see the commit log for status.
