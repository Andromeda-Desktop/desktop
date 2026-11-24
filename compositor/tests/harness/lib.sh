# Helpers shared by harness checks. A check sources it with: source "$ANDROMEDA_HARNESS_LIB"

# Runs the pointer helper through a sequence and holds the resulting input state (a pressed button, a held modifier)
# until pointer_release: pointer_hold <width> <height> <command>... [-- <command>...]. It returns once every command
# before `--` has been processed; the commands after `--` run on release. One hold at a time.
pointer_hold() {
  local width=$1 height=$2
  shift 2
  local -a before=()
  while (($# > 0)) && [[ $1 != -- ]]; do
    before+=("$1")
    shift
  done
  (($# > 0)) && shift
  local fifo=$ANDROMEDA_RUNTIME_DIR/pointer-hold.fifo
  POINTER_HOLD_LOG=$ANDROMEDA_RUNTIME_DIR/pointer-hold.log
  rm -f "$fifo"
  mkfifo "$fifo"
  "$ANDROMEDA_POINTER_CLIENT" "$width" "$height" "${before[@]}" mark held hold "$@" < "$fifo" > "$POINTER_HOLD_LOG" 2>&1 &
  POINTER_HOLD_PID=$!
  exec {POINTER_HOLD_FD}> "$fifo"
  for _ in $(seq 200); do
    grep -q '^held$' "$POINTER_HOLD_LOG" && return 0
    sleep 0.025
  done
  echo "the pointer helper never reached its hold: $(< "$POINTER_HOLD_LOG")"
  return 1
}

# Several holds in one run: put `mark <label> hold` among the commands after `--`, and pointer_step <label> runs the
# helper on to that hold.
pointer_step() {
  echo >&"$POINTER_HOLD_FD"
  for _ in $(seq 200); do
    grep -q "^$1\$" "$POINTER_HOLD_LOG" && return 0
    sleep 0.025
  done
  echo "the pointer helper never reached $1: $(< "$POINTER_HOLD_LOG")"
  return 1
}

# Runs the remaining commands after `--` and waits for the helper to exit.
pointer_release() {
  echo >&"$POINTER_HOLD_FD"
  exec {POINTER_HOLD_FD}>&-
  wait "$POINTER_HOLD_PID"
}

# The window observer suffixes its enter with the held-key count, so events are matched as line prefixes.
events() {
  local count
  count=$(grep -c "^$2" "$1" 2>/dev/null) || true
  echo "${count:-0}"
}

await_events() {
  local file=$1 event=$2 expected=$3 label=${4:-$1}
  for _ in $(seq 60); do
    (($(events "$file" "$event") >= expected)) && return 0
    sleep 0.1
  done
  echo "timed out waiting for $expected '$event' on $label: $(tr '\n' '|' < "$file")"
  return 1
}

# Waits for at least $3 lines matching $2 in $1, then returns. Poll step is $4 (default 0.1s), matching
# await_events; a check whose waits are tight under parallel stress passes a shorter step.
await_lines() {
  local file=$1 pattern=$2 expected=$3 step=${4:-0.1}
  for _ in $(seq 60); do
    (($(events "$file" "$pattern") >= expected)) && return 0
    sleep "$step"
  done
  echo "timed out waiting for $expected '$pattern' in $file: $(tr '\n' '|' < "$file")"
  return 1
}

# Owner of every "wait for the compositor state" loop: await_until TRIES STEP CMD... runs CMD every STEP seconds until it
# succeeds. The owners below poll `andromeda windows --json` for up to 15 s and print the observed state on timeout.
AWAIT_TRIES=600
AWAIT_STEP=0.025
await_until() {
  local tries=$1 step=$2
  shift 2
  for ((; tries > 0; tries--)); do
    "$@" && return 0
    sleep "$step"
  done
  return 1
}
_aw_windows() { "$ANDROMEDA" windows --json; }
_aw_match() { _aw_windows | jq -e "$1" > /dev/null; }
_aw_field() {
  _aw_windows | jq -r --arg t "$1" --arg f "$2" '[.[] | select(.title == $t) | .[$f]] | if length == 0 then "<absent>" else .[0] end'
}
_aw_has() { _aw_windows | jq -e --arg k "$2" "any(.[]; .$1 == \$k${3:+ and $3})" > /dev/null; }
_aw_field_is() { [[ $(_aw_field "$1" "$2") == "$3" ]]; }
_aw_count_is() { [[ $(_aw_windows | jq length) -eq $1 ]]; }
_aw_log_since() { tail -n +"$(($1 + 1))" "$ANDROMEDA_LOG" | grep -q "$2"; }
_aw_log_count() { (($(grep -c "$2" "$1" 2> /dev/null || true) >= ${3:-1})); }

# Waits until the jq filter $1 holds for the windows array; $2 names the failure.
await_windows() {
  await_until "$AWAIT_TRIES" "$AWAIT_STEP" _aw_match "$1" && return 0
  echo "$2: $(_aw_windows)"
  return 1
}
await_count() {
  await_until "$AWAIT_TRIES" "$AWAIT_STEP" _aw_count_is "$1" && return 0
  echo "expected $1 windows, got: $(_aw_windows)"
  return 1
}
# await_field TITLE FIELD EXPECTED: the window with that exact title has field == EXPECTED (compared as text).
await_field() {
  await_until "$AWAIT_TRIES" "$AWAIT_STEP" _aw_field_is "$1" "$2" "$3" && return 0
  echo "expected '$1' field '$2' to be '$3', got '$(_aw_field "$1" "$2")': $(_aw_windows)"
  return 1
}
await_window() {
  await_until "$AWAIT_TRIES" "$AWAIT_STEP" _aw_has title "$1" && return 0
  echo "missing window: $1: $(_aw_windows)"
  return 1
}
await_active() {
  await_until "$AWAIT_TRIES" "$AWAIT_STEP" _aw_field_is "$1" active true && return 0
  echo "expected '$1' to be active: $(_aw_windows)"
  return 1
}
# await_focus ID: the window with that id holds its workspace focus.
await_focus() {
  await_until "$AWAIT_TRIES" "$AWAIT_STEP" _aw_has id "$1" .focused && return 0
  echo "expected $1 to be focused: $(_aw_windows)"
  return 1
}
await_workspace() {
  await_until "$AWAIT_TRIES" "$AWAIT_STEP" _aw_field_is "$1" workspace "$2" && return 0
  echo "expected '$1' on '$2', got '$(_aw_field "$1" workspace)'"
  return 1
}
# Waits for a line matching $2 in $ANDROMEDA_LOG after its first $1 lines (a mark taken with wc -l).
await_log_since() {
  await_until "$AWAIT_TRIES" "$AWAIT_STEP" _aw_log_since "$1" "$2" && return 0
  echo "timed out waiting for log: $2"
  tail -8 "$ANDROMEDA_LOG" | sed 's/^/  | /'
  return 1
}
# Waits for at least $3 (default 1) lines matching $2 anywhere in file $1.
await_log_count() {
  await_until "$AWAIT_TRIES" "$AWAIT_STEP" _aw_log_count "$1" "$2" "${3:-1}" && return 0
  echo "expected ${3:-1} '$2' lines in $1: $(tr '\n' '|' < "$1" 2> /dev/null || true)"
  return 1
}
# Query a mapped window by its exact title.
window_by_title() { "$ANDROMEDA" windows --json | jq -ce --arg title "$1" '.[] | select(.title == $title)'; }
await_window_closed() {
  for _ in $(seq 100); do
    window_by_title "$1" > /dev/null || return 0
    sleep 0.025
  done
  echo "window still open: $1"; return 1
}

# Shared fixture for live workspace reveal checks.
workspace_reveal_config() {
  cp "$ANDROMEDA_REPO/examples/effects/animation/workspace_wipe/shader.glsl" "$ANDROMEDA_RUNTIME_DIR/wipe.glsl"
  cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = true
[animation.windows_in]
enabled = false
[animation.windows_out]
enabled = false
[animation.windows_move]
enabled = false
[animation.workspaces]
style = "reveal"
effect = "wipe"
duration_ms = 1000
curve = "linear"
[effects.preset.wipe]
kind = "animation"
shader = "wipe.glsl"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
CONFIG
}
