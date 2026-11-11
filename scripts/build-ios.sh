#!/usr/bin/env bash
# Keep paths/arguments out of shell command strings when called through wsl.exe.
set -euo pipefail
. "$HOME/.local/share/swiftly/env.sh"
exec "$HOME/bin/xtool" "$@"
