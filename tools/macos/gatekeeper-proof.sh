#!/usr/bin/env bash
# Runs on a Mac: can a non-notarized app replace itself with a downloaded build and start it without a Gatekeeper
# prompt? Installs ZIP as a browser download would (quarantined), clears the quarantine as the user's "Open Anyway"
# would, starts the app, and has a shell inside it (a child of the app, as the updater helper will be) download ZIP
# again and unpack it with ditto. Then it quits the app, swaps the bundles with two renames and starts the new one.
# Prints the extended attributes at each stage and the Gatekeeper log; fails when the swapped app does not start.
# Usage: tools/macos/gatekeeper-proof.sh ZIP_URL [OUT]
set -euo pipefail
url=$1
out=${2:-/var/tmp/gatekeeper-proof}
name="Andromeda Terminal.app"
apps=$HOME/Applications
app=$apps/$name
exe=$app/Contents/MacOS/andromeda-terminal
stage=$HOME/Library/Caches/org.andromeda.Terminal/update
rm -rf "$out" "$app" "$app.old" "$stage"
mkdir -p "$out" "$apps"

wait_for() {
  local tries=$(($1 * 5))
  shift
  while ! "$@" > /dev/null 2>&1; do
    tries=$((tries - 1))
    [ $tries -gt 0 ] || return 1
    sleep 0.2
  done
}
attrs() { echo "== $1"; xattr -lr "$2" 2>/dev/null | grep -v '^$' | sed 's/^/   /' | sort -u | head -20 || true; }
gatekeeper_log() {
  log show --last 3m --style compact \
    --predicate 'subsystem == "com.apple.syspolicy" OR process == "syspolicyd" OR process == "CoreServicesUIAgent"' \
    > "$out/gatekeeper.log" 2>&1 || true
  grep -iE 'deny|reject|quarantine|translocat|prompt|notar' "$out/gatekeeper.log" | tail -30 || true
}
fail() {
  echo "gatekeeper-proof: FAIL: $1" >&2
  gatekeeper_log
  screencapture -x "$out/failure.png" || true
  exit 1
}
first_session() { "$exe" ctl ls | jq -er '.[0].id'; }

# 1. The user's download: Safari marks the zip, and ditto carries the mark onto every file it unpacks.
curl -fsSL -o "$out/download.zip" "$url"
xattr -w com.apple.quarantine "0083;$(printf %x "$(date +%s)");Safari;$(uuidgen)" "$out/download.zip"
ditto -x -k "$out/download.zip" "$apps"
attrs "installed, as downloaded" "$app"
# 2. "Open Anyway" in System Settings: the user approves the app once.
xattr -dr com.apple.quarantine "$app"
attrs "installed, approved" "$app"

# 3. The running app downloads and unpacks the next build itself.
open "$app"
wait_for 30 first_session || fail "the installed app did not start"
id=$(first_session)
cmd="mkdir -p '$stage' && curl -fsSL -o '$stage/next.zip' '$url' && ditto -x -k '$stage/next.zip' '$stage' && echo staged-ok"
"$exe" ctl send-text "$id" "$cmd"$'\n'
wait_for 120 sh -c '"$1" ctl get-text --extent all "$2" | grep -q "^staged-ok"' _ "$exe" "$id" || fail "the in-app download did not finish"
attrs "staged by the app" "$stage/$name"

# 4. The swap: quit, two renames on one volume, start the new bundle.
osascript -e 'tell application "Andromeda Terminal" to quit' || true
wait_for 15 sh -c '! pgrep -x andromeda-terminal' || pkill -x andromeda-terminal || true
mv "$app" "$app.old"
mv "$stage/$name" "$app"
rm -rf "$app.old"
attrs "swapped in" "$app"
spctl --assess --type execute -vv "$app" > "$out/spctl.txt" 2>&1 || true
echo "== spctl: $(tr '\n' ' ' < "$out/spctl.txt")"
open "$app"
wait_for 30 first_session || fail "the swapped app did not start (Gatekeeper block or prompt)"
screencapture -x "$out/swapped.png" || true
gatekeeper_log
osascript -e 'tell application "Andromeda Terminal" to quit' || pkill -x andromeda-terminal || true
echo "gatekeeper-proof: ok: the swapped app started without a prompt"
