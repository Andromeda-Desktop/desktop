#!/usr/bin/env bash
# Usage: [DEB_VERSION=x.y.z] [ANDROMEDA_APP=libdesk] tools/ubuntu-deb.sh [--clean | --trim-cache] [ubuntu-version | debian-suite]
# --trim-cache only trims the CI ccache to 1.5G and gives the caches back to the user: a CI step runs it under always(),
# so a cancelled build saves no untrimmed cache.
# Builds the Ubuntu or Debian (a suite name, e.g. testing) .deb packages (debian/) from the working tree in a container, then installs them with apt to
# prove their dependencies resolve. Incremental: the copied tree, the meson build dirs and the cargo target dir stay
# in the output dir, so a rerun compiles only what changed. --clean builds from nothing (CI, releases).
# Output in ~/.cache/andromeda-ubuntu/deb-<version>/ (tmpfs /tmp is too small): the debs, build.log, install.log.
set -euo pipefail
CLEAN=0
TRIM=0
if [[ ${1:-} == --clean ]]; then
  CLEAN=1
  shift
elif [[ ${1:-} == --trim-cache ]]; then
  TRIM=1
  shift
fi
VERSION=${1:-26.04}
[[ $VERSION == [0-9]* ]] && BASE=ubuntu:$VERSION || BASE=debian:$VERSION
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${XDG_CACHE_HOME:-$HOME/.cache}/andromeda-ubuntu/deb-$VERSION
IMAGE=andromeda-deb:${BASE/:/-}
# ccache lives beside OUT, so --clean keeps it.
CCACHE=$(dirname "$OUT")/ccache-deb-$VERSION
# In CI the repo's caches share 10 GiB: no limit while building (a trim mid-build would drop entries this build has
# not reached yet), then a trim to 1.5G after it, least recently used first.
[[ -z ${CI:-} ]] || ((TRIM)) || { mkdir -p "$CCACHE" && echo 'max_size = 0' > "$CCACHE/ccache.conf"; }
# Crate downloads persist across runs, so a build needs the network only for new crates.
CARGO_CACHE=$(dirname "$OUT")/cargo-deb-$VERSION
trim_cache() {
  [[ -z ${CI:-} ]] || ! docker image inspect "$IMAGE" > /dev/null 2>&1 \
    || docker run --rm -v "$CCACHE:/ccache" -e CCACHE_DIR=/ccache "$IMAGE" ccache --max-size=1.5G --cleanup > /dev/null
  mkdir -p "$OUT" "$CCACHE" "$CARGO_CACHE"
  docker run --rm -v "$OUT:/o" -v "$CCACHE:/ccache" -v "$CARGO_CACHE:/cargo" "$BASE" chown -R "$(id -u):$(id -g)" /o /ccache /cargo
}
if ((TRIM)); then
  trim_cache
  exit 0
fi
mkdir -p "$CARGO_CACHE"
mkdir -p "$OUT" "$CCACHE"
# The container writes as root: clear through it.
if [[ $CLEAN == 1 ]]; then
  docker run --rm -v "$OUT:/o" "$BASE" sh -c 'rm -rf /o/*'
else
  docker run --rm -v "$OUT:/o" "$BASE" sh -c 'rm -f /o/*.deb /o/*.ddeb /o/*.buildinfo /o/*.changes'
fi
mkdir -p "$OUT/src/pkg"
# Tracked and new files without the Link submodule (Link ships its own deb) or pkg/ build output. rsync skips
# unchanged files, so their timestamps stay and ninja does not rebuild them. A file the last copy had and the tree
# no longer has is deleted, or it shadows the file that replaced it; build output is in neither list and stays.
git -C "$ROOT" ls-files -co --exclude-standard -z | grep -zv -e '^link/' -e '^pkg/' | sort -z > "$OUT/files.new"
[[ ! -f $OUT/files ]] || comm -z -23 "$OUT/files" "$OUT/files.new" | (cd "$OUT/src" && xargs -0r rm -f --)
rsync -a --from0 --files-from="$OUT/files.new" "$ROOT/" "$OUT/src/"
mv "$OUT/files.new" "$OUT/files"
rsync -a "$ROOT/pkg/PKGBUILD" "$ROOT/pkg/deb-build.sh" "$OUT/src/pkg/"
# A release sets DEB_VERSION from its tag, so apt upgrades between releases; other builds keep 0~git0, which sorts
# below every release.
if [[ -n ${DEB_VERSION:-} ]]; then
  sed -i "1s/([^)]*)/($DEB_VERSION)/" "$OUT/src/debian/changelog"
  # `andromeda --version` and the log's start line report the release, not the source tree's placeholder.
  printf '%s\n' "$DEB_VERSION" > "$OUT/src/compositor/VERSION"
fi
# The copy has no .git, so the build cannot describe its own commit.
git -C "$ROOT" describe --tags --always --dirty=-dirty --abbrev=12 > "$OUT/src/compositor/REVISION"

# The build dependencies from debian/control live in an image layer: docker reuses it until control changes.
CTX=$OUT/image
mkdir -p "$CTX"
cp "$ROOT/debian/control" "$CTX/control"
docker build --progress=plain -t "$IMAGE" -f - "$CTX" << DOCKER
FROM $BASE
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get -o Acquire::Retries=5 update && apt-get install -y -qq --no-install-recommends devscripts equivs ca-certificates
COPY control /tmp/control
RUN apt-get -o Acquire::Retries=5 update && mk-build-deps -i -r -t "apt-get -y -qq --no-install-recommends" /tmp/control
RUN apt-get install -y -qq --no-install-recommends ccache
DOCKER

# Packages from our apt repo that the distro lacks (tools/deb-xwayland-satellite.sh).
mkdir -p "$OUT/extra"
cp "$(dirname "$OUT")/xwayland-satellite-$VERSION"/*.deb "$OUT/extra/" 2> /dev/null || true
# Link's latest debs (release-apps puts them beside OUT for link-app, which needs that andromeda-link).
[[ ${ANDROMEDA_APP:-} != libdesk ]] || cp "$(dirname "$OUT")/link-$VERSION"/andromeda-link_*.deb "$OUT/extra/" 2> /dev/null || true
# ANDROMEDA_ONLY="shell" (a unit release): only those units' debs stay, are installed and get published; the PKGBUILD
# names them, a deb being its package name without -git.
ONLY_DEBS=
[[ -z ${ANDROMEDA_ONLY:-} ]] ||
  ONLY_DEBS=$(cd "$ROOT/pkg" && ANDROMEDA_ONLY="$ANDROMEDA_ONLY" bash -c 'source PKGBUILD > /dev/null; echo "${pkgname[@]%-git}"')
LINK_REPO=https://github.com/Andromeda-Desktop/desktop/releases/latest/download
[[ $VERSION == [0-9]* ]] || LINK_REPO=https://github.com/Andromeda-Desktop/desktop/releases/download/debian
status=0
# -nc: no pre-clean, so debian/build (meson dirs) and system/target (cargo) survive; dh_prep still clears each
# package's staging dir before the install step.
# Host network: on the default bridge, DNS inside the container failed mid-build in three runs.
docker run --rm --network host --cpus "${UBUNTU_BUILD_CPUS:-6}" -v "$OUT:/o" -v "$CCACHE:/ccache" -v "$CARGO_CACHE:/cargo" -e CCACHE_DIR=/ccache -e CARGO_HOME=/cargo -e CARGO_NET_RETRY=10 -e ANDROMEDA_APP="${ANDROMEDA_APP:-}" -e ANDROMEDA_CORE="${ANDROMEDA_CORE:-}" -e ANDROMEDA_ONLY="${ANDROMEDA_ONLY:-}" -e ONLY_DEBS="$ONLY_DEBS" -e ANDROMEDA_VERSION="${DEB_VERSION:-}" -e LINK_REPO="$LINK_REPO" -w /o/src "$IMAGE" bash -c '
  # A kept build stamp would make dh skip the build step, and cargo with it. A kept debian/files lists the
  # artifacts of the last build, and dpkg-genchanges fails when their version differs.
  rm -f debian/debhelper-build-stamp debian/files
  set -o pipefail
  # Fetch every crate before the build, with retries: a DNS drop mid-build failed two runs.
  for lock in $(find . -name Cargo.lock -not -path "*/target/*" -not -path "./debian/*"); do
    for try in 1 2 3; do cargo fetch --locked --manifest-path "$(dirname "$lock")/Cargo.toml" > /dev/null 2>> /o/fetch.log && break
      [ $try = 3 ] && { echo "cargo fetch failed: $lock" | tee -a /o/build.log; exit 11; }; sleep 5; done
  done
  dpkg-buildpackage -b -nc -us -uc 2>&1 | tee /o/build.log || exit 11
  # A unit release: the other debs are empty.
  if [ -n "$ONLY_DEBS" ]; then
    for deb in /o/*.deb; do name=$(basename "$deb"); case " $ONLY_DEBS " in *" ${name%%_*} "*) ;; *) rm "$deb" ;; esac; done
  fi
  # andromeda-link comes from the Link release, not this source; install every other package. An app build installs
  # only its release unit: the other debs are empty. What the unit needs from the core or Link (andromeda-preview,
  # andromeda-link) comes from the published apt repo of this base.
  [ -z "$ANDROMEDA_APP" ] || echo "deb [signed-by=/o/src/andromeda-release.asc] $LINK_REPO ./" > /etc/apt/sources.list.d/andromeda.list
  apt-get -o Acquire::Retries=5 update
  if [ -n "$ANDROMEDA_APP" ]; then
    debs=$(ls /o/andromeda-"$ANDROMEDA_APP"_*.deb 2> /dev/null)
    [ "$ANDROMEDA_APP" != libdesk ] || debs="$debs $(ls /o/andromeda-files_*.deb /o/andromeda-terminal_*.deb /o/andromeda-player_*.deb /o/andromeda-viewer_*.deb /o/andromeda-link-app_*.deb)"
    [ "$ANDROMEDA_APP" != libdesk ] || debs="$debs $(ls /o/extra/andromeda-link_*.deb 2> /dev/null)"
  else
    debs=$(ls /o/*.deb /o/extra/*.deb 2> /dev/null | grep -v "/andromeda_\|/andromeda-dbgsym_")
    [ -z "$ANDROMEDA_CORE" ] || debs=$(echo "$debs" | grep -v "/andromeda-\(files\|player\|viewer\|terminal\|link-app\|libdesk\)_")
  fi
  apt-get install -y -qq $debs 2>&1 | tee /o/install.log || exit 12
' || status=$?
trim_cache
case $status in
  0) echo "PASS: $(ls "$OUT"/*.deb | wc -l) debs built and installed" ;;
  11) echo "FAIL build: $(tail -n 3 "$OUT/build.log")" ;;
  12) echo "FAIL install: $(tail -n 3 "$OUT/install.log")" ;;
  *) echo "FAIL: container exit $status" ;;
esac
exit "$status"
