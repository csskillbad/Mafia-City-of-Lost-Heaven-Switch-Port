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
- Rendering: **16:9 by default.** The game runs an 854x480 (16:9) display mode,
  mapped onto the whole window, so the scene is rasterised at the display's
  native resolution: **1280x720 handheld, 1920x1080 docked**, following docking
  live. `runtime/native_widescreen.c` hooks LS3D's `UpdateProjMatrix` for a Hor+
  field of view (wider view, same vertical FOV as 4:3) with culling widened to
  match; the game's own FOV value and draw distance are unchanged.
  `WIDESCREEN=0` gives the original 4:3 (960x720 / 1440x1080, pillarboxed).
- CPU: code generated for the Cortex-A57 (`-mcpu=cortex-a57+crc+crypto`), and
  threads are placed on the 3 application cores: game on core 0, the GL replay
  thread (driver work) on core 1, sound/streaming/SDL audio on core 2. libnx
  would otherwise start them all on core 0.

## Build
The easy way is the NSP builder (`build_nsp_gui.sh` / `build_nsp_gui.bat`,
[docs/SWITCH_GUIDE.md](../docs/SWITCH_GUIDE.md)): it runs the steps below in
the `familia-switch` container (`docker/Dockerfile.switch`,
`switch/container_build.sh`) and packs on the host. By hand:
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
`TEXTURES` (32/16), `CPU_CLOCK`, `STATS`, `MOVIES`. Any `FAMILIA_*` switch
from the main README works as `KEY=VALUE` here.

### Frame rate
The game's main thread is the limit, not the GPU: profiled in the city
(Free Ride), the GL/driver thread idles ~80% of the time while the game
thread is always busy. Its speed is the CPU clock, so `CPU_CLOCK=1785`
(default) asks for the Switch's own boost clock through `clkrst`/`pcv`
(re-applied after dock/undock and sleep, put back to 1020 MHz on exit).
`CPU_CLOCK=1020` or `0` keeps the normal clock (longer battery life in
handheld). Free Ride's traffic and pedestrian density sliders and the
game's view-distance option lower the game thread's work further;
`LIMIT=30` gives a steady 30 if 60 is not held.

### Profiling on the console
`PROF=<seconds after the first frame>` and `PROF_LEN=<seconds>` sample every
thread's PC each millisecond (`svcSetThreadActivity` + `svcGetThreadContext3`)
into `sd:/switch/mafia/prof.txt`:
```sh
python3 tools/nx_prof_report.py prof.txt build/switch/familia.elf --src <decompilation>
```

## Known limits
- The Bink logo (`Logo3.bik`) is skipped: devkitPro's FFmpeg has no Bink
  decoder. The `.avi` intros play.
- In 16:9 the 2D HUD and menus are laid out by the game for the wider screen;
  some full-screen 2D artwork may look stretched.
- Untested on hardware at the time of writing; see the commit log for status.
