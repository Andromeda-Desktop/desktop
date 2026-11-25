#!/usr/bin/env bash
# Usage: sudo tools/greeter-switch.sh greetd|sddm|current
# greetd makes greetd with the shell's greeter mode (andromeda-greeter-git) the display manager from the next
# boot, keeping SDDM installed; sddm switches back. current removes a drop-in left by the retired legacy mode. None stops
# the running session: the change takes effect at the next greeter start.
set -euo pipefail
[[ $EUID == 0 ]] || { echo "run with sudo" >&2; exit 1; }
case ${1:-} in
  greetd)
    [[ -e /usr/share/noctalia-greeter/greetd.toml ]] || { echo "install andromeda-greeter-git first" >&2; exit 1; }
    [[ -e /etc/greetd/config.toml.pre-andromeda ]] || cp /etc/greetd/config.toml /etc/greetd/config.toml.pre-andromeda
    install -m644 /usr/share/noctalia-greeter/greetd.toml /etc/greetd/config.toml
    systemctl disable sddm
    systemctl enable greetd
    echo "greetd is the display manager from the next boot; 'sudo $0 sddm' undoes it"
    ;;
  sddm)
    systemctl disable greetd
    systemctl enable sddm
    [[ -e /etc/greetd/config.toml.pre-andromeda ]] && mv /etc/greetd/config.toml.pre-andromeda /etc/greetd/config.toml
    echo "SDDM is the display manager from the next boot"
    ;;
  current)
    rm -f /etc/systemd/system/greetd.service.d/zz-andromeda-legacy.conf
    systemctl daemon-reload
    echo "greetd runs the shell's greeter mode from its next start"
    ;;
  *) echo "usage: sudo $0 greetd|sddm|current" >&2; exit 2 ;;
esac
