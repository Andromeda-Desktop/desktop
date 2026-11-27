#!/usr/bin/env bash
# Installs libdesk's public headers into one tree: install-headers.sh INCDIR SRC... --protocols DIR... --generated H...
set -euo pipefail
inc="${MESON_INSTALL_DESTDIR_PREFIX}/$1"; shift
mode=src
for a in "$@"; do
  case $a in
    --protocols|--generated) mode=${a#--}; continue ;;
  esac
  case $mode in
    src) (cd "$a" && find . -name '*.h' -print0 | xargs -0 -I{} install -Dm644 {} "$inc/{}") ;;
    protocols) find -L "$a" -maxdepth 1 -name '*.h' -exec install -Dm644 -t "$inc/protocols" {} + ;;
    generated) [[ $a == *.h ]] && install -Dm644 -t "$inc" "$a" ;;
  esac
done
