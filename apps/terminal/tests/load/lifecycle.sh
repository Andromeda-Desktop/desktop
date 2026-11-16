#!/usr/bin/env bash
# Leaks and teardown, ours only. Kill gates: closing a session, a window or the app, SIGTERM and SIGKILL end every
# shell descendant (fg flood, bg jobs) with no zombie; under HOLDER=1 a SIGKILLed app reattaches its live sessions.
# Leak gates: lifecycle loops return RSS within 5 % + 2 MiB and the fd count to the level after the warm-up; with
# ASAN=1 (BIN = the build-asan binary) LeakSanitizer reports no leak in our code at exit, and only fds are gated:
# ASan's quarantine (256 MiB) keeps freed memory resident.
# Env: PARTS ("kill leak"), LOOPS (scale, default 1 = 200 sessions, 50 windows, 100 themes).
source "$(dirname "$0")/lib.sh"
PARTS=${PARTS:-kill leak}
LOOPS=${LOOPS:-1}
if [[ ${ASAN:-0} == 1 ]]; then
  export ASAN_OPTIONS="detect_leaks=1:log_path=$OUT/asan:abort_on_error=0:fast_unwind_on_malloc=0"
  export LSAN_OPTIONS="suppressions=$HERE/lsan.supp:print_suppressions=0"
  export UBSAN_OPTIONS="print_stacktrace=1:log_path=$OUT/ubsan"
  rm -f "$OUT"/asan.* "$OUT"/ubsan.*
fi
load_boot
key() { run wtype -k Shift_L "$@"; }
new_session() { key -M ctrl -M shift t -m shift -m ctrl; }
close_session() { key -M ctrl -M shift w -m shift -m ctrl; }
new_window() { key -M ctrl -M shift n -m shift -m ctrl; }

# descendants PID: every live process under PID (pty children of the app, or of the holder).
descendants() {
  local p
  for p in $(pgrep -P "$1"); do echo "$p"; descendants "$p"; done
}
# gone PIDS...: within 2 s every pid has exited and was reaped (no zombie).
gone() {
  local left
  (( $# )) || { echo "no pids: the jobs never started"; return 1; }
  for _ in $(seq 40); do
    left=
    for p in "$@"; do [[ -e /proc/$p ]] && left+="$p($(awk '{print $3}' /proc/$p/stat 2> /dev/null)) "; done
    [[ -z $left ]] && return 0
    sleep 0.05
  done
  echo "still there: $left"
  return 1
}
# load_jobs: in the focused session start a bg sleep, a bg yes and a fg flood; print their pids after 1 s.
load_jobs() {
  local mark="$OUT/res/jobs-$RANDOM"
  term_type "sleep 1000 & yes > /dev/null & echo \$\$ > $mark; yes"
  "$ROOTDIR/tools/wait.sh" --file "$mark" --timeout 10 > /dev/null
  sleep 1
  local sh
  sh=$(cat "$mark")
  echo "$sh $(descendants "$sh" | tr '\n' ' ')"
}
# confirm: the close sheet has no default button (Return alone does nothing): Tab to Close, then Return.
confirm() { sleep 0.5; key -k ${CONFIRM_KEYS:-Tab -k Tab -k Return}; }

if [[ $PARTS == *kill* ]]; then
  term_start ours
  pids=$(load_jobs); echo "session pids: $pids"
  new_session; sleep 1
  pids2=$(load_jobs)
  close_session; confirm
  gate "close session ends its jobs" gone $pids2
  gate "other session kept" bash -c "kill -0 ${pids%% *}"
  run "$ANDROMEDA" msg window-close > /dev/null; confirm
  gate "close window ends its jobs" gone $pids
  gate "app exits after its last window" gone "$TPID"
  TPID=

  term_start ours
  pids=$(load_jobs)
  kill -TERM "$TPID"
  gate "SIGTERM ends the jobs" gone $pids
  gate "SIGTERM exits the app" gone "$TPID"
  TPID=

  term_start ours
  term_type "bash -c \"trap '' HUP; echo \\\$\\\$ > $OUT/res/stubborn; exec sleep 1000\" &"
  "$ROOTDIR/tools/wait.sh" --file "$OUT/res/stubborn" --timeout 10 > /dev/null
  stubborn=$(cat "$OUT/res/stubborn")
  pids=$(load_jobs)
  kill -KILL "$TPID"
  if [[ ${HOLDER:-0} == 1 ]]; then
    gate "SIGKILL keeps held sessions" bash -c "kill -0 ${pids%% *}"
    TPID=
    term_start ours
    res="$OUT/res/reattach"; rm -f "$res"
    # load_jobs left `yes` in the foreground: stop it, so the shell reads the next line.
    key -M ctrl c -m ctrl
    term_type "echo \$\$ > $res"
    gate "restart reattaches the held shell" bash -c "\"$ROOTDIR/tools/wait.sh\" --file $res --timeout 10 > /dev/null && [[ \$(cat $res) == ${pids%% *} ]]"
  else
    gate "SIGKILL ends the jobs (kernel hangup)" gone $pids
    if [[ -e /proc/$stubborn ]]; then echo "info: a child that ignores SIGHUP outlives the app ($stubborn)"; kill "$stubborn"; fi
    TPID=
  fi
  term_stop
fi

if [[ $PARTS == *leak* ]]; then
  term_start ours
  base_fd=$(fds "$TPID")
  steady() { # steady LABEL RSS0 FD0: RSS within 5 % + 2 MiB of RSS0 and fds == FD0
    local r f
    r=$(rss_kb "$TPID"); f=$(fds "$TPID")
    echo "  $1: rss $((r / 1024)) MiB (start $(($2 / 1024))), fds $f (start $3)"
    [[ ( ${ASAN:-0} == 1 || $r -le $(($2 * 105 / 100 + 2048)) ) && $f -le $3 ]] || { echo "rss $r kB vs $2, fds $f vs $3"; return 1; }
  }
  # shells: live shells of the app (children of the holder under HOLDER=1). Each step waits for its count, so a
  # slow build (ASan) never closes the only session.
  shells() { pgrep -c -P "$([[ ${HOLDER:-0} == 1 ]] && echo "$HOLDER_PID" || echo "$TPID")" || true; }
  until_shells() { for _ in $(seq "${2:-600}"); do [[ $(shells) -eq $1 ]] && return 0; sleep 0.05; done; return 1; }
  # press KEYFN WANT: a new virtual keyboard can lose its keys while the app is busy (finding F4): retry up to 3 times
  # and count the retries.
  RETRIES=0
  press() {
    for _ in 1 2 3; do
      "$1"
      until_shells "$2" 200 && return 0
      RETRIES=$((RETRIES + 1))
    done
    until_shells "$2" || { echo "shells $(shells), want $2" >&2; return 1; }
  }
  cycle_sessions() {
    local n
    n=$(shells)
    for _ in $(seq "$1"); do press new_session $((n + 1)); press close_session "$n"; done
  }
  cycle_windows() {
    local n
    n=$(shells)
    for _ in $(seq "$1"); do press new_window $((n + 1)); sleep 0.3; press close_session "$n"; sleep 0.3; done
  }
  cycle_themes() {
    for i in $(seq "$1"); do
      printf '[appearance]\ntheme = "%s"\n[text]\nsize = 13\n' "$( ((i % 2)) && echo light || echo dark)" > "$CFG/terminal.toml"
      sleep 0.2
    done
  }
  cycle_scrollback() {
    for i in $(seq "$1"); do
      term_do "$PROBE run $CORPUS/ascii $OUT/res/sb-$i" "$OUT/res/sb-$i" 120
      term_type "printf '\\e[3J'; clear"
    done
  }
  cycle_images() {
    for i in $(seq "$1"); do
      term_do "$PROBE run $CORPUS/kitty_images $OUT/res/img-$i" "$OUT/res/img-$i" 120
      term_type "printf '\\e_Ga=d\\e\\\\'; clear"
    done
  }
  for loop in "sessions 20 $((200 * LOOPS))" "windows 5 $((50 * LOOPS))" "themes 10 $((100 * LOOPS))" "scrollback 1 4" "images 1 3"; do
    set -- $loop
    echo "loop $1: warm-up $2, measured $3"
    "cycle_$1" "$2"; sleep 2
    r0=$(rss_kb "$TPID"); f0=$(fds "$TPID")
    "cycle_$1" "$3"; sleep 3
    gate "leak $1 x$3" steady "$1" "$r0" "$f0"
  done
  echo "info: shortcut retries (lost keys, F4): $RETRIES"
  gate "alive after lifecycle" alive
  if [[ ${ASAN:-0} == 1 ]]; then
    run "$ANDROMEDA" msg window-close > /dev/null
    "$ROOTDIR/tools/wait.sh" --pid "$TPID" --timeout 60 > /dev/null || true
    TPID=
    lsan() {
      local f n=0
      for f in "$OUT"/asan.* "$OUT"/ubsan.*; do [[ -e $f ]] && n=$((n + $(grep -cE 'ERROR: (Address|Leak)Sanitizer|runtime error:' "$f"))); done
      [[ $n -eq 0 ]] || { echo "$n sanitizer reports in $OUT/asan.* $OUT/ubsan.*"; return 1; }
    }
    gate "sanitizers clean at exit" lsan
  fi
  term_stop
fi
echo "FAILS $FAILS"
exit $((FAILS > 0))
