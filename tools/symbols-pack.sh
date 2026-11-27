#!/usr/bin/env bash
# Moves the debug packages of an ANDROMEDA_SYMBOLS=1 makepkg build out of PKGDIR into OUTDIR/andromeda-symbols-TAG-x86_64.tar,
# a release asset that never enters the package repository. Map a crash offset with:
# addr2line -f -C -e <unpacked usr/lib/debug/...debug file> 0x<offset>.
#   tools/symbols-pack.sh TAG PKGDIR OUTDIR
set -euo pipefail
tag=$1 pkgdir=$2 outdir=$3
shopt -s nullglob
debug=("$pkgdir"/*-debug-*.pkg.tar.zst)
((${#debug[@]})) || { echo "::error::$pkgdir has no debug packages: build with ANDROMEDA_SYMBOLS=1" >&2; exit 1; }
mkdir -p "$outdir"
tar -cf "$outdir/andromeda-symbols-$tag-x86_64.tar" -C "$pkgdir" "${debug[@]##*/}"
rm -- "${debug[@]}"
