#!/usr/bin/env bash
# Package build/switch/familia.elf as a standalone Switch Application NSP.
# Host script: stages ExeFS/Control in the devkitPro container, then packs and
# signs with hacBrewPack on the host. Contains no game data.
#
#   PROD_KEYS=/path/to/prod.keys bash switch/make_nsp.sh
#
# FULL=1 GAME_DIR=/path/to/Mafia: a personal all-in-one NSP with your game
# files inside (romfs:/data, read when sd:/switch/mafia/data is absent).
# Output build/switch/Mafia_full.nsp. It contains the game: for your own
# console only, never share or upload it.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
img="${DEVKITPRO_IMAGE:-docker.io/devkitpro/devkita64:20251117}"
keys="${PROD_KEYS:?set PROD_KEYS=/path/to/your/prod.keys}"
hbp="${HACBREWPACK:-$(command -v hacbrewpack || echo "$HOME/.local/bin/hacbrewpack")}"
version="${APP_VERSION:-0.1.0}"
stage=build/switch/nsp_stage
out=build/switch/Mafia.nsp
full="${FULL:-0}"
[ "$full" = 1 ] && out=build/switch/Mafia_full.nsp

elf="${ELF:-build/switch/familia.elf}"
[ -f "$elf" ] || { echo "$elf missing: build first" >&2; exit 2; }
[ -f "$keys" ] || { echo "prod.keys not found: $keys" >&2; exit 2; }
[ -x "$hbp" ] || { echo "hacbrewpack not found" >&2; exit 2; }

mkdir -p "$stage"
podman run --rm --security-opt label=disable -v "$root:/src" -w /src "$img" bash -lc "
  set -e
  export PATH=/opt/devkitpro/tools/bin:\$PATH
  s=$stage
  mkdir -p \$s/exefs \$s/control \$s/romfs \$s/nsp_out \$s/nca_tmp
  elf2nso $elf \$s/exefs/main
  npdmtool switch/main.npdm.json \$s/exefs/main.npdm
  nacptool --create 'Mafia' 'The Familia (Switch port)' '$version' \$s/control/control.nacp --titleid=0100d3d001420000
  cp switch/icon.jpg \$s/control/icon_AmericanEnglish.dat
  cp -r switch/romfs/. \$s/romfs/
  printf 'Game data is read from sdmc:/switch/mafia/data\n' > \$s/romfs/README.txt
"

# Game files for FULL=1 (hard links when on the same filesystem, else copies);
# a plain build must not pick up a previous FULL build's data.
rm -rf "$stage/romfs/data"
if [ "$full" = 1 ]; then
    gd="${GAME_DIR:?FULL=1 needs GAME_DIR=/path/to/your/Mafia folder}"
    [ -e "$gd/Game.exe" ] || [ -e "$gd/game.exe" ] || { echo "no Game.exe in $gd" >&2; exit 2; }
    mkdir -p "$stage/romfs/data"
    for f in "$gd"/*; do
        r="$(readlink -f "$f")"; n="$(basename "$f")"
        cp -al "$r" "$stage/romfs/data/$n" 2>/dev/null || cp -rL "$r" "$stage/romfs/data/$n"
    done
    echo "game files in RomFS: $(du -shL "$stage/romfs/data" | cut -f1)"
fi

# Temporary key file: only plain 32/64-hex entries (hacBrewPack 3.05 rejects
# newer key formats). Never printed, deleted on exit.
tmpdir="$(mktemp -d)"
trap 'rm -f "$tmpdir/keys"; rmdir "$tmpdir" 2>/dev/null || true' EXIT
umask 077
awk -F'=' '{v=$2; gsub(/[ \t\r]/,"",v); l=length(v); if ((l==32||l==64) && v ~ /^[0-9a-fA-F]+$/) print}' "$keys" > "$tmpdir/keys"

(cd "$tmpdir" && "$hbp" -k "$tmpdir/keys" \
    --exefsdir "$root/$stage/exefs" --romfsdir "$root/$stage/romfs" \
    --controldir "$root/$stage/control" --tempdir "$root/$stage/hac_tmp" \
    --ncadir "$root/$stage/nca_tmp" --nspdir "$root/$stage/nsp_out" --nologo >/dev/null)
nsp="$(find "$stage/nsp_out" -maxdepth 1 -name '*.nsp' | head -n1)"
[ -n "$nsp" ] || { echo "hacBrewPack produced no NSP" >&2; exit 1; }
cp -f "$nsp" "$out"
chmod 644 "$out"
ls -lh "$out"
sha256sum "$out"
