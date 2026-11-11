#!/usr/bin/env bash
# Makes the iPhone attached to Windows visible to WSL tools (xtool, libimobiledevice) by relaying
# the Unix socket /var/run/usbmuxd to Apple Mobile Device Service on Windows (127.0.0.1:27015).
# Needs WSL mirrored networking (or any setup where WSL's 127.0.0.1 reaches Windows) and root.
# Usage: wsl -u root bash scripts/wsl-usbmuxd-relay.sh --user <WSL-user>   (idempotent)
set -euo pipefail
SOCK=/var/run/usbmuxd
RELAY_USER=${SUDO_USER:-${2:-}}
if [ -z "$RELAY_USER" ] || ! id "$RELAY_USER" >/dev/null 2>&1; then
  echo "usage (as root): wsl-usbmuxd-relay.sh --user <WSL-user>" >&2
  exit 2
fi
RELAY_UID=$(id -u "$RELAY_USER")
RELAY_GID=$(id -g "$RELAY_USER")
if ! timeout 3 bash -c '</dev/tcp/127.0.0.1/27015' 2>/dev/null; then
  echo "Windows usbmuxd (127.0.0.1:27015) not reachable: is the iPhone attached and Apple Mobile Device Service running?" >&2
  exit 1
fi
if [ -S "$SOCK" ] && pgrep -x socat >/dev/null; then
  chmod 600 "$SOCK"
  chown "$RELAY_UID:$RELAY_GID" "$SOCK"
  echo "relay already running (restricted to the requested user)"
else
  pgrep -x usbmuxd >/dev/null && { echo "stopping Linux usbmuxd (it would own $SOCK)"; pkill -x usbmuxd || true; }
  rm -f "$SOCK"
  umask 077
  setsid socat "UNIX-LISTEN:$SOCK,fork,mode=600,user=$RELAY_UID,group=$RELAY_GID" TCP:127.0.0.1:27015 >/dev/null 2>&1 < /dev/null &
  sleep 0.5
  echo "relay started: $SOCK -> 127.0.0.1:27015"
fi
command -v idevice_id >/dev/null && idevice_id -l || true
