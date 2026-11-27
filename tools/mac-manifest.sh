#!/usr/bin/env bash
# Signs and uploads the update manifest of a macOS app: <NAME>.json ({version, url, sha256} of ZIP) and its binary
# detached signature <NAME>.json.sig by the release key, as andromeda-app-updater verifies them. RELEASE must serve
# releases/latest/download (the `repo` release). A manifest that already names a newer version stays.
#   tools/mac-manifest.sh RELEASE ZIP VERSION URL NAME
#   tools/mac-manifest.sh repo Andromeda-Terminal-2026.11.19-macos-arm64.zip 2026.11.19 https://.../x.zip Andromeda-Terminal-macos-arm64
# Env: GH_TOKEN, GITHUB_REPOSITORY, RELEASE_GPG_KEY.
set -euo pipefail
release=$1 zip=$2 version=$3 url=$4 name=$5
repo=${GITHUB_REPOSITORY:?}
[[ $url == https://* ]] || { echo "mac-manifest: url is not https: $url" >&2; exit 1; }
[[ $version =~ ^[0-9]+(\.[0-9]+)*$ ]] || { echo "mac-manifest: version is not dotted numbers: $version" >&2; exit 1; }
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

if gh release download "$release" -R "$repo" -p "$name.json" -D "$out/old" 2> /dev/null; then
  old=$(jq -r .version "$out/old/$name.json")
  if [[ $old != "$version" && $(printf '%s\n%s\n' "$old" "$version" | sort -V | tail -1) == "$old" ]]; then
    echo "mac-manifest: $name.json names $old, newer than $version; kept"
    exit 0
  fi
fi

echo "$RELEASE_GPG_KEY" | gpg --batch --import 2> /dev/null
key=$(gpg --list-secret-keys --with-colons | awk -F: '/^fpr/{print $10; exit}')
sha=$(sha256sum "$zip" | cut -d' ' -f1)
jq -cn --arg v "$version" --arg u "$url" --arg s "$sha" '{version: $v, url: $u, sha256: $s}' > "$out/$name.json"
gpg --batch --yes --detach-sign --no-armor -u "$key" -o "$out/$name.json.sig" "$out/$name.json"
gpg --batch --verify "$out/$name.json.sig" "$out/$name.json" 2> /dev/null
# The signature goes first: a client that reads the new manifest with the old signature refuses it and retries later.
gh release upload "$release" -R "$repo" --clobber "$out/$name.json.sig" "$out/$name.json"
echo "mac-manifest: $name.json -> $version ($sha)"
