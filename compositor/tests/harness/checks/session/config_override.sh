#!/usr/bin/env bash
# A runtime override reaches the live layout, outlives a reload of a file that sets the same key, and unsetting it
# hands the key back to the file.
set -euo pipefail

snapshot() { "$ANDROMEDA" windows --json | jq -Sc '[.[] | {x, w, h}] | sort_by(.x)'; }

foot sh -c 'sleep 120' > /dev/null 2>&1 &
foot sh -c 'sleep 120' > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$ANDROMEDA" windows --json | jq 'length') -eq 2 ]] && break
  sleep 0.1
done
"$ANDROMEDA" settle
base=$(snapshot)

"$ANDROMEDA" config set layout.gap 40
"$ANDROMEDA" settle
gapped=$(snapshot)
if [[ $("$ANDROMEDA" config get layout.gap) != 40 || $gapped == "$base" ]]; then
  echo "config set did not reach the layout: get=$("$ANDROMEDA" config get layout.gap) $gapped"
  exit 1
fi
if ! tail -n 20 "$ANDROMEDA_LOG" | grep -q "config reloaded (sections: layout; effects: workspace layout)"; then
  echo "config set was not applied as a layout change:"
  tail -n 5 "$ANDROMEDA_LOG" | sed "s/^/    /"
  exit 1
fi

printf '\n[layout]\ngap = 5\n' >> "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle
if [[ $("$ANDROMEDA" config get layout.gap --json) != '{"override":true,"value":40}' || $(snapshot) != "$gapped" ]]; then
  echo "a file reload undid the override: $("$ANDROMEDA" config get layout.gap --json) $(snapshot)"
  exit 1
fi

if "$ANDROMEDA" config set layout.gap '"wide"' 2> "$ANDROMEDA_RUNTIME_DIR/refused"; then
  echo "an invalid override was accepted"
  exit 1
fi
if ! grep -q "layout.gap" "$ANDROMEDA_RUNTIME_DIR/refused" || [[ $("$ANDROMEDA" config get layout.gap) != 40 ]]; then
  echo "a refused override was not reported or changed the value: $(cat "$ANDROMEDA_RUNTIME_DIR/refused")"
  exit 1
fi

"$ANDROMEDA" config unset layout.gap
"$ANDROMEDA" settle
if [[ $("$ANDROMEDA" config get layout.gap --json) != '{"override":false,"value":5}' || $(snapshot) == "$gapped" ]]; then
  echo "config unset did not hand the key back to the file: $("$ANDROMEDA" config get layout.gap --json) $(snapshot)"
  exit 1
fi
if "$ANDROMEDA" config unset layout.gap 2> /dev/null; then
  echo "unsetting a key without an override succeeded"
  exit 1
fi

echo "override applied, survived a reload, refused a bad value, and unset back to the file"
