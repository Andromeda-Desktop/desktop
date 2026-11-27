#!/usr/bin/env bash
# Fetches the pinned llvm-mingw toolchain (clang, libc++, UCRT; Apache-2.0 with LLVM exception) into
# ~/.cache/andromeda-win and writes ~/.cache/andromeda-win/cross.ini, the meson cross file for x86_64 Windows.
# CI builds natively on windows-latest with MSYS2 CLANG64, the same clang + libc++ + UCRT stack.
set -euo pipefail
version=20261117
name=llvm-mingw-$version-ucrt-ubuntu-22.04-x86_64
sha256=e7a15a49a2ea95e9f3d8a9d0cfabe5052a4b10fb4777cc54de5a8c27b64bb701
cache=$HOME/.cache/andromeda-win
mkdir -p "$cache"
if [ ! -x "$cache/$name/bin/x86_64-w64-mingw32-clang++" ]; then
  curl -fsSL -o "$cache/$name.tar.xz.part" "https://github.com/mstorsjo/llvm-mingw/releases/download/$version/$name.tar.xz"
  echo "$sha256  $cache/$name.tar.xz.part" | sha256sum -c --quiet
  tar xf "$cache/$name.tar.xz.part" -C "$cache"
  rm "$cache/$name.tar.xz.part"
fi
bin=$cache/$name/bin
cat > "$cache/cross.ini" <<INI
[binaries]
c = '$bin/x86_64-w64-mingw32-clang'
cpp = '$bin/x86_64-w64-mingw32-clang++'
ar = '$bin/llvm-ar'
strip = '$bin/llvm-strip'
windres = '$bin/x86_64-w64-mingw32-windres'

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
INI
echo "$cache/cross.ini"
