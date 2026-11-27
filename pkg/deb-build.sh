#!/usr/bin/env bash
# Usage (from debian/rules, at the source root): pkg/deb-build.sh build|install
# Ubuntu packages. Install steps come from the PKGBUILD's package_*() functions, so both distros share one install
# list. The build differs from PKGBUILD build(): no Link (it ships its own deb), and the update crates use apt.
set -euo pipefail
root=$PWD
srcdir=$root/debian/build
mkdir -p "$srcdir"
ln -sfn "$root" "$srcdir/desktop"

# The flags of Arch's arch-meson, except the wrap mode (the wraps download). --libdir=lib as on Arch: the fixed
# /usr/lib/andromeda paths and the PKGBUILD install steps expect it, not Debian's multiarch dir.
# A kept build dir (tools/ubuntu-deb.sh builds incrementally) is reconfigured, so changed flags still apply.
arch-meson() {
  local reconfigure=()
  [[ -f $2/build.ninja ]] && reconfigure=(--reconfigure)
  meson setup "${reconfigure[@]}" --prefix=/usr --libdir=lib --libexecdir=lib --sbindir=bin --buildtype=plain \
    --auto-features=enabled -Db_pie=true "$@"
}

# shellcheck source=PKGBUILD
source "$root/pkg/PKGBUILD"
cd "$srcdir"

# ANDROMEDA_APP=libdesk|files|terminal|player|viewer (app release CI): build and stage only that release unit
# (PKGBUILD's _app_units; libdesk is libdesk with every app). dpkg-buildpackage still writes every package of
# debian/control; the other debs stay empty and unpublished.
# ANDROMEDA_CORE=1 (core release CI): no apps; they release on their own tags. Their debs stay empty and unpublished.
apps=(files viewer player terminal)
[[ -z ${ANDROMEDA_CORE:-} ]] || apps=()
app_dirs=()
[[ -z ${ANDROMEDA_APP:-} ]] || app_dirs=("${_app_units[@]}")

case ${1:-} in
  build)
    # Ubuntu has no zig 0.17: the script fetches the pinned one.
    [[ -n ${ANDROMEDA_CORE:-} ]] || ! _needs_libghostty || desktop/tools/build-libghostty.sh
    if [[ -n ${ANDROMEDA_APP:-} ]]; then
      for dir in "${app_dirs[@]}"; do
        arch-meson "desktop/apps/$dir" "build-$dir" -Dcpp_std=c++23 -Db_lto=true -Dlibdesk-ui:dev_tokens=false
        meson compile -C "build-$dir"
      done
      exit 0
    fi
    # ANDROMEDA_ONLY="shell" (a unit release): build only those units, as the PKGBUILD's _want.
    want() { [[ -z ${ANDROMEDA_ONLY:-} || " $ANDROMEDA_ONLY " == *" $1 "* ]]; }
    # As PKGBUILD build(): the patched wlroots and libliftoff wraps.
    want compositor && arch-meson desktop/compositor build-compositor -Dcpp_std=c++23 -Dtests=disabled -Db_lto=true \
      --force-fallback-for=wlroots-0.20,libliftoff \
      -Dwlroots:libliftoff=enabled -Dlibliftoff:default_library=shared -Dlibliftoff:werror=false
    { want shell || want greeter; } && arch-meson desktop/shell build-shell -Dcpp_std=c++23 -Dtests=disabled -Db_lto=true -Dlibdesk-ui:dev_tokens=false
    want portal && arch-meson desktop/portal build-portal -Dcpp_std=c++23
    [[ -n ${ANDROMEDA_ONLY:-} ]] || arch-meson desktop/indexer build-indexer -Db_lto=true
    want preview && arch-meson desktop/andromeda-preview build-preview -Dcpp_std=c++23 -Db_lto=true -Dlibdesk-ui:dev_tokens=false
    for app in "${apps[@]}"; do
      arch-meson desktop/apps/$app build-$app -Dcpp_std=c++23 -Db_lto=true -Dlibdesk-ui:dev_tokens=false
    done
    for dir in compositor shell portal preview; do want $dir && meson compile -C build-$dir; done
    want shell || ! want greeter || meson compile -C build-shell tools/greeter-apply-appearance/andromeda-greeter-apply-appearance
    [[ -n ${ANDROMEDA_ONLY:-} ]] || meson compile -C build-indexer
    for app in "${apps[@]}"; do meson compile -C build-$app; done
    if want system; then
      cargo build --locked --release --manifest-path desktop/system/Cargo.toml --workspace \
        --exclude andromeda-update --exclude andromeda-update-helper --exclude andromeda-app-updater
      cargo build --locked --release --manifest-path desktop/system/Cargo.toml -p andromeda-update \
        -p andromeda-update-helper --no-default-features --features apt
    fi
    if want greetd; then
      # andromeda-greetd: the PKGBUILD source pin plus our patches (a kept tree is already patched).
      if [[ ! -d greetd ]]; then
        git clone -q https://git.sr.ht/~kennylevinsen/greetd greetd
        git -C greetd checkout -q "${source[1]##*#commit=}"
        for patch in desktop/session/greetd/patches/*.patch; do patch -d greetd -Np1 < "$patch"; done
      fi
      cargo build --locked --release --manifest-path greetd/Cargo.toml -p greetd
    fi
    ;;
  install)
    declare -A debs=(
      [andromeda]=andromeda
      [andromeda-compositor-git]=andromeda-compositor
      [andromeda-shell-git]=andromeda-shell
      [xdg-desktop-portal-andromeda-git]=xdg-desktop-portal-andromeda
      [andromeda-system-git]=andromeda-system
      [andromeda-power-git]=andromeda-power
      [andromeda-greeter-git]=andromeda-greeter
      [andromeda-greetd]=andromeda-greetd
      [andromeda-preview-git]=andromeda-preview
      [andromeda-files-git]=andromeda-files
      [andromeda-player-git]=andromeda-player
      [andromeda-viewer-git]=andromeda-viewer
      [andromeda-terminal-git]=andromeda-terminal
      [andromeda-libdesk-git]=andromeda-libdesk
    )
    if [[ -n ${ANDROMEDA_APP:-} ]]; then
      declare -A debs=()
      for fn in "${pkgname[@]}"; do debs[$fn]=${fn%-git}; done
    fi
    [[ -z ${ANDROMEDA_CORE:-} ]] || unset 'debs[andromeda-files-git]' 'debs[andromeda-player-git]' 'debs[andromeda-viewer-git]' 'debs[andromeda-terminal-git]' 'debs[andromeda-libdesk-git]'
    if [[ -n ${ANDROMEDA_ONLY:-} ]]; then
      declare -A wanted=()
      for fn in "${pkgname[@]}"; do wanted[$fn]=${debs[$fn]}; done
      unset debs
      declare -A debs
      for fn in "${!wanted[@]}"; do debs[$fn]=${wanted[$fn]}; done
    fi
    for fn in "${!debs[@]}"; do
      pkgname=${debs[$fn]}
      pkgdir=$root/debian/$pkgname
      mkdir -p "$pkgdir"
      "package_$fn"
      # Debian keeps licences in debian/copyright.
      rm -rf "$pkgdir/usr/share/licenses"
    done
    [[ -z ${ANDROMEDA_APP:-} ]] || exit 0
    # Debian PAM stacks: common-* replace Arch's system-login, system-local-login and system-auth.
    sed -Ei 's/^(account|password|session)\s+include\s+system-login$/@include common-\1/' "$root"/debian/*/etc/pam.d/*
    sed -Ei 's/^(auth|account|password|session)\s+include\s+system-(local-login|auth)$/@include common-\1/' \
      "$root"/debian/*/etc/pam.d/*
    # The pacman hook's apt equal is the dpkg trigger in debian/andromeda-system.triggers.
    [[ ! -d $root/debian/andromeda-system/usr/share/libalpm ]] || rm -r "$root/debian/andromeda-system/usr/share/libalpm"
    ;;
  *)
    echo "usage: $0 build|install" >&2
    exit 2
    ;;
esac
