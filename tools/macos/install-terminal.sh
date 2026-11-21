#!/bin/sh
# Installs the newest Andromeda Terminal for macOS:
#   curl -fsSL https://raw.githubusercontent.com/Andromeda-Desktop/desktop/main/tools/macos/install-terminal.sh | sh
# curl sets no quarantine flag, so Gatekeeper does not block the unnotarized app. Pass a version (2026.11.8) to pin one.
set -eu

main() {
  repo=Andromeda-Desktop/desktop
  [ "$(uname -s)" = Darwin ] || fail "this installer is for macOS"
  [ "$(uname -m)" = arm64 ] || fail "Andromeda Terminal needs an Apple silicon Mac"
  major=$(sw_vers -productVersion | cut -d. -f1)
  [ "$major" -ge 26 ] || fail "Andromeda Terminal needs macOS 26 or later (this Mac has $(sw_vers -productVersion))"

  if [ $# -gt 0 ]; then
    url=https://github.com/$repo/releases/download/terminal-v$1/Andromeda-Terminal-$1-macos-arm64.zip
  else
    # The Latest release (tag repo) holds the newest Mac build; check-macos.yml uploads it on each terminal tag.
    url=https://github.com/$repo/releases/latest/download/Andromeda-Terminal-macos-arm64.zip
  fi

  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  echo "Downloading $url"
  curl -fL --progress-bar -o "$tmp/terminal.zip" "$url"
  ditto -x -k "$tmp/terminal.zip" "$tmp"
  [ -d "$tmp/Andromeda Terminal.app" ] || fail "the download holds no Andromeda Terminal.app"

  dest=/Applications
  [ -w "$dest" ] || { dest=$HOME/Applications; mkdir -p "$dest"; }
  osascript -e 'quit app "Andromeda Terminal"' 2>/dev/null || true
  rm -rf "$dest/Andromeda Terminal.app"
  ditto "$tmp/Andromeda Terminal.app" "$dest/Andromeda Terminal.app"
  echo "Installed $dest/Andromeda Terminal.app"
  open "$dest/Andromeda Terminal.app"
}

fail() { echo "install-terminal: $*" >&2; exit 1; }

main "$@"
