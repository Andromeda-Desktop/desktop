# Sourced: the one way a test starts a headless Andromeda compositor (tests/e2e/lib.sh, the compositor harness and the
# scripts that need their own instance).
HEADLESS_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P) # canonical: --trust-binary matches /proc/<pid>/exe

# headless_compositor RUNTIME OUTPUTS CONFIG [-uVAR | VAR=VALUE]... [-- WRAPPER...]: execs ${ANDROMEDA} headless with
# RUNTIME as XDG_RUNTIME_DIR, OUTPUTS headless outputs and CONFIG; binaries under the repo, /usr/bin and RUNTIME (test
# helpers built there) are trusted. Run it in the background (`headless_compositor ... &`):
# the backgrounded subshell execs, so $! is the compositor (or WRAPPER, which must exec it). The caller redirects its
# output. Later VAR=VALUE items override the defaults (WLR_BACKENDS=wayland for a nested run).
headless_compositor() {
  local runtime=$1 outputs=$2 config=$3
  shift 3
  local unset=(-u WAYLAND_DISPLAY -u DISPLAY) assign=() wrapper=()
  while (($#)); do
    case $1 in
      --) shift; wrapper=("$@"); break ;;
      -u*) unset+=("$1") ;;
      *) assign+=("$1") ;;
    esac
    shift
  done
  exec env "${unset[@]}" XDG_RUNTIME_DIR="$runtime" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
    WLR_HEADLESS_OUTPUTS="$outputs" "${assign[@]}" "${wrapper[@]}" \
    "${ANDROMEDA:?}" -c "$config" --trust-binary "$HEADLESS_ROOT/" --trust-binary /usr/bin/ --trust-binary "$runtime/"
}

# wait_compositor PID PATH [TIMEOUT_S]: waits until the socket PATH exists. Returns 1 when PID exits first, 124 on timeout
# (default 10 s).
wait_compositor() {
  local pid=$1 path=$2 ticks=$((${3:-10} * 100))
  while [[ ! -S $path ]]; do
    kill -0 "$pid" 2> /dev/null || return 1
    ((ticks-- > 0)) || return 124
    sleep 0.01
  done
}
