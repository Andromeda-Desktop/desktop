#!/usr/bin/env bash
# Model test for window presence (AR1): a seeded random sequence of workspace, minimize, fullscreen, lock and DPMS ops.
# After each op, every window's last dsk_surface_v1.presence equals the rule derived from IPC placement facts:
# hidden (2) when not visible, off (3) while locked or powered off, occluded (1) under another visible fullscreen
# window or off the output, else shown (0). At the end, a window that is not shown gets no frame callbacks.
# PRESENCE_SEED=<n> replays one sequence.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_BACKGROUND_CLIENT:-./build-debug/tests/background-client}"
readonly LOCK_CLIENT="${ANDROMEDA_LOCK_CLIENT:-./build-debug/tests/lock-client}"
readonly SEED="${PRESENCE_SEED:-$((SRANDOM % 100000))}"
readonly OPS=24
readonly TITLES=(model-a model-b model-c model-d)
RANDOM=$SEED

cat >> "$ANDROMEDA_CONFIG" <<'CONF'

[animation]
enabled = false
CONF
"$ANDROMEDA" msg config-reload > /dev/null

for title in "${TITLES[@]}"; do
  "$CLIENT" "$title" > "$ANDROMEDA_RUNTIME_DIR/$title.log" 2>&1 &
  await_events "$ANDROMEDA_RUNTIME_DIR/$title.log" mapped 1 "$title"
done
"$ANDROMEDA" settle

locked=0
dpms_off=0
lock_fd=""
lock_count=0
history=()

expected_all() {
  "$ANDROMEDA" windows --json | jq -r --argjson locked "$locked" --argjson off "$dpms_off" '
    . as $all
    | [$all[] | select(.visible and .fullscreen) | .title] as $fs
    | .[] | select(.title | startswith("model-"))
    | .title as $t
    | $t + " " + (
        if (.visible | not) then "2"
        elif ($locked == 1 or $off == 1) then "3"
        elif ($fs | map(select(. != $t)) | length) > 0 then "1"
        elif (.x + .w <= 0 or .y + .h <= 0 or .x >= 1280 or .y >= 720) then "1"
        else "0" end)' | sort
}

actual_all() {
  for title in "${TITLES[@]}"; do
    echo "$title $(grep '^presence ' "$ANDROMEDA_RUNTIME_DIR/$title.log" | tail -n 1 | cut -d' ' -f2)"
  done | sort
}

await_model() {
  local expected actual
  for _ in $(seq 60); do
    expected=$(expected_all)
    actual=$(actual_all)
    [[ $expected == "$actual" ]] && return 0
    sleep 0.05
  done
  echo "seed $SEED, after: ${history[*]}"
  echo "expected: $(tr '\n' ',' <<< "$expected")"
  echo "actual:   $(tr '\n' ',' <<< "$actual")"
  "$ANDROMEDA" windows --json | jq -c '.[] | {title, visible, fullscreen, x, y, w, h}'
  exit 1
}

fullscreen_count() { "$ANDROMEDA" windows --json | jq '[.[] | select(.fullscreen)] | length'; }
focused_fullscreen() { "$ANDROMEDA" windows --json | jq -r '[.[] | select(.active and .fullscreen)] | length'; }

echo "seed $SEED"
await_model
for _ in $(seq "$OPS"); do
  op=$((RANDOM % 9))
  case $op in
    0)
      n=$((RANDOM % 2 + 1))
      step="workspace-switch $n"
      "$ANDROMEDA" msg "workspace-switch:$n" > /dev/null 2>&1 || step+=" (rejected)"
      ;;
    1)
      # One fullscreen window at most, so the brute-force rule knows which one is on top.
      n=$((RANDOM % 2 + 1))
      if (($(fullscreen_count) == 0)); then
        step="window-move-to-workspace $n"
        "$ANDROMEDA" msg "window-move-to-workspace:$n" > /dev/null 2>&1 || step+=" (rejected)"
      else
        step="skip"
      fi
      ;;
    2) step="window-minimize"; "$ANDROMEDA" msg window-minimize > /dev/null 2>&1 || step+=" (rejected)" ;;
    3) step="window-restore-minimized"; "$ANDROMEDA" msg window-restore-minimized > /dev/null 2>&1 || step+=" (rejected)" ;;
    4)
      if (($(fullscreen_count) == 0 || $(focused_fullscreen) == 1)); then
        step="window-toggle-fullscreen"
        "$ANDROMEDA" msg window-toggle-fullscreen > /dev/null 2>&1 || step+=" (rejected)"
      else
        step="skip"
      fi
      ;;
    5)
      if (($(fullscreen_count) == 0)); then
        step="window-focus-next"
        "$ANDROMEDA" msg window-focus-next > /dev/null 2>&1 || step+=" (rejected)"
      else
        step="skip"
      fi
      ;;
    6)
      if ((locked == 0)); then
        step="lock"
        lock_count=$((lock_count + 1))
        fifo="$ANDROMEDA_RUNTIME_DIR/lock-$lock_count"
        mkfifo "$fifo"
        exec {lock_fd}<> "$fifo"
        "$LOCK_CLIENT" <&"$lock_fd" > "$ANDROMEDA_RUNTIME_DIR/lock-$lock_count.log" 2>&1 &
        await_events "$ANDROMEDA_RUNTIME_DIR/lock-$lock_count.log" locked 1 "lock $lock_count"
        locked=1
      else
        step="unlock"
        echo unlock >&"$lock_fd"
        await_events "$ANDROMEDA_RUNTIME_DIR/lock-$lock_count.log" unlocked 1 "unlock $lock_count"
        locked=0
      fi
      ;;
    7) step="dpms-off"; "$ANDROMEDA" msg dpms-off > /dev/null 2>&1 || step+=" (rejected)"; dpms_off=1 ;;
    8) step="dpms-on"; "$ANDROMEDA" msg dpms-on > /dev/null 2>&1 || step+=" (rejected)"; dpms_off=0 ;;
  esac
  history+=("$step")
  # settle waits for a frame, which a powered-off output never shows.
  if ((dpms_off == 0)); then
    "$ANDROMEDA" settle
  fi
  await_model
done

frames() { grep -c '^frame ' "$ANDROMEDA_RUNTIME_DIR/$1.log" || true; }
declare -A before
for title in "${TITLES[@]}"; do before[$title]=$(frames "$title"); done
sleep 1 # real time: frame callbacks follow the output's refresh
while read -r title presence; do
  gained=$(($(frames "$title") - before[$title]))
  if [[ $presence != 0 ]] && ((gained > 1)); then
    echo "seed $SEED: $title has presence $presence but got $gained frame callbacks in 1 s"
    exit 1
  fi
done < <(actual_all)

echo "seed $SEED: $OPS ops, presence matched the rule after each (${history[*]})"
