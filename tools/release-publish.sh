#!/usr/bin/env bash
# The one owner of a tag release's page: creates the release of an existing tag (or keeps the one there) and uploads
# FILES with --clobber. NOTES_UNIT heads the notes with tools/release-notes.sh for that unit; "-" keeps GitHub's
# generated notes only. Never marked Latest: the rolling `repo` release stays Latest.
#   tools/release-publish.sh TAG NOTES_UNIT VERSION [FILE...]
# Env: GH_TOKEN, GITHUB_REPOSITORY.
set -euo pipefail
tag=$1 unit=$2 version=$3
shift 3
repo=${GITHUB_REPOSITORY:?}
notes=()
[[ $unit == - ]] || notes=(--notes "$("$(dirname "$0")/release-notes.sh" "$unit" "$version")")
gh release create "$tag" -R "$repo" --verify-tag --latest=false --generate-notes "${notes[@]}" \
  || gh release view "$tag" -R "$repo" > /dev/null
(($# == 0)) || gh release upload "$tag" -R "$repo" --clobber "$@"
