#!/usr/bin/env bash
# Usage: sudo tools/andromeda-switch.sh
# Installs the packages `just package` built last, then makes the desktop's own login the display manager from the next
# boot: swaps in the patched Gaze (tools/gaze-patched.sh), opens the password prompt
# beside the face check for sudo and polkit, and switches SDDM to greetd (tools/greeter-switch.sh). Reboot afterwards.
# Undo: sudo tools/greeter-switch.sh sddm; sudo tools/gaze-patched.sh restore.
set -euo pipefail
[[ $EUID == 0 ]] || { echo "run with sudo" >&2; exit 1; }
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# The newest build of each package: each release unit has its own version (`just package` runs one makepkg per unit).
names=$(cd "$ROOT/pkg" && env -u ANDROMEDA_APP -u ANDROMEDA_CORE -u ANDROMEDA_ONLY bash -c 'source PKGBUILD > /dev/null; echo "${pkgname[@]}"')
mapfile -t packages < <(ls -t "$ROOT"/pkg/*.pkg.tar.zst 2> /dev/null | awk -F/ -v names="$names" '
  BEGIN { n = split(names, list, " "); for (i = 1; i <= n; i++) ours[list[i]] }
  { name = $NF; sub(/-[0-9]+:[^-]+-[^-]+-[^-]+\.pkg\.tar\.zst$/, "", name); if (name in ours && !(name in seen)) { seen[name]; print } }')
# The power policy changes kernel defaults and stays opt-in: only an installed one is upgraded.
pacman -Q andromeda-power-git > /dev/null 2>&1 || pacman -Q umbriel-power-git > /dev/null 2>&1 || mapfile -t packages < <(printf '%s\n' "${packages[@]}" | grep -v /andromeda-power-git-)
[[ ${#packages[@]} -gt 0 ]] || { echo "no packages in pkg/; run just package" >&2; exit 1; }
# -U ignores `replaces`; --ask 4 answers yes to removing the conflicting umbriel-* packages.
pacman -U --needed --noconfirm --ask 4 "${packages[@]}"

"$ROOT/tools/gaze-patched.sh" install
# Face scans only on request: an empty answer to the one prompt starts the camera.
sed -i -E 's/^(auth\s+sufficient\s+pam_gaze\.so)(\s+(simultaneous|sequential|retry))?$/\1 on-request/' /etc/pam.d/polkit-1 /etc/pam.d/sudo
"$ROOT/tools/greeter-switch.sh" greetd
echo "done: reboot to log in through the Ryoku greeter"
