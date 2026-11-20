#!/usr/bin/env bash
# Links every object of every archive in a macOS build dir into one arm64 executable. Fails on an undefined symbol,
# except those the later steps of .agent/TASK-terminal-macos.md provide: gl*/egl* (ANGLE, step 9) and
# createDisplay() (Cocoa backend, step 10).
# EXE (optional): an executable target of that build dir; its objects and main() join the link.
# Further arguments: prebuilt archives outside the build dir (libghostty-vt).
set -euo pipefail
build=${1:-libdesk-app/build-macos}
exe=${2:-}
shift $(( $# < 2 ? $# : 2 ))
extra=("$@")
mapfile -t archives < <(find "$build" -name '*.a' | sort)
[ ${#archives[@]} -gt 0 ] || { echo "macos link: no archives in $build"; exit 1; }
deps=$HOME/.cache/andromeda-mac/deps
sdk=$(sed -n "s/^cpp = .*'-isysroot', '\([^']*\)'.*/\1/p" "$HOME/.cache/andromeda-mac/cross.ini")
export PKG_CONFIG_LIBDIR=$deps/lib/pkgconfig:$deps/share/pkgconfig
mac_deps="cairo cairo-ft fontconfig freetype2 fribidi gio-2.0 glib-2.0 gobject-2.0 harfbuzz libjxl libjxl_threads
  librsvg-2.0 libwebp libwebpdemux md4c pango pangocairo pangoft2 xkbcommon zlib"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if [ -n "$exe" ]; then
  mapfile -t mains < <(find "$build/$exe.p" -name '*.o' | sort)
  [ ${#mains[@]} -gt 0 ] || { echo "macos link: no objects in $build/$exe.p"; exit 1; }
else
  echo 'int main() { return 0; }' > "$work/main.cpp"
  mains=("$work/main.cpp")
fi
# shellcheck disable=SC2086
if clang++ -target arm64-apple-macos26 -isysroot "$sdk" -fuse-ld=lld "${mains[@]}" \
    -Wl,-all_load -Wl,--error-limit=0 "${archives[@]}" ${extra[@]+"${extra[@]}"} \
    -L"$deps/lib" $(pkg-config --libs $mac_deps) -framework AppKit -framework UserNotifications -lc++experimental -o "$work/link-check" > "$work/link.log" 2>&1; then
  echo "macos link: ok"
  exit 0
fi
if grep -q 'undefined symbol' "$work/link.log"; then
  missing=$(sed -n 's/.*undefined symbol: //p' "$work/link.log" | grep -vE '^((gl|egl)[A-Z]|createDisplay\(\)$)' | sort -u || true)
  if [ -z "$missing" ]; then
    echo "macos link: ok (GL/EGL and createDisplay left for steps 9 and 10)"
    exit 0
  fi
  echo "macos link: undefined symbols outside GL/EGL and createDisplay:"
  echo "$missing" | head -n 40
else
  tail -n 20 "$work/link.log"
fi
exit 1
