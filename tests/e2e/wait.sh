# Sourced: the one owner of e2e waits. No side effects, so scripts without lib.sh source it too.
# wait_for DESCRIPTION COMMAND...: run COMMAND every 0.1 s until it succeeds. After WAIT_S seconds (default 10) shoot
# $OUT/wait-fail.png and fail the test: through the script's fail() when it has one. Exported into with_noctalia.
wait_for() {
  local what=$1
  shift
  for _ in $(seq $((${WAIT_S:-10} * 10))); do
    "$@" && return 0
    sleep 0.1
  done
  [[ -z ${OUT:-} ]] || { declare -F run > /dev/null && run grim "$OUT/wait-fail.png" || grim "$OUT/wait-fail.png"; } 2> /dev/null || true
  declare -F fail > /dev/null && fail "$what"
  echo "FAIL: $what"
  exit 1
}
# poll_for SECONDS COMMAND...: as wait_for, but return 1 when the time runs out.
poll_for() {
  local tries=$(($1 * 10))
  shift
  for _ in $(seq "$tries"); do
    "$@" && return 0
    sleep 0.1
  done
  return 1
}
# log_since FILE FROM REGEX [COUNT]: FILE has at least COUNT (default 1) lines matching the extended REGEX after its
# first FROM lines.
log_since() { (($(tail -n +"$(($2 + 1))" "$1" | grep -cE -- "$3") >= ${4:-1})); }
# wait_log FROM REGEX [SECONDS]: as poll_for SECONDS (default 5) log_since "$LOG" FROM REGEX.
wait_log() { poll_for "${3:-5}" log_since "$LOG" "$1" "$2"; }
export -f wait_for poll_for log_since wait_log
