#!/usr/bin/env bash
# Build the pinned libghostty-vt into apps/terminal/subprojects/libghostty-vt/prefix (needs zig 0.17).
set -euo pipefail
VER=1.0.1
SRC_SHA=333dfce97641ef9b536bb9e08907ad86abaaada50e8f7417b70d9284adef37aa
DEPS_SHA=91a4e10960a52bf5bf4b15aa2a2cddaff8a89d6cd903d3055bda4fd17de3356d
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/apps/terminal/subprojects/libghostty-vt
PREFIX=$OUT/prefix
[[ -f $PREFIX/lib/pkgconfig/libghostty-vt.pc && $(cat "$OUT/.version" 2>/dev/null) == "$VER" ]] && exit 0
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
URL=https://release.files.ghostty.org/libghostty-vt/$VER
curl -fsSL -o "$WORK/src.tar.gz" "$URL/libghostty-vt-$VER.tar.gz"
curl -fsSL -o "$WORK/deps.tar.gz" "$URL/libghostty-vt-$VER-deps.tar.gz"
echo "$SRC_SHA  $WORK/src.tar.gz" | sha256sum -c --quiet
echo "$DEPS_SHA  $WORK/deps.tar.gz" | sha256sum -c --quiet
tar -C "$WORK" -xzf "$WORK/src.tar.gz"; tar -C "$WORK" -xzf "$WORK/deps.tar.gz"
cd "$WORK/libghostty-vt-$VER"
rm -rf "$PREFIX"
ZIG_GLOBAL_CACHE_DIR=$WORK/cache zig build --prefix "$PREFIX" --system "$WORK/libghostty-vt-$VER-deps" -Doptimize=ReleaseFast -Dcpu=baseline
sed -i "s|^prefix=.*|prefix=$PREFIX|" "$PREFIX"/lib/pkgconfig/*.pc
echo "$VER" > "$OUT/.version"
