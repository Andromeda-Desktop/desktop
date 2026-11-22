#!/bin/sh
# Sets lazy:Depends for each package: the packages of the libraries its files open with dlopen on first use
# (core/lazy_library.h). dpkg-shlibdeps sees only linked libraries; the meson builds list the sonames in lazy-sonames.txt.
set -eu
sonames=$(find . -path ./debian -prune -o -name lazy-sonames.txt -print | xargs -r cat | sort -u)
for pkg in $(dh_listpackages); do
  deps=
  for soname in $sonames; do
    grep -rqaF "$soname" "debian/$pkg/usr" 2> /dev/null || continue
    path=$(ldconfig -p | awk -v s="$soname" '$1 == s { print $NF; exit }')
    [ -n "$path" ] || { echo "lazy-depends: $soname is not installed" >&2; exit 1; }
    owner=$(dpkg -S "$(readlink -f "$path")" | head -n 1 | cut -d: -f1)
    deps="$deps, $owner"
  done
  [ -z "$deps" ] || echo "lazy:Depends=${deps#, }" >> "debian/$pkg.substvars"
done
