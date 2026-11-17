#!/usr/bin/env bash
# The version of a release unit (core, or an app: files, player, viewer, terminal); never written by hand.
# A release build sets ANDROMEDA_VERSION from its tag. Any other build is the unit's last tag plus the commit count,
# which sorts above that release and below the next one (pacman and apt): 2026.11.1.r15800.
#   tools/unit-version.sh UNIT        print the version
#   tools/unit-version.sh UNIT next   print the next release version: YYYY.M.N, N counts the unit's tags this month
set -euo pipefail
unit=$1
cd "$(dirname "$0")/.."
prefix=$([[ $unit == core ]] && echo v || echo "$unit-v")
if [[ ${2:-} == next ]]; then
  month=$(date -u +%Y.%-m)
  n=$(git tag -l "$prefix$month.*" | wc -l)
  echo "$month.$((n + 1))"
  exit 0
fi
if [[ -n ${ANDROMEDA_VERSION:-} ]]; then
  echo "$ANDROMEDA_VERSION"
  exit 0
fi
# Only date versions count: the older hand-picked tags (v0.2.x, terminal-v0.1.x) sort below them.
if tag=$(git describe --tags --abbrev=0 --match "$prefix[0-9][0-9][0-9][0-9].*" 2> /dev/null); then
  echo "${tag#"$prefix"}.r$(git rev-list --count HEAD)"
elif count=$(git rev-list --count HEAD 2> /dev/null); then
  echo "0.r$count"
else
  echo 0
fi
