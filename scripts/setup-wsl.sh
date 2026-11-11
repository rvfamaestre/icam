#!/usr/bin/env bash
# One-time WSL (Ubuntu 24.04) setup for building the iPhone app: system packages (root), Swift via
# swiftly and xtool (user). Re-runnable.
#   wsl -u root bash scripts/setup-wsl.sh root
#   wsl bash scripts/setup-wsl.sh user
set -euo pipefail
XTOOL_VERSION=1.24.0
XTOOL_SHA256=dbfd9d7216f077c84c171b688c4173c37c23ca76bf7fd185d1a9ae8a53dab59f   # xtool-x86_64.AppImage
SWIFTLY_VERSION=1.1.2
SWIFT_VERSION=6.4.2

case "${1:-}" in
root)
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq binutils git gnupg2 libc6-dev libcurl4-openssl-dev libedit2 libgcc-13-dev libncurses-dev \
    libpython3-dev libsqlite3-0 libstdc++-13-dev libxml2-dev libz3-dev pkg-config tzdata unzip zlib1g-dev curl \
    ca-certificates usbmuxd libimobiledevice-utils socat libfuse2t64 zip
  echo "root setup done"
  ;;
user)
  cd ~
  if [ ! -x ~/.local/share/swiftly/bin/swiftly ]; then
    BOOTSTRAP=$(mktemp -d)
    trap 'rm -rf -- "$BOOTSTRAP"' EXIT
    mkdir -m 700 "$BOOTSTRAP/gnupg"
    ARCHIVE="swiftly-$SWIFTLY_VERSION-$(uname -m).tar.gz"
    curl --compressed -fsSL "https://download.swift.org/swiftly/linux/$ARCHIVE" -o "$BOOTSTRAP/$ARCHIVE"
    curl --compressed -fsSL "https://download.swift.org/swiftly/linux/$ARCHIVE.sig" -o "$BOOTSTRAP/$ARCHIVE.sig"
    curl --compressed -fsSL https://www.swift.org/keys/all-keys.asc -o "$BOOTSTRAP/swift-keys.asc"
    gpg --homedir "$BOOTSTRAP/gnupg" --batch --import "$BOOTSTRAP/swift-keys.asc"
    gpg --homedir "$BOOTSTRAP/gnupg" --batch --verify "$BOOTSTRAP/$ARCHIVE.sig" "$BOOTSTRAP/$ARCHIVE"
    tar zxf "$BOOTSTRAP/$ARCHIVE" -C "$BOOTSTRAP"
    "$BOOTSTRAP/swiftly" init --quiet-shell-followup --assume-yes --skip-install
  fi
  . ~/.local/share/swiftly/env.sh
  swiftly install --use "$SWIFT_VERSION" --assume-yes --verify
  mkdir -p ~/bin
  if ! echo "$XTOOL_SHA256  $HOME/bin/xtool.AppImage" | sha256sum -c - >/dev/null 2>&1; then
    curl -fsSL -o ~/bin/xtool.AppImage \
      "https://github.com/xtool-org/xtool/releases/download/$XTOOL_VERSION/xtool-x86_64.AppImage"
    echo "$XTOOL_SHA256  $HOME/bin/xtool.AppImage" | sha256sum -c -
    chmod +x ~/bin/xtool.AppImage
  fi
  ln -sf ~/bin/xtool.AppImage ~/bin/xtool
  hash -r
  swift --version | head -1
  ~/bin/xtool --version
  ;;
*)
  echo "usage: setup-wsl.sh root|user" >&2
  exit 2
  ;;
esac
