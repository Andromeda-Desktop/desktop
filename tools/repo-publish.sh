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
here=$(dirname "$(realpath "$0")")

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
  while read -r name file; do
    repo-remove --sign --key "$key" "$out/andromeda.db.tar.gz" "$name"
    rm -f "$out/$file" "$out/$file.sig"
    echo "$file" >> "$out.dropped"
  done < <(replaced "$out/andromeda.db.tar.gz")
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
    # Old entries stay only when they still describe their file: apt reads the first entry of a version and rejects
    # a file of another size. This run's files and a republished file (another size) lose their old entry.
    id=$(gh api "repos/$repo/releases/tags/$release" -q .id)
    gh api --paginate "repos/$repo/releases/$id/assets?per_page=100" -q '.[] | "./\(.name) \(.size)"' > ours
    names | sed 's/$/ -1/' >> ours
    awk -v RS= -v FS='\n' -v ORS='\n\n' 'NR == FNR { for (i = 1; i <= NF; i++) { split($i, a, " "); size[a[1]] = a[2] }; next }
      { file = ""; bytes = ""
        for (i = 1; i <= NF; i++) { if ($i ~ /^Filename: /) file = substr($i, 11); if ($i ~ /^Size: /) bytes = substr($i, 7) }
        if (file in size && size[file] == bytes && !(file in seen)) { seen[file]; print } }' ours Packages.old > Packages.kept
    # --multiversion keeps the older entries; apt installs the highest version.
    { cat Packages.kept; echo; dpkg-scanpackages --multiversion . /dev/null; } | cat -s > Packages
    replaced Packages > gone
    awk -v RS= -v ORS='\n\n' -v names="$(awk '{ print $1 }' gone)" \
      'BEGIN { split(names, n, "\n"); for (i in n) gone[n[i]] } { split($0, f, "\n"); if (!(substr(f[1], 10) in gone)) print }' \
      Packages > Packages.left
    mv Packages.left Packages
    awk '{ print $2 }' gone | tee -a "$out.dropped" | xargs -r rm -f
    rm Packages.old Packages.kept ours gone
    gzip -kf9 Packages
    apt-ftparchive release . > Release
    gpg --batch --yes --clearsign -u "$key" -o InRelease Release
    gpg --batch --yes --armor --detach-sign -u "$key" -o Release.gpg Release
  )
}

# Entries another entry replaces outright ("<name> <file>"): the merge takes them out of the index.
replaced() { "$here/repo-check.py" "$kind" "$1" --replaced; }

# The names the index must list after the upload: this run's packages, less those a listed package replaces.
names() {
  touch "$out.dropped"
  _names | grep -vxFf "$out.dropped" || true
}
_names() {
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

# One run at a time per index: an atomic ref create is the lock; a lock older than 30 min is a dead run's and is taken.
lock=locks/repo-$kind-$release
lock_sha=
unlock() {
  [[ -n $lock_sha && $(gh api "repos/$repo/git/ref/$lock" -q .object.sha 2> /dev/null) == "$lock_sha" ]] \
    && gh api -X DELETE "repos/$repo/git/refs/$lock" > /dev/null || true
}
acquire() {
  local tree held born
  tree=$(gh api "repos/$repo/git/commits/${GITHUB_SHA:?}" -q .tree.sha)
  lock_sha=$(gh api "repos/$repo/git/commits" -f message="repo-publish lock, run ${GITHUB_RUN_ID:-local}" -f tree="$tree" -q .sha)
  # 45 min of waits: longer than the 30 min after which a lock is stale (a cancelled run never unlocks), so a waiter
  # takes a dead lock instead of giving up just before it may.
  for _ in $(seq 180); do
    if err=$(gh api "repos/$repo/git/refs" -f ref="refs/$lock" -f sha="$lock_sha" 2>&1 > /dev/null); then
      trap unlock EXIT
      return 0
    fi
    if ! held=$(gh api "repos/$repo/git/ref/$lock" -q .object.sha 2> /dev/null); then
      echo "repo-publish: cannot take or read $lock: $err" >&2
      sleep 15
      continue
    fi
    born=$(gh api "repos/$repo/git/commits/$held" -q .committer.date)
    if (($(date +%s) - $(date -d "$born" +%s) > 1800)); then
      echo "repo-publish: taking the stale lock $lock from $born" >&2
      gh api -X DELETE "repos/$repo/git/refs/$lock" > /dev/null || true
      continue
    fi
    echo "repo-publish: $lock is held, waiting" >&2
    sleep 15
  done
  echo "::error::repo-publish: $lock still held after 45 min" >&2
  exit 1
}
[[ ${PUBLISH:-true} != true ]] || acquire

index_file() { if [[ $kind == arch ]]; then echo andromeda.db.tar.gz; else echo Packages; fi; }
# Unmet dependencies of an index, one per line; none for a missing index.
unmet() { [[ ! -f $1 ]] || "$(dirname "$0")/repo-check.py" "$kind" "$1" 2> /dev/null || true; }

for attempt in 1 2 3; do
  rm -rf "$out" "$out.base" "$out.dropped"
  mkdir -p "$out" "$out.base"
  gh release download "$release" -R "$repo" -D "$out.base" -p "$(index_file)" 2> /dev/null || true
  "merge_$kind"
  # The upload must not leave an app without its libdesk: it fails on an unmet dependency the index did not have.
  # One it had already (an older release's drift) does not block a release that leaves it as it was or fixes it.
  added=$(comm -13 <(unmet "$out.base/$(index_file)" | sort) <(unmet "$out/$(index_file)" | sort))
  if [[ -n $added ]]; then
    echo "::error::repo-publish: the merged index gets unmet dependencies:" >&2
    echo "$added" >&2
    exit 1
  fi
  kept=$(unmet "$out/$(index_file)")
  [[ -z $kept ]] || echo "::warning::repo-publish: the index still has unmet dependencies: $kept" >&2
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
