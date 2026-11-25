#!/bin/sh
# Burnbot Pad installer for macOS. Needs nothing beforehand: it gets uv (github.com/astral-sh/uv),
# which brings its own Python, for this user only (no admin, no Command Line Tools).
cd "$(dirname "$0")"
UV="$HOME/.local/bin/uv"
[ -x "$UV" ] || { echo "Getting uv, which brings Python ..."; curl -LsSf https://astral.sh/uv/install.sh | sh; }
"$UV" run --no-project --python 3.12 install.py "$@"
echo; printf "Press Enter to close."; read x
