#!/bin/sh
# ssh from Andromeda Terminal. A host without the xterm-andromeda terminfo gets it once: the first login compiles it
# into the remote ~/.terminfo and stays open as a control connection, so the session after it asks for no second
# login. A remote command, an unknown host or a failed install runs with TERM=xterm-256color instead.
[ "$TERM" = xterm-andromeda ] || exec ssh "$@"

# True when a remote command follows the destination (it would get the install script as arguments).
has_command() {
  while [ $# -gt 0 ]; do
    case $1 in
      -[BbcDEeFIiJLlmOoPpRSWw]) shift ;;
      --) shift; [ $# -gt 1 ]; return ;;
      -*) ;;
      *) [ $# -gt 1 ]; return ;;
    esac
    [ $# -gt 0 ] && shift
  done
  return 1
}

cache="${XDG_CACHE_HOME:-$HOME/.cache}/andromeda/terminfo-hosts"
target=$(ssh -G "$@" 2> /dev/null | awk '$1 == "user" { u = $2 } $1 == "hostname" { h = $2 } $1 == "port" { p = $2 } END { if (h != "") print u "@" h ":" p }')
[ -n "$target" ] && grep -qxF "$target" "$cache" 2> /dev/null && exec ssh "$@"
if [ -z "$target" ] || has_command "$@" || ! command -v infocmp > /dev/null 2>&1; then
  TERM=xterm-256color exec ssh "$@"
fi
dir=$(mktemp -d) || TERM=xterm-256color exec ssh "$@"
if infocmp -x xterm-andromeda 2> /dev/null | ssh -o ControlMaster=yes -o ControlPath="$dir/c" -o ControlPersist=60 "$@" \
  'infocmp xterm-andromeda > /dev/null 2>&1 && exit 0; command -v tic > /dev/null 2>&1 || exit 1; mkdir -p ~/.terminfo && tic -x - > /dev/null 2>&1' 2> /dev/null; then
  mkdir -p "${cache%/*}" && printf '%s\n' "$target" >> "$cache"
  ssh -o ControlPath="$dir/c" "$@"
else
  TERM=xterm-256color ssh -o ControlPath="$dir/c" "$@"
fi
status=$?
rm -rf "$dir"
exit "$status"
