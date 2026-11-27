#!/usr/bin/env bash
# Makes a libdesk app bundle self-contained: copies every dylib that BINs load from outside the system into
# APP/Contents/Frameworks (from the deps.py prefix, then EXTRA, colon-separated dirs), until none is missing, points
# each load at @rpath/<name>, and gives each BIN that loads one an rpath to Frameworks. Adds what libdesk reads at run
# time from Resources: the fontconfig setup (cocoa_display.mm) and ANGLE's licence. Fails when any Mach-O in APP
# still loads or searches a path outside the bundle. Needs llvm-otool, llvm-install-name-tool and GNU realpath.
#   bundle-libdesk.sh APP EXTRA BIN...
set -euo pipefail
app=$1
deps=$HOME/.cache/andromeda-mac/deps
search=$deps/lib${2:+:$2}
shift 2
fw=$app/Contents/Frameworks
res=$app/Contents/Resources
mkdir -p "$fw" "$res/fonts/conf.d" "$res/licenses/angle"

# Load paths a bundle may keep; anything else points at the build host or at a Homebrew prefix.
allowed() { [[ $1 == /usr/lib/* || $1 == /System/* || $1 == @rpath/* || $1 == @executable_path/* || $1 == @loader_path/* || $1 == @loader_path ]]; }
# Dependencies of one Mach-O, without its own install id.
deps_of() {
  local id
  id=$(llvm-otool -D "$1" | sed -n 2p)
  llvm-otool -L "$1" | sed -n 's/^\t\(.*\) (compatibility.*/\1/p' | grep -vxF -- "${id:-/nonexistent}" || true
}
rpaths_of() { llvm-otool -l "$1" | sed -n '/cmd LC_RPATH/,+2s/^ *path \(.*\) (offset.*/\1/p'; }
keep_rpath() {
  local bin=$1 keep=$2
  while read -r rp; do
    [ "$rp" = "$keep" ] || llvm-install-name-tool -delete_rpath "$rp" "$bin"
  done < <(rpaths_of "$bin")
  rpaths_of "$bin" | grep -qxF "$keep" || llvm-install-name-tool -add_rpath "$keep" "$bin"
}

bins=("$@")
queue=("$@")
while [ ${#queue[@]} -gt 0 ]; do
  bin=${queue[0]}
  queue=("${queue[@]:1}")
  while read -r dep; do
    [[ -z $dep || $dep == /usr/lib/* || $dep == /System/* ]] && continue
    name=${dep##*/}
    if [ ! -e "$fw/$name" ]; then
      src=
      IFS=: read -ra dirs <<< "$search"
      for dir in "${dirs[@]}"; do
        [ -e "$dir/$name" ] && { src=$dir/$name; break; }
      done
      [ -n "$src" ] || { echo "bundle-libdesk.sh: $bin needs $dep, not in $search" >&2; exit 1; }
      cp -L "$src" "$fw/$name"
      chmod u+w "$fw/$name"
      llvm-install-name-tool -id "@rpath/$name" "$fw/$name"
      queue+=("$fw/$name")
    fi
    [ "$dep" = "@rpath/$name" ] || llvm-install-name-tool -change "$dep" "@rpath/$name" "$bin"
  done < <(deps_of "$bin")
done

for bin in "${bins[@]}"; do
  if deps_of "$bin" | grep -q '^@rpath/'; then
    keep_rpath "$bin" "@executable_path/$(realpath --relative-to="$(dirname "$bin")" "$fw")"
  fi
done
shopt -s nullglob
for lib in "$fw"/*; do keep_rpath "$lib" @loader_path; done
shopt -u nullglob

cp "$(dirname "$0")/fonts.conf" "$res/fonts/"
for conf in 05-macos 10-hinting-slight 10-scale-bitmap-fonts 30-metric-aliases 40-nonlatin 45-generic 45-latin 48-spacing \
    49-sansserif 60-generic 60-latin 65-nonlatin 80-delicious 90-synthetic; do
  cp "$deps/share/fontconfig/conf.avail/$conf.conf" "$res/fonts/conf.d/"
done
cp "$deps/share/licenses/angle/LICENSE" "$res/licenses/angle/"

bad=0
while IFS= read -r -d '' file; do
  llvm-otool -h "$file" 2> /dev/null | grep -q magic || continue
  while read -r dep; do
    allowed "$dep" || { echo "bundle-libdesk.sh: $file loads $dep" >&2; bad=1; }
  done < <(deps_of "$file")
  while read -r rp; do
    allowed "$rp" || { echo "bundle-libdesk.sh: $file has rpath $rp" >&2; bad=1; }
  done < <(rpaths_of "$file")
done < <(find "$app/Contents" -type f \( -path '*/MacOS/*' -o -path '*/Frameworks/*' \) -print0)
[ $bad -eq 0 ]
