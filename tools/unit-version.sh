#!/usr/bin/env bash
# The version of a release unit (core, or an app: files, player, viewer, terminal); never written by hand.
# A release build sets ANDROMEDA_VERSION from its tag. Any other build is the unit's last tag plus the commit count,
# which sorts above that release and below the next one (pacman and apt): 2026.11.1.r15800.
#   tools/unit-version.sh UNIT        print the version
#   tools/unit-version.sh UNIT next   print the next release version: YYYY.M.N, N counts the unit's tags this month
#                                     (the core and its units count together)
set -euo pipefail
unit=$1
cd "$(dirname "$0")/.."
prefix=$([[ $unit == core ]] && echo v || echo "$unit-v")
# The core's packages release alone too (release-<unit>.yml); they share the core's counter, so each release sorts above
# every core and unit release before it (pacman and apt compare the numbers, never the tag names).
core_units="compositor shell portal greeter preview system greetd power"
# libdesk and its appd module apps release together from one commit with one version (release-apps.yml, tag
# libdesk-v*): the apps link libdesk shared and pin its exact version.
libdesk_group="libdesk files terminal player viewer link-app"
if [[ " $libdesk_group " == *" $unit "* ]]; then
  unit=libdesk prefix=libdesk-v
fi
if [[ ${2:-} == next ]]; then
  month=$(date -u +%Y.%-m)
  if [[ $unit == libdesk ]]; then
    # One above the highest of the group this month, so each package's version only grows.
    n=$(for u in $libdesk_group; do git tag -l "$u-v$month.*"; done | sed 's/.*\.//' | sort -n | tail -n 1)
    echo "$month.$((${n:-0} + 1))"
    exit 0
  fi
  if [[ $unit == core || " $core_units " == *" $unit "* ]]; then
    n=$({ git tag -l "v$month.*"; for u in $core_units; do git tag -l "$u-v$month.*"; done; } | wc -l)
  else
    n=$(git tag -l "$prefix$month.*" | wc -l)
  fi
  echo "$month.$((n + 1))"
  exit 0
fi
if [[ -n ${ANDROMEDA_VERSION:-} ]]; then
  echo "$ANDROMEDA_VERSION"
  exit 0
fi
# Only date versions count: the older hand-picked tags (v0.2.x, terminal-v0.1.x) sort below them.
# Before the first libdesk-v tag, the group's newest Files or Terminal tag stands for it.
match=("--match" "$prefix[0-9][0-9][0-9][0-9].*")
[[ $unit != libdesk ]] || match+=("--match" "files-v[0-9][0-9][0-9][0-9].*" "--match" "terminal-v[0-9][0-9][0-9][0-9].*")
if tag=$(git describe --tags --abbrev=0 "${match[@]}" 2> /dev/null); then
  tag=${tag#"$prefix"} tag=${tag#files-v} tag=${tag#terminal-v}
  echo "$tag.r$(git rev-list --count HEAD)"
elif count=$(git rev-list --count HEAD 2> /dev/null); then
  echo "0.r$count"
else
  echo 0
fi
