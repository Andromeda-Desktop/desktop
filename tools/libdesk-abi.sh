#!/usr/bin/env bash
# Prints the libdesk ABI: a hash of every header libdesk's headers can include (src and protocols, through their
# symlinks into motion/ and protocol/), the Wayland protocol XML and the design tokens. Apps link libdesk shared and
# include its internal headers, so any such change is a new ABI. The libdesk package provides it and each app depends on it.
#   tools/libdesk-abi.sh [DIR]    hash the tree in DIR (default: this checkout)
#   tools/libdesk-abi.sh --rev REV  hash the files of commit REV
set -euo pipefail
export LC_ALL=C
if [[ ${1:-} == --rev ]]; then
  # A commit's tree, extracted: some headers are symlinks, and find hashes the file they point to.
  tree=$(mktemp -d)
  trap 'rm -rf "$tree"' EXIT
  git -C "$(dirname "$0")/.." archive "$2" -- 'libdesk-*' protocol motion | tar -x -C "$tree"
  set -- "$tree"
fi
cd "${1:-$(dirname "$0")/..}"
abi=$({ find -L libdesk-*/src libdesk-*/protocols \( -name '*.h' -o -name '*.xml' \) -print0 | sort -z |
  xargs -0 sha256sum; sha256sum protocol/design-tokens.toml; } | sha256sum | cut -c1-16)
[[ -n $abi ]]
echo "$abi"
