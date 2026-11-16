# Andromeda Terminal hooks for zsh: OSC 133 command marks with the command line, OSC 7 cwd, git branch (OSC 1337 SetUserVar).
autoload -Uz add-zsh-hook

# Percent-encodes $1 into REPLY (bytes outside the unreserved set and '/').
__andromeda_urlencode() {
  emulate -L zsh
  local LC_ALL=C text="$1" out="" c
  local -i i
  for (( i = 1; i <= ${#text}; i++ )); do
    c="${text[i]}"
    case "$c" in
      [a-zA-Z0-9._~/-]) out+="$c" ;;
      *) out+="$(builtin printf '%%%02X' "'$c")" ;;
    esac
  done
  REPLY="$out"
}

__andromeda_preexec() {
  __andromeda_running=1
  __andromeda_urlencode "$1"
  builtin print -rn -- $'\e]133;C;cmdline_url='"$REPLY"$'\a'
}

# The git branch of $PWD into REPLY (short hash when detached; empty outside a repository). Builtins only.
__andromeda_branch() {
  emulate -L zsh
  local dir="$PWD" git head
  REPLY=""
  while :; do
    git=""
    if [[ -d "$dir/.git" ]]; then
      git="$dir/.git"
    elif [[ -f "$dir/.git" ]]; then
      builtin read -r git < "$dir/.git" || return
      [[ "$git" == "gitdir: "* ]] || return
      git="${git#gitdir: }"
      [[ "$git" == /* ]] || git="$dir/$git"
    fi
    if [[ -n "$git" ]]; then
      builtin read -r head < "$git/HEAD" 2> /dev/null || return
      if [[ "$head" == "ref: refs/heads/"* ]]; then
        REPLY="${head#ref: refs/heads/}"
      elif (( ${#head} >= 7 )); then
        REPLY="${head[1,7]}"
      fi
      return
    fi
    [[ -z "$dir" ]] && return
    dir="${dir%/*}"
  done
}

# Base64 of $1 into REPLY.
__andromeda_base64() {
  emulate -L zsh
  local LC_ALL=C text="$1" abc=ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/ out=""
  local ch
  local -i i a b c v n=${#1}
  for (( i = 1; i <= n; i += 3 )); do
    ch="${text[i]}"; a=$(( #ch )); b=0; c=0
    (( i + 1 <= n )) && { ch="${text[i+1]}"; b=$(( #ch )); }
    (( i + 2 <= n )) && { ch="${text[i+2]}"; c=$(( #ch )); }
    (( v = (a & 255) << 16 | (b & 255) << 8 | (c & 255) ))
    out+="${abc[(v >> 18 & 63) + 1]}${abc[(v >> 12 & 63) + 1]}"
    if (( i + 1 <= n )); then out+="${abc[(v >> 6 & 63) + 1]}"; else out+="="; fi
    if (( i + 2 <= n )); then out+="${abc[(v & 63) + 1]}"; else out+="="; fi
  done
  REPLY="$out"
}

__andromeda_precmd() {
  local ret=$?
  if (( ${+__andromeda_running} )); then
    builtin unset __andromeda_running
    builtin print -rn -- $'\e]133;D;'"$ret"$'\a'
  fi
  if (( ! ${+__andromeda_prompt_set} )); then
    __andromeda_prompt_set=1
    # The first prompt runs after .zshrc: only a plain prompt becomes the blank row and three blank cells.
    if [[ "${(k)functions}" != *starship* && "$PS1" != *'$('* && "$PS1" != *$'\e['* && "$PS1" != *'%F'* && "$PS1" != *'%{'* ]]; then
      PS1=$'\n''   %{'$'\e]133;B\a''%}'
    fi
  fi
  __andromeda_urlencode "$PWD"
  builtin print -rn -- $'\e]7;file://'"${HOST-}$REPLY"$'\a\e]133;A\a'
  __andromeda_branch
  __andromeda_base64 "$REPLY"
  builtin print -rn -- $'\e]1337;SetUserVar=git_branch='"$REPLY"$'\a'
  return $ret
}

add-zsh-hook preexec __andromeda_preexec
precmd_functions=(__andromeda_precmd ${precmd_functions:#__andromeda_precmd})
