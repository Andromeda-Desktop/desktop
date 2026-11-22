# Andromeda Terminal shell integration for zsh. The terminal points ZDOTDIR here; this file restores the user's
# ZDOTDIR (zsh then reads .zprofile and .zshrc from there), sources the user's .zshenv, then adds the hooks.
if [[ -n "${ANDROMEDA_ZSH_ZDOTDIR+x}" ]]; then
  ZDOTDIR="$ANDROMEDA_ZSH_ZDOTDIR"
else
  builtin unset ZDOTDIR
fi
builtin unset ANDROMEDA_ZSH_ZDOTDIR
() {
  local userenv="${ZDOTDIR:-$HOME}/.zshenv"
  [[ -r "$userenv" ]] && builtin source "$userenv"
}
if [[ -o interactive ]]; then
  builtin source "${${(%):-%x}:A:h}/andromeda.zsh"
fi
