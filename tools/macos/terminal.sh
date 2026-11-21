#!/usr/bin/env bash
# Cross-builds "Andromeda Terminal.app" for arm64 macOS on Linux: terminal, ptyd, agentd, the dylibs they load,
# the data files, and an ad-hoc signature. Out: $OUT (default /var/tmp/andromeda-macos).
set -euo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
out=${OUT:-/var/tmp/andromeda-macos}
cross=$HOME/.cache/andromeda-mac/cross.ini
[ -f "$cross" ] || tools/macos/deps.py
deps=$HOME/.cache/andromeda-mac/deps
sdk=$(sed -n "s/^cpp = .*'-isysroot', '\([^']*\)'.*/\1/p" "$cross")
ghostty_lib=$root/apps/terminal/subprojects/libghostty-vt/prefix-macos/lib

tools/build-libghostty.sh macos
[ -d apps/terminal/build-macos ] || meson setup apps/terminal/build-macos apps/terminal --cross-file "$cross" -Dplatform=macos --buildtype=release
tools/build.sh ninja -C apps/terminal/build-macos andromeda-terminal

export MACOSX_DEPLOYMENT_TARGET=26.0
export CARGO_TARGET_AARCH64_APPLE_DARWIN_LINKER=clang
export CARGO_TARGET_AARCH64_APPLE_DARWIN_RUSTFLAGS="-C link-arg=-target -C link-arg=arm64-apple-macos26 -C link-arg=-isysroot -C link-arg=$sdk -C link-arg=-fuse-ld=lld"
for crate in ptyd agentd; do
  cargo build -q --release --locked --target aarch64-apple-darwin --manifest-path apps/terminal/$crate/Cargo.toml \
    --target-dir /var/tmp/andromeda-macos-cargo
done

app="$out/Andromeda Terminal.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources"
macos=$app/Contents/MacOS
fw=$app/Contents/Frameworks
res=$app/Contents/Resources
cp apps/terminal/build-macos/andromeda-terminal "$macos/"
cp /var/tmp/andromeda-macos-cargo/aarch64-apple-darwin/release/andromeda-{ptyd,agentd} "$macos/"

# Load paths a bundle may keep; anything else points at the build host or at a Homebrew prefix.
allowed() { [[ $1 == /usr/lib/* || $1 == /System/* || $1 == @rpath/* || $1 == @executable_path/* || $1 == @loader_path/* || $1 == @loader_path ]]; }
# Dependencies of one Mach-O, without its own install id.
deps_of() {
  local id
  id=$(llvm-otool -D "$1" | sed -n 2p)
  llvm-otool -L "$1" | sed -n 's/^\t\(.*\) (compatibility.*/\1/p' | grep -vxF -- "${id:-/nonexistent}" || true
}
rpaths_of() { llvm-otool -l "$1" | sed -n '/cmd LC_RPATH/,+2s/^ *path \(.*\) (offset.*/\1/p'; }

queue=("$macos/andromeda-terminal" "$macos/andromeda-ptyd" "$macos/andromeda-agentd")
while [ ${#queue[@]} -gt 0 ]; do
  bin=${queue[0]}
  queue=("${queue[@]:1}")
  while read -r dep; do
    [[ -z $dep || $dep == /usr/lib/* || $dep == /System/* ]] && continue
    name=${dep##*/}
    if [ ! -e "$fw/$name" ]; then
      src=
      for dir in "$deps/lib" "$ghostty_lib"; do
        [ -e "$dir/$name" ] && { src=$dir/$name; break; }
      done
      [ -n "$src" ] || { echo "terminal.sh: $bin needs $dep, not in $deps/lib" >&2; exit 1; }
      cp -L "$src" "$fw/$name"
      chmod u+w "$fw/$name"
      llvm-install-name-tool -id "@rpath/$name" "$fw/$name"
      queue+=("$fw/$name")
    fi
    [ "$dep" = "@rpath/$name" ] || llvm-install-name-tool -change "$dep" "@rpath/$name" "$bin"
  done < <(deps_of "$bin")
done

keep_rpath() {
  local bin=$1 keep=$2
  while read -r rp; do
    [ "$rp" = "$keep" ] || llvm-install-name-tool -delete_rpath "$rp" "$bin"
  done < <(rpaths_of "$bin")
  rpaths_of "$bin" | grep -qxF "$keep" || llvm-install-name-tool -add_rpath "$keep" "$bin"
}
keep_rpath "$macos/andromeda-terminal" @executable_path/../Frameworks
for lib in "$fw"/*; do keep_rpath "$lib" @loader_path; done

mkdir -p "$res/assets" "$res/terminfo" "$res/fonts/conf.d" "$res/licenses/angle"
rsync -a --exclude '*-wallpaper.png' shell/assets/ "$res/assets/"
cp -r apps/terminal/data/shell-integration "$res/"
tic -x -o "$res/terminfo" apps/terminal/data/andromeda.terminfo
# macOS ncurses looks entries up under the hex name of their first letter.
for dir in "$res/terminfo"/?; do mv "$dir" "$res/terminfo/$(printf '%x' "'${dir##*/}")"; done
cp tools/macos/fonts.conf "$res/fonts/"
tools/macos/icns.py apps/terminal/data/icons/dev.andromeda.Terminal.svg "$res/AppIcon.icns"
for conf in 05-macos 10-hinting-slight 10-scale-bitmap-fonts 30-metric-aliases 40-nonlatin 45-generic 45-latin 48-spacing \
    49-sansserif 60-generic 60-latin 65-nonlatin 80-delicious 90-synthetic; do
  cp "$deps/share/fontconfig/conf.avail/$conf.conf" "$res/fonts/conf.d/"
done
cp "$deps/share/licenses/angle/LICENSE" "$res/licenses/angle/"

# A bundle version is at most three numbers: drop the .rN build suffix.
version=$(tools/unit-version.sh terminal)
version=${version%%.r*}
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>org.andromeda.Terminal</string>
  <key>CFBundleName</key><string>Andromeda Terminal</string>
  <key>CFBundleExecutable</key><string>andromeda-terminal</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$version</string>
  <key>CFBundleVersion</key><string>$version</string>
  <key>LSMinimumSystemVersion</key><string>26.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
EOF

bad=0
for bin in "$macos"/* "$fw"/*; do
  while read -r dep; do
    allowed "$dep" || { echo "terminal.sh: $bin loads $dep" >&2; bad=1; }
  done < <(deps_of "$bin")
  while read -r rp; do
    allowed "$rp" || { echo "terminal.sh: $bin has rpath $rp" >&2; bad=1; }
  done < <(rpaths_of "$bin")
done
[ $bad -eq 0 ] || exit 1

llvm-strip -S "$macos/andromeda-terminal"
# Signs every Mach-O in the bundle; install-name changes broke their old signatures.
rcodesign sign "$app" 2>&1 | grep -c 'signing Mach-O' | sed 's/^/terminal.sh: signed Mach-O files: /' 
echo "terminal.sh: $app ($(du -sh "$app" | cut -f1))"
