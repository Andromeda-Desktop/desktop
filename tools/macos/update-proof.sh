#!/usr/bin/env bash
# Runs on a Mac: proves the self-update of a built Terminal.app with a proof build of andromeda-app-updater (it trusts
# a throwaway key and http://127.0.0.1). Release 1.0.0 is installed, 1.0.1 is served. A tampered zip and a manifest
# changed after signing are refused and leave no ready marker; the signed 1.0.1 stages, swaps in with one APFS
# exchange after the app exits, and starts; a stage left ready is found again without a download.
# Usage: tools/macos/update-proof.sh [APP]
set -euo pipefail
cd "$(dirname "$0")/../.."
src=${1:-/var/tmp/andromeda-macos/Andromeda Terminal.app}
work=$(mktemp -d /var/tmp/update-proof.XXXXXX)
server=
cleanup() {
  [ -z "$server" ] || kill "$server" 2> /dev/null || true
  pkill -f "$work/Applications/Andromeda Terminal.app/Contents/MacOS/andromeda-terminal" 2> /dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT
fail() {
  echo "update-proof: FAIL: $*" >&2
  exit 1
}
# Runs the command every 0.2 s until it succeeds or SECONDS pass.
wait_for() {
  local tries=$(($1 * 5))
  shift
  while ! "$@" > /dev/null 2>&1; do
    tries=$((tries - 1))
    [ $tries -gt 0 ] || return 1
    sleep 0.2
  done
}

cargo build -q --release --locked --manifest-path system/Cargo.toml -p andromeda-app-updater --features proof \
  --target-dir /var/tmp/update-proof-cargo
proof_helper=/var/tmp/update-proof-cargo/release/andromeda-app-updater

# release VERSION DIR: a copy of the app that claims VERSION and holds the proof helper.
release() {
  local app="$2/Andromeda Terminal.app"
  mkdir -p "$2"
  ditto "$src" "$app"
  plutil -replace CFBundleShortVersionString -string "$1" "$app/Contents/Info.plist"
  cp "$proof_helper" "$app/Contents/MacOS/andromeda-app-updater"
  codesign --force --deep --sign - "$app"
}
version() { plutil -extract CFBundleShortVersionString raw -o - "$1/Contents/Info.plist"; }

apps=$work/Applications
app="$apps/Andromeda Terminal.app"
stage="$apps/.Andromeda Terminal.app.update"
srv=$work/srv
mkdir -p "$srv"
release 1.0.0 "$apps"
release 1.0.1 "$work/next"
(cd "$work/next" && ditto -c -k --keepParent "Andromeda Terminal.app" "$srv/next.zip")
cp "$srv/next.zip" "$srv/bad.zip"
printf x >> "$srv/bad.zip"
sha=$(shasum -a 256 "$srv/next.zip" | cut -d' ' -f1)

export GNUPGHOME=$work/gpg
mkdir -m 700 "$GNUPGHOME"
gpg --batch --pinentry-mode loopback --passphrase '' --quick-gen-key 'Proof <proof@invalid>' ed25519 sign never 2> /dev/null
gpg --armor --export proof@invalid > "$work/key.asc"
export ANDROMEDA_UPDATER_PROOF_KEY=$work/key.asc

port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
python3 -m http.server -b 127.0.0.1 -d "$srv" "$port" > /dev/null 2>&1 &
server=$!
base=http://127.0.0.1:$port
wait_for 10 curl -fs "$base/" || fail "the test server did not start"

# manifest NAME ZIP: NAME.json names ZIP with the sha256 of the good zip, signed by the proof key.
manifest() {
  jq -cn --arg u "$base/$2" --arg s "$sha" '{version: "1.0.1", url: $u, sha256: $s}' > "$srv/$1.json"
  gpg --batch --yes --detach-sign --no-armor -o "$srv/$1.json.sig" "$srv/$1.json"
}
check() { "$app/Contents/MacOS/andromeda-app-updater" check "$base/$1.json"; }

manifest bad bad.zip
if out=$(check bad); then fail "a tampered zip was accepted: $out"; fi
[ ! -e "$stage/ready" ] || fail "a tampered zip left a ready marker"

manifest good next.zip
sed 's/1\.0\.1/1.0.2/' "$srv/good.json" > "$srv/edited.json"
cp "$srv/good.json.sig" "$srv/edited.json.sig"
if out=$(check edited); then fail "a manifest changed after signing was accepted: $out"; fi
[ "$(version "$app")" = 1.0.0 ] || fail "a refused update changed the app"

out=$(check good)
[ "$out" = "ready 1.0.1" ] || fail "the signed release did not stage: $out"
kill "$server"
server=
out=$(check good)
[ "$out" = "ready 1.0.1" ] || fail "a ready stage was not found again without the server: $out"

# A stand-in for the running app that is not this shell's child (a zombie would look alive): the swap waits for it.
pid=$(sh -c 'sleep 2 > /dev/null 2>&1 & echo $!')
"$app/Contents/MacOS/andromeda-app-updater" swap "$pid" reopen || fail "the swap failed"
[ "$(version "$app")" = 1.0.1 ] || fail "the app is $(version "$app") after the swap"
[ ! -e "$stage/ready" ] && [ ! -e "$stage/Andromeda Terminal.app" ] || fail "the swap left the stage behind"
codesign --verify --deep --strict "$app" || fail "the swapped app is not a whole signed bundle"
wait_for 30 pgrep -f "$app/Contents/MacOS/andromeda-terminal" || fail "the swapped app did not start"
echo "update-proof: ok (refused a tampered zip and an edited manifest; 1.0.0 -> 1.0.1 swapped and started)"
