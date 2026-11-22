#!/usr/bin/env bash
# Build the pinned libghostty-vt into libdesk-term/libghostty-vt/prefix. It needs zig 0.17: without it
# on PATH (Ubuntu ships none), the pinned zig tarball is fetched. `macos` cross-builds aarch64-macos into prefix-macos.
set -euo pipefail
VER=1.0.1
# The terminal uses none of these; grid-introspection stays (ghostty_cell_get reads the wide-cell flag), kitty-graphics
# stays (inline images).
FEATURES=-color,-glyph-protocol
SRC_SHA=333dfce97641ef9b536bb9e08907ad86abaaada50e8f7417b70d9284adef37aa
DEPS_SHA=91a4e10960a52bf5bf4b15aa2a2cddaff8a89d6cd903d3055bda4fd17de3356d
ZIG_VER=0.17.1
ZIG_SHA=3581863e3cbcfd0fce0efc682ce7e9e93da96668a977791d356a26d2a7e9b17a
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/libdesk-term/libghostty-vt
PREFIX=$OUT/prefix
STAMP=$OUT/.version
TARGET=()
if [[ ${1:-} == macos ]]; then
  PREFIX=$OUT/prefix-macos
  STAMP=$OUT/.version-macos
  TARGET=(-Dtarget=aarch64-macos)
fi
[[ -f $PREFIX/lib/pkgconfig/libghostty-vt.pc && -f $PREFIX/LICENSE && $(cat "$STAMP" 2>/dev/null) == "$VER $FEATURES" ]] && exit 0
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
URL=https://release.files.ghostty.org/libghostty-vt/$VER
curl -fsSL -o "$WORK/src.tar.gz" "$URL/libghostty-vt-$VER.tar.gz"
curl -fsSL -o "$WORK/deps.tar.gz" "$URL/libghostty-vt-$VER-deps.tar.gz"
echo "$SRC_SHA  $WORK/src.tar.gz" | sha256sum -c --quiet
echo "$DEPS_SHA  $WORK/deps.tar.gz" | sha256sum -c --quiet
tar -C "$WORK" -xzf "$WORK/src.tar.gz"; tar -C "$WORK" -xzf "$WORK/deps.tar.gz"
ZIG=zig
if [[ $(zig version 2>/dev/null) != 0.17.* ]]; then
  curl -fsSL -o "$WORK/zig.tar.xz" "https://ziglang.org/download/$ZIG_VER/zig-x86_64-linux-$ZIG_VER.tar.xz"
  echo "$ZIG_SHA  $WORK/zig.tar.xz" | sha256sum -c --quiet
  tar -C "$WORK" -xJf "$WORK/zig.tar.xz"
  ZIG=$WORK/zig-x86_64-linux-$ZIG_VER/zig
fi
cd "$WORK/libghostty-vt-$VER"
rm -rf "$PREFIX"
ZIG_GLOBAL_CACHE_DIR=$WORK/cache "$ZIG" build --prefix "$PREFIX" --system "$WORK/libghostty-vt-$VER-deps" -Doptimize=ReleaseFast -Dcpu=baseline -Dvt-features=$FEATURES "${TARGET[@]}"
sed -i "s|^prefix=.*|prefix=$PREFIX|" "$PREFIX"/lib/pkgconfig/*.pc
cp LICENSE "$PREFIX/"
echo "$VER $FEATURES" > "$STAMP"
