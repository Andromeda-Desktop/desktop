#!/usr/bin/env bash
# Checks that one app (files, terminal, player, viewer) can release alone from commit REV: the newest libdesk that each
# repository serves must provide the libdesk ABI of REV (tools/libdesk-abi.sh). Prints nothing and exits 0 when all do;
# else prints the first index that does not and exits 1.
#   tools/app-release-gate.sh [REV]    (needs gh with read access to the repository's releases)
set -euo pipefail
rev=${1:-HEAD}
cd "$(dirname "$0")/.."
abi=$(tools/libdesk-abi.sh --rev "$rev")
repo=${GITHUB_REPOSITORY:-Andromeda-Desktop/desktop}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

gh release download repo -R "$repo" -p andromeda.db.tar.gz -D "$tmp"
desc=$(tar -tzf "$tmp/andromeda.db.tar.gz" | grep -E '^andromeda-libdesk-git-[^/]+/desc$' || true)
[[ -n $desc ]] || { echo "the Arch repository has no andromeda-libdesk-git"; exit 1; }
served=$(tar -xOzf "$tmp/andromeda.db.tar.gz" "$desc" | awk '/^%PROVIDES%$/ { p = 1; next } /^%/ { p = 0 } p && /^andromeda-libdesk-abi=/ { sub(/.*=/, ""); print }')
[[ $served == "$abi" ]] || { echo "the Arch repository's libdesk has ABI ${served:-none}, $rev has $abi"; exit 1; }

# apt keeps several versions (--multiversion); an upgrade installs the newest, so that one must provide the ABI.
for release in repo debian; do
  gh release download "$release" -R "$repo" -p Packages -O "$tmp/Packages-$release" --clobber
  served=$(awk -v RS= -v ORS='\n\n' '/(^|\n)Package: andromeda-libdesk\n/' "$tmp/Packages-$release" |
    awk '/^Version:/ { v = $2 } /^Provides:/ { p = $0 } /^$/ { if (v) print v, p; v = p = "" } END { if (v) print v, p }' |
    sort -V | tail -n 1)
  [[ -n $served ]] || { echo "the $release apt repository has no andromeda-libdesk"; exit 1; }
  [[ " ${served//,/ } " == *" andromeda-libdesk-abi-$abi "* ]] ||
    { echo "the $release apt repository's libdesk ${served%% *} does not provide andromeda-libdesk-abi-$abi"; exit 1; }
done
