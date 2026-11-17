#!/usr/bin/env bash
# Merges packages into a shared repository release and uploads it: the pacman repository or the apt repository of
# one base. Nothing else is removed: the index keeps every other unit's entries, and repo-add / --multiversion keep
# the highest version of each package. After the upload it reads the index back; when another run replaced it in the
# meantime (its packages are missing), it merges again.
#   tools/repo-publish.sh arch RELEASE DIR      DIR holds *.pkg.tar.zst
#   tools/repo-publish.sh apt RELEASE DIR       DIR holds *.deb
# Env: GH_TOKEN, GITHUB_REPOSITORY, RELEASE_GPG_KEY; PUBLISH=false builds the merged index in ./repo-out only.
set -euo pipefail
kind=$1 release=$2 src=$(realpath "$3")
repo=${GITHUB_REPOSITORY:?}
out=$PWD/repo-out

echo "$RELEASE_GPG_KEY" | gpg --batch --import 2> /dev/null
key=$(gpg --list-secret-keys --with-colons | awk -F: '/^fpr/{print $10; exit}')

merge_arch() {
  gh release download "$release" -R "$repo" -D "$out" --clobber \
    -p andromeda.db.tar.gz -p andromeda.db.tar.gz.sig -p andromeda.files.tar.gz -p andromeda.files.tar.gz.sig
  # GitHub rewrites ':' in asset names (the epoch); pacman reads the version from inside the package.
  for package in "$src"/*.pkg.tar.zst; do
    name=$(basename "$package")
    cp "$package" "$out/${name//:/.}"
    gpg --batch --yes --detach-sign --no-armor -u "$key" "$out/${name//:/.}"
  done
  repo-add --sign --key "$key" "$out/andromeda.db.tar.gz" "$out"/*.pkg.tar.zst
  rm -f "$out"/*.old "$out"/*.old.sig
  # Release assets cannot be symlinks.
  for link in andromeda.db andromeda.db.sig andromeda.files andromeda.files.sig; do
    cp --remove-destination "$(readlink -f "$out/$link")" "$out/$link"
  done
}

merge_apt() {
  for deb in "$src"/*.deb; do
    name=$(basename "$deb")
    cp "$deb" "$out/${name//:/.}"
  done
  (
    cd "$out"
    gh release download "$release" -R "$repo" -p Packages -O Packages.old --clobber
    # --multiversion keeps the older entries; apt installs the highest version.
    { cat Packages.old; echo; dpkg-scanpackages --multiversion . /dev/null; } | cat -s > Packages
    rm Packages.old
    gzip -kf9 Packages
    apt-ftparchive release . > Release
    gpg --batch --yes --clearsign -u "$key" -o InRelease Release
    gpg --batch --yes --armor --detach-sign -u "$key" -o Release.gpg Release
  )
}

# The names the index must list after the upload: this run's packages.
names() {
  if [[ $kind == arch ]]; then
    for p in "$src"/*.pkg.tar.zst; do b=$(basename "$p"); echo "${b//:/.}"; done
  else
    for p in "$src"/*.deb; do b=$(basename "$p"); echo "./${b//:/.}"; done
  fi
}

listed() {
  local index
  index=$(mktemp -d)
  if [[ $kind == arch ]]; then
    gh release download "$release" -R "$repo" -D "$index" -p andromeda.db.tar.gz
    tar -xOzf "$index/andromeda.db.tar.gz" --wildcards '*/desc' | awk '/^%FILENAME%$/{getline; print}'
  else
    gh release download "$release" -R "$repo" -D "$index" -p Packages
    awk '/^Filename: /{print $2}' "$index/Packages"
  fi
  rm -rf "$index"
}

for attempt in 1 2 3; do
  rm -rf "$out"
  mkdir -p "$out"
  "merge_$kind"
  if [[ ${PUBLISH:-true} != true ]]; then
    echo "repo-publish: dry run, merged index in $out"
    exit 0
  fi
  gh release upload "$release" -R "$repo" --clobber "$out"/*
  missing=$(comm -23 <(names | sort) <(listed | sort))
  if [[ -z $missing ]]; then
    echo "repo-publish: $kind index of $release lists this run's packages"
    exit 0
  fi
  echo "repo-publish: another run replaced the index (missing: $missing); merging again ($attempt)" >&2
done
echo "::error::repo-publish: the $kind index of $release still misses this run's packages" >&2
exit 1
