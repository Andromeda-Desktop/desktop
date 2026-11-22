# Andromeda Terminal shell integration for bash: OSC 133 command marks with the command line, OSC 7 cwd, git branch (OSC 1337 SetUserVar).
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

  # The git branch of $PWD into REPLY (short hash when detached; empty outside a repository). Builtins only.
  __andromeda_branch() {
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
          REPLY="${head:0:7}"
        fi
        return
      fi
      [[ -z "$dir" ]] && return
      dir="${dir%/*}"
    done
  }

  # Base64 of $1 into REPLY.
  __andromeda_base64() {
    local LC_ALL=C text="$1" abc=ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/ out="" a b c v
    local -i i n=${#1}
    for ((i = 0; i < n; i += 3)); do
      builtin printf -v a '%d' "'${text:i:1}"
      b=0 c=0
      ((i + 1 < n)) && builtin printf -v b '%d' "'${text:i+1:1}"
      ((i + 2 < n)) && builtin printf -v c '%d' "'${text:i+2:1}"
      ((v = (a & 255) << 16 | (b & 255) << 8 | (c & 255)))
      out+="${abc:v >> 18 & 63:1}${abc:v >> 12 & 63:1}"
      if ((i + 1 < n)); then out+="${abc:v >> 6 & 63:1}"; else out+="="; fi
      if ((i + 2 < n)); then out+="${abc:v & 63:1}"; else out+="="; fi
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
    __andromeda_branch
    __andromeda_base64 "$REPLY"
    builtin printf '\e]1337;SetUserVar=git_branch=%s\a' "$REPLY"
    return "$ret"
  }

  # A plain prompt becomes a blank row (the terminal draws the context pills there) and three blank cells (the terminal
  # draws the brand mark in the first). A styled or generated
  # prompt (colours, command substitution, starship) stays: then the terminal draws no pills.
  # STARSHIP_SHELL is inherited from a parent shell, so starship counts only when its functions are here.
  if [[ "$(builtin compgen -A function)" != *starship* && "$PS1" != *'$('* && "$PS1" != *'`'* && "$PS1" != *'\e['* && "$PS1" != *'\033['* ]]; then
    PS1='\n''   \[\e]133;B\a\]'
  fi
  # ssh copies the terminfo to a host that lacks it (andromeda-ssh.sh); a user's own ssh function stays.
  if ! builtin declare -F ssh > /dev/null; then
    __andromeda_ssh_script="${BASH_SOURCE[0]%/*}/../andromeda-ssh.sh"
    function ssh { command sh "$__andromeda_ssh_script" "$@"; }
  fi
  PS0='$(__andromeda_preexec)'"${PS0-}"
  PROMPT_COMMAND="__andromeda_precmd${PROMPT_COMMAND:+; $PROMPT_COMMAND}"
fi
