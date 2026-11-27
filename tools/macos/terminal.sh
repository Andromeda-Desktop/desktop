#!/usr/bin/env bash
# Cross-builds "Andromeda Terminal.app" for arm64 macOS on Linux: terminal, ptyd, agentd, the dylibs they load,
# the data files, and an ad-hoc signature. Out: $OUT (default /var/tmp/andromeda-macos).
set -euo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
out=${OUT:-/var/tmp/andromeda-macos}
cross=$HOME/.cache/andromeda-mac/cross.ini
[ -f "$cross" ] || tools/macos/deps.py
sdk=$(sed -n "s/^cpp = .*'-isysroot', '\([^']*\)'.*/\1/p" "$cross")
ghostty_lib=$root/libdesk-term/libghostty-vt/prefix-macos/lib

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
res=$app/Contents/Resources
cp apps/terminal/build-macos/andromeda-terminal "$macos/"
cp /var/tmp/andromeda-macos-cargo/aarch64-apple-darwin/release/andromeda-{ptyd,agentd} "$macos/"

tools/macos/bundle-libdesk.sh "$app" "$ghostty_lib" "$macos/andromeda-terminal" "$macos/andromeda-ptyd" "$macos/andromeda-agentd"

mkdir -p "$res/assets" "$res/terminfo"
rsync -a --exclude '*-wallpaper.png' shell/assets/ "$res/assets/"
cp -r libdesk-term/data/shell-integration "$res/"
tic -x -o "$res/terminfo" apps/terminal/data/andromeda.terminfo
# macOS ncurses looks entries up under the hex name of their first letter.
# tic on a Mac writes them so already.
shopt -s nullglob
for dir in "$res/terminfo"/?; do mv "$dir" "$res/terminfo/$(printf '%x' "'${dir##*/}")"; done
shopt -u nullglob
mkdir -p "$res/translations/terminal"
cp apps/terminal/translations/en.json "$res/translations/terminal/"
tools/macos/icns.py apps/terminal/data/icons/dev.andromeda.Terminal.svg "$res/AppIcon.icns"

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

llvm-strip -S "$macos/andromeda-terminal"
# Signs every Mach-O in the bundle; install-name changes broke their old signatures. A Mac has codesign.
if command -v rcodesign > /dev/null; then
  rcodesign sign "$app" 2>&1 | grep -c 'signing Mach-O' | sed 's/^/terminal.sh: signed Mach-O files: /'
else
  codesign --force --deep --sign - "$app"
fi
echo "terminal.sh: $app ($(du -sh "$app" | cut -f1))"
