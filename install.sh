#!/bin/sh
# Installs frame-finch into ~/.local/bin and its agent skill into ~/.claude/skills/frame-finch.
# Fetches ffmpeg and ffprobe as static builds on Linux when they are missing (SteamOS can't install packages).
#
#   curl -fsSL drstrider.github.io/frame-finch/install.sh | sh
#
# FRAME_FINCH_BIN=/some/dir puts the binary somewhere else.
set -eu

REPO=drstrider/frame-finch
BIN="${FRAME_FINCH_BIN:-$HOME/.local/bin}"
SKILL="$HOME/.claude/skills/frame-finch"

say() { printf 'frame-finch: %s\n' "$*"; }
die() { say "$*" >&2; exit 1; }

case "$(uname -s)" in
    Linux) os=linux ;;
    Darwin) os=macos ;;
    *) die "no prebuilt binary for $(uname -s): clone https://github.com/$REPO and run make" ;;
esac
case "$(uname -m)" in
    x86_64|amd64) arch=x86_64 ;;
    aarch64|arm64) arch=arm64 ;;
    *) die "no prebuilt binary for $(uname -m): clone https://github.com/$REPO and run make" ;;
esac
[ "$os-$arch" = macos-x86_64 ] && die "no prebuilt binary for Intel Macs: clone https://github.com/$REPO and run make"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fetch() {   # a file from the latest release -> $tmp/name
    curl -fsSL -o "$tmp/$1" "https://github.com/$REPO/releases/latest/download/$1" || die "can't download $1"
}

mkdir -p "$BIN" "$SKILL"
fetch "frame-finch-$os-$arch"
install -m 755 "$tmp/frame-finch-$os-$arch" "$BIN/frame-finch"
fetch SKILL.md
install -m 644 "$tmp/SKILL.md" "$SKILL/SKILL.md"
say "installed $BIN/frame-finch and the skill in $SKILL"

if ! command -v ffmpeg >/dev/null 2>&1 || ! command -v ffprobe >/dev/null 2>&1; then
    if [ "$os" = linux ]; then
        say "ffmpeg is missing: fetching a static build into $BIN"
        a=amd64; [ "$arch" = arm64 ] && a=arm64
        curl -fsSL "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-$a-static.tar.xz" | tar -xJ -C "$tmp"
        for t in ffmpeg ffprobe; do install -m 755 "$tmp"/ffmpeg-*-static/$t "$BIN/$t"; done
    else
        say "ffmpeg is missing: install it with 'brew install ffmpeg'"
    fi
fi

case ":$PATH:" in
    *":$BIN:"*) ;;
    *) say "add $BIN to your PATH (for example: echo 'export PATH=\"$BIN:\$PATH\"' >> ~/.profile)" ;;
esac
"$BIN/frame-finch" 2>&1 | grep -q usage && say "ready: frame-finch VIDEO.mp4"
