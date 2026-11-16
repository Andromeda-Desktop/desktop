# Sourced by the terminal load suites: a headless Andromeda session, any terminal under test, metric snapshots.
# Env: TERMS (ours foot kitty ghostty), BIN (our binary; copied, other sessions relink the build), HOLDER=1 (run
# through andromeda-ptyd, as the real session does), CORPUS (corpus dir), MIB (corpus size), OUT (logs, results).
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOTDIR=$(cd "$HERE/../../../.." && pwd)
OUT=${OUT:-${XDG_CACHE_HOME:-$HOME/.cache}/term-load/run}
CORPUS=${CORPUS:-${XDG_CACHE_HOME:-$HOME/.cache}/term-load-corpus}
MIB=${MIB:-32}
PROBE="python3 -I $HERE/probe.py"
source "$ROOTDIR/tests/e2e/lib.sh"
mkdir -p "$OUT/res"
OURS="$OUT/andromeda-terminal"
install -m 755 "${BIN:-$ROOTDIR/apps/terminal/build/andromeda-terminal}" "$OURS"
TCK=$(getconf CLK_TCK)
CRASH='AddressSanitizer|LeakSanitizer|runtime error:|Segmentation fault|Aborted|terminate called|panicked|assertion .* failed|SIGSEGV|SIGABRT'

load_boot() {
  $PROBE gen "$CORPUS" "$MIB"
  boot_headless 1
  trap 'term_stop; pkill -P "${KBD_PID:-0}" wtype || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
  # Headless has no real keyboard: each wtype would add and remove the seat keyboard, and keys sent before the app
  # binds it are lost. One idle virtual keyboard keeps it, as a physical keyboard does.
  # An idle wtype never sees the compositor go: the trap stops it (KBD_PID is the subshell that runs it).
  run wtype -s 86400000 > /dev/null 2>&1 &
  KBD_PID=$!
  CFG="$RUNTIME/home/.config/andromeda"
  mkdir -p "$CFG"
  if [[ ${HOLDER:-0} == 1 ]]; then
    mkdir -p "$RUNTIME/andromeda"
    run "${PTYD:-$ROOTDIR/apps/terminal/ptyd/target/release/andromeda-ptyd}" --socket "$RUNTIME/andromeda/ptyd-1.sock" > "$OUT/ptyd.log" 2>&1 &
    "$ROOTDIR/tools/wait.sh" --file "$RUNTIME/andromeda/ptyd-1.sock" --timeout 10 > /dev/null || [[ -S $RUNTIME/andromeda/ptyd-1.sock ]]
    HOLDER_PID=$!
  fi
}

app_id() {
  case $1 in
    ours) echo dev.andromeda.Terminal ;;
    foot) echo foot ;;
    kitty) echo kitty ;;
    ghostty) echo com.mitchellh.ghostty ;;
  esac
}

mapped() { run "$ANDROMEDA" windows --json | jq -e --arg a "$1" '[.. | objects | select(.app_id? == $a)] | length > 0' > /dev/null; }

# term_start ID: start one terminal on bash with no rc files, 13 px text and ~10 MB of scrollback, maximized.
# Sets TPID, TLOG, START_MS (launch to first map).
term_start() {
  TID=$1
  TLOG="$OUT/$TID.log"
  local sh=(env SHELL=/bin/bash PS1='$ ' HISTFILE=/dev/null)
  case $TID in
    ours)
      printf '[text]\nsize = 13\n' > "$CFG/terminal.toml"
      run "${sh[@]}" "$OURS" > "$TLOG" 2>&1 &
      ;;
    foot) run "${sh[@]}" foot -o main.shell=/bin/bash -o font=monospace:pixelsize=13 -o scrollback.lines=100000 > "$TLOG" 2>&1 & ;;
    kitty) run "${sh[@]}" kitty -o shell=/bin/bash -o font_size=9.75 -o scrollback_lines=100000 -o update_check_interval=0 > "$TLOG" 2>&1 & ;;
    ghostty) run "${sh[@]}" ghostty --command=/bin/bash --font-size=9.75 --gtk-single-instance=false > "$TLOG" 2>&1 & ;;
  esac
  TPID=$!
  local t0
  t0=$(date +%s%N)
  for _ in $(seq 500); do mapped "$(app_id "$TID")" && break; sleep 0.02; done
  START_MS=$((($(date +%s%N) - t0) / 1000000))
  if ! mapped "$(app_id "$TID")"; then
    echo "$TID did not map; threads (name wchan state):" >&2
    local t
    for t in /proc/$TPID/task/*; do echo "  $(< "$t/comm") $(< "$t/wchan") $(awk '{print $3}' "$t/stat")" >&2; done
    return 1
  fi
  # The launcher can exec or fork; take the newest process of that binary under our runtime.
  if [[ $TID == ours ]]; then TPID=$(pgrep -n -f -- "$OURS"); else TPID=$(pgrep -n -x "$TID"); fi
  run "$ANDROMEDA" msg window-toggle-maximize > /dev/null || true
  sleep 1
  term_type 'stty -echoctl; clear'
}

term_stop() {
  [[ -n ${TPID:-} ]] && kill "$TPID" 2> /dev/null && { "$ROOTDIR/tools/wait.sh" --pid "$TPID" --timeout 5 > /dev/null || kill -9 "$TPID" 2> /dev/null; }
  TPID=
  return 0
}

# term_type TEXT: type TEXT and Return into the focused terminal. The first key of a new virtual keyboard is lost:
# Shift_L goes first.
term_type() { run wtype -k Shift_L -s 20 "$1" -k Return; }

# term_do CMD RESULT TIMEOUT: type CMD, wait until it writes RESULT. Returns 124 on timeout.
term_do() {
  rm -f "$2"
  term_type "$1"
  "$ROOTDIR/tools/wait.sh" --file "$2" --timeout "$3" > /dev/null
}

gpu_ns() {
  local t=0 k v
  for f in /proc/"$1"/fdinfo/*; do
    while read -r k v _; do
      case $k in drm-engine-render: | drm-engine-rcs: | drm-cycles-rcs:) t=$((t + v)) ;; esac
    done < <(grep -s '^drm-' "$f" || true)
  done
  echo "$t"
}
cpu_ticks() { awk '{ print $14 + $15 }' /proc/"$1"/stat; }
rss_kb() { awk '/^VmRSS:/{print $2}' /proc/"$1"/status; }
hwm_kb() { awk '/^VmHWM:/{print $2}' /proc/"$1"/status; }
pss_kb() { awk '/^Pss:/{print $2}' /proc/"$1"/smaps_rollup; }
fds() { ls /proc/"$1"/fd | wc -l; }
threads() { awk '/^Threads:/{print $2}' /proc/"$1"/status; }

# alive: the terminal runs and its log has no crash line.
alive() {
  kill -0 "$TPID" 2> /dev/null || { echo "dead"; return 1; }
  if grep -qE "$CRASH" "$TLOG"; then echo "crash line: $(grep -m1 -E "$CRASH" "$TLOG")"; return 1; fi
}

# responsive: a DSR round trip on the idle terminal within 2 s.
responsive() {
  term_do "$PROBE dsr $OUT/res/dsr" "$OUT/res/dsr" 10 || { echo "no DSR result"; return 1; }
  local ns ok
  read -r ns ok < "$OUT/res/dsr"
  [[ $ok == ok && $ns -lt 2000000000 ]] || { echo "DSR $ok after $((ns / 1000000)) ms"; return 1; }
}

# quiet: 0 CPU ticks in the terminal over 5 s after 10 s without input or output.
quiet() {
  sleep 10
  local a
  a=$(cpu_ticks "$TPID")
  sleep 5
  local d=$(($(cpu_ticks "$TPID") - a))
  [[ $d -le 1 ]] || { echo "idle CPU $d ticks in 5 s"; return 1; }
}

FAILS=0
gate() { # gate NAME CMD...: print PASS/FAIL with the reason
  local why
  if why=$("${@:2}" 2>&1); then echo "PASS $1"; else echo "FAIL $1: $why"; FAILS=$((FAILS + 1)); fi
}

median() { sort -n | awk '{a[NR]=$1} END {print (NR % 2) ? a[(NR + 1) / 2] : int((a[NR / 2] + a[NR / 2 + 1]) / 2)}'; }
pct() { sort -n | awk -v p="$1" '{a[NR]=$1} END {i = int(NR * p / 100 + 0.999); if (i < 1) i = 1; print a[i]}'; }
