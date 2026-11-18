#!/usr/bin/env bash
# Spaces draws the current workspace as a frame below the thumbs: its wallpaper mirror under the workspace fill
# (colors.overview.workspace_background), so the windows' area stands out from the dimmed backdrop around it.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
readonly IMAGE="$ANDROMEDA_RUNTIME_DIR/spaces-frame.png"
cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[colors]
backdrop = "#8C8C8CFF"

[overview]
style = "spaces"
EOF
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
grim -o HEADLESS-1 "$IMAGE"
# On 1280x720 the frame spans about x 212-1067, y 207-688 (thumbs 79 px tall from y 52, 76 px gap, 32 px bottom margin).
read -r ir ig ib < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" mean 40x40+240+230)
read -r or og ob < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" mean 40x40+80+400)
diff=$(((ir + ig + ib) - (or + og + ob)))
if ((diff > -30 && diff < 30)); then
  echo "the Spaces frame is not drawn: inside $ir,$ig,$ib, backdrop $or,$og,$ob"
  exit 1
fi
echo "the Spaces frame stands out from the backdrop: inside $ir,$ig,$ib, backdrop $or,$og,$ob"
