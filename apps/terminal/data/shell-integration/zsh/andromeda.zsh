# Andromeda Terminal hooks for zsh: OSC 133 command marks with the command line, OSC 7 cwd.
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

__andromeda_precmd() {
  local ret=$?
  if (( ${+__andromeda_running} )); then
    builtin unset __andromeda_running
    builtin print -rn -- $'\e]133;D;'"$ret"$'\a'
  fi
  if (( ! ${+__andromeda_prompt_set} )); then
    __andromeda_prompt_set=1
    # The first prompt runs after .zshrc: only a plain prompt becomes the blank row and `λ  `.
    if [[ "${(k)functions}" != *starship* && "$PS1" != *'$('* && "$PS1" != *$'\e['* && "$PS1" != *'%F'* && "$PS1" != *'%{'* ]]; then
      PS1=$'\n''λ  %{'$'\e]133;B\a''%}'
    fi
  fi
  __andromeda_urlencode "$PWD"
  builtin print -rn -- $'\e]7;file://'"${HOST-}$REPLY"$'\a\e]133;A\a'
  return $ret
}

add-zsh-hook preexec __andromeda_preexec
precmd_functions=(__andromeda_precmd ${precmd_functions:#__andromeda_precmd})
