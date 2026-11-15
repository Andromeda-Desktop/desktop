# Andromeda Terminal shell integration for bash: OSC 133 command marks with the command line, OSC 7 cwd.
# The terminal starts `bash --posix -i` with ENV set to this file: POSIX mode reads only $ENV, so this file first
# restores the normal startup files, then adds the hooks. The user's rc files stay untouched.
if [[ -n "${ANDROMEDA_BASH_INJECT-}" ]]; then
  builtin set +o posix
  builtin unset ENV
  if [[ "$ANDROMEDA_BASH_INJECT" == login ]]; then
    builtin unset ANDROMEDA_BASH_INJECT
    [[ -r /etc/profile ]] && builtin source /etc/profile
    for __andromeda_rc in ~/.bash_profile ~/.bash_login ~/.profile; do
      if [[ -r "$__andromeda_rc" ]]; then
        builtin source "$__andromeda_rc"
        break
      fi
    done
    builtin unset __andromeda_rc
  else
    builtin unset ANDROMEDA_BASH_INJECT
    [[ -r ~/.bashrc ]] && builtin source ~/.bashrc
  fi
fi

if [[ $- == *i* && -z "${__andromeda_hooked-}" ]]; then
  __andromeda_hooked=1

  # Percent-encodes $1 into REPLY (bytes outside the unreserved set and '/').
  __andromeda_urlencode() {
    local LC_ALL=C text="$1" out="" c i
    for ((i = 0; i < ${#text}; i++)); do
      c="${text:i:1}"
      case "$c" in
        [a-zA-Z0-9._~/-]) out+="$c" ;;
        *) builtin printf -v c '%%%02X' "'$c"; out+="$c" ;;
      esac
    done
    REPLY="$out"
  }

  # Runs from PS0 in a command substitution: it prints, it cannot set variables.
  __andromeda_preexec() {
    local line
    line="$(HISTTIMEFORMAT='' builtin history 1)"
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line#*[[:space:]]}"
    line="${line#"${line%%[![:space:]]*}"}"
    __andromeda_urlencode "$line"
    builtin printf '\e]133;C;cmdline_url=%s\a' "$REPLY"
  }

  # First in PROMPT_COMMAND, so $? is the command's status; it returns that status for the hooks after it.
  __andromeda_precmd() {
    local ret=$?
    if [[ -n "${__andromeda_prompted-}" ]]; then
      builtin printf '\e]133;D;%s\a' "$ret"
    fi
    __andromeda_prompted=1
    __andromeda_urlencode "$PWD"
    builtin printf '\e]7;file://%s%s\a\e]133;A\a' "${HOSTNAME-}" "$REPLY"
    return "$ret"
  }

  # A plain prompt becomes a blank row (the terminal draws the context pills there) and `λ  `. A styled or generated
  # prompt (colours, command substitution, starship) stays: then the terminal draws no pills.
  # STARSHIP_SHELL is inherited from a parent shell, so starship counts only when its functions are here.
  if [[ "$(builtin compgen -A function)" != *starship* && "$PS1" != *'$('* && "$PS1" != *'`'* && "$PS1" != *'\e['* && "$PS1" != *'\033['* ]]; then
    PS1='\n''λ  \[\e]133;B\a\]'
  fi
  PS0='$(__andromeda_preexec)'"${PS0-}"
  PROMPT_COMMAND="__andromeda_precmd${PROMPT_COMMAND:+; $PROMPT_COMMAND}"
fi
