#!/usr/bin/env bash
# The Familia - Switch NSP builder (Linux). Needs Python 3 with Tkinter and
# Docker or Podman.
cd "$(dirname "$(readlink -f "$0")")"
python3 -c "import tkinter" 2>/dev/null || {
    echo "Tkinter is missing: install python3-tk (Debian/Ubuntu) or python3-tkinter (Fedora)."; exit 1; }
exec python3 tools/nsp_builder.py "$@"
