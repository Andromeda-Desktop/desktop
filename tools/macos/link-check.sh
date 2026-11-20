#!/usr/bin/env bash
# Links every object of the macOS libdesk-ui build into one arm64 executable. Fails on an undefined symbol, except
# gl*/egl*: ANGLE provides those from step 9 of .agent/TASK-terminal-macos.md.
set -euo pipefail
build=${1:-libdesk-ui/build-macos}
deps=$HOME/.cache/andromeda-mac/deps
sdk=$(sed -n "s/^cpp = .*'-isysroot', '\([^']*\)'.*/\1/p" "$HOME/.cache/andromeda-mac/cross.ini")
export PKG_CONFIG_LIBDIR=$deps/lib/pkgconfig:$deps/share/pkgconfig
mac_deps="cairo cairo-ft fontconfig freetype2 fribidi gio-2.0 glib-2.0 gobject-2.0 harfbuzz libjxl libjxl_threads
  librsvg-2.0 libwebp libwebpdemux md4c pango pangocairo pangoft2 xkbcommon zlib"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
echo 'int main() { return 0; }' > "$work/main.cpp"
# shellcheck disable=SC2086
if clang++ -target arm64-apple-macos26 -isysroot "$sdk" -fuse-ld=lld "$work/main.cpp" \
    -Wl,-all_load -Wl,--error-limit=0 "$build/libdesk_ui.a" "$build/libfzy.a" \
    -L"$deps/lib" $(pkg-config --libs $mac_deps) -o "$work/link-check" > "$work/link.log" 2>&1; then
  echo "macos link: ok"
  exit 0
fi
if grep -q 'undefined symbol' "$work/link.log"; then
  missing=$(sed -n 's/.*undefined symbol: //p' "$work/link.log" | grep -vE '^(gl|egl)[A-Z]' | sort -u || true)
  if [ -z "$missing" ]; then
    echo "macos link: ok (GL/EGL left for ANGLE)"
    exit 0
  fi
  echo "macos link: undefined symbols outside GL/EGL:"
  echo "$missing" | head -n 40
else
  tail -n 20 "$work/link.log"
fi
exit 1
