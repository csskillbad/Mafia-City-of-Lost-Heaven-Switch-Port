#!/usr/bin/env bash
# Runs inside the familia-switch image (docker/Dockerfile.switch), started by
# tools/nsp_builder.py or by hand:
#
#   docker run --rm -v "$PWD:/src" -v /path/to/Mafia:/game:ro \
#       -v /path/to/pcrecomp:/pcrecomp:ro -e JOBS=2 familia-switch:1 \
#       bash switch/container_build.sh
#
# 1. translate your Game.exe + DLLs to C (build/gen), skipped when build/gen
#    was made from the same files
# 2. compile for the Switch (build/switch/familia.elf)
# 3. stage ExeFS/Control/RomFS for hacBrewPack (build/switch/nsp_stage)
# Packing and signing happen on the host (your prod.keys never enter the
# container). Lines starting with @@ are progress markers for the GUI.
set -euo pipefail
cd /src
export PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:$PATH
JOBS="${JOBS:-2}"
GAME="${GAME:-/game}"
export PCRECOMP="${PCRECOMP:-/pcrecomp}"
VERSION="${APP_VERSION:-0.1.0}"

step() { echo "@@STEP $1 $2"; }

[ -f "$PCRECOMP/tools/lift/lift32.py" ] || { echo "@@ERROR PCRecomp not found at $PCRECOMP (git submodule update --init)"; exit 2; }
exe=""
for n in Game.exe game.exe GAME.EXE; do [ -f "$GAME/$n" ] && exe="$GAME/$n" && break; done
[ -n "$exe" ] || { echo "@@ERROR no Game.exe in the game folder"; exit 2; }

# ---- 1. translate -------------------------------------------------------
step 1 translate
sum="$(sha256sum "$exe" | cut -d' ' -f1)"
if [ -f build/gen/modules.json ] && grep -q "$sum" build/gen/modules.json && [ "${RETRANSLATE:-0}" != 1 ]; then
    echo "build/gen already translated from this Game.exe ($sum): reusing it"
else
    echo "Game.exe sha256 $sum"
    rm -rf build/gen build/rebased
    python3 -P tools/translate.py "$GAME" --out build/gen
fi

# ---- 2. compile -----------------------------------------------------------
step 2 compile
todo="$(make -f switch/Makefile -n 2>/dev/null | grep -c -- ' -c ' || true)"
echo "@@TOTAL $todo"
make -f switch/Makefile -j"$JOBS" 2>&1 | while IFS= read -r l; do
    case "$l" in
        *" -c "*) echo "@@TICK ${l##* -o }" ;;
        *) echo "$l" ;;
    esac
done
[ "${PIPESTATUS[0]}" = 0 ] || { echo "@@ERROR compile failed"; exit 1; }

# ---- 3. stage -------------------------------------------------------------
step 3 stage
s=build/switch/nsp_stage
rm -rf "$s"; mkdir -p "$s/exefs" "$s/control" "$s/romfs"
elf2nso build/switch/familia.elf "$s/exefs/main"
npdmtool switch/main.npdm.json "$s/exefs/main.npdm"
nacptool --create 'Mafia' 'The Familia (Switch port)' "$VERSION" "$s/control/control.nacp" --titleid=0100d3d001420000
cp switch/icon.jpg "$s/control/icon_AmericanEnglish.dat"
cp -r switch/romfs/. "$s/romfs/"
echo "@@DONE $s"
