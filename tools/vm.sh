#!/usr/bin/env bash
# Usage: tools/vm.sh [--distro arch|ubuntu|debian] reset|start [--headless|--gl-headless]|install|ssh [CMD...]|screenshot FILE|
#   suspend|wake
# --gl-headless: a virgl GPU with no window, so a session runs (screenshots over ssh with grim).
# A clean VM for the out-of-box checks (docs/gaps.md Tier A, docs/ubuntu-port.md 4). The official cloud image is the
# read-only base; `reset` throws away every change by making a new overlay on it.
# arch: the Arch "basic" image (user and password `arch`). ubuntu: the Ubuntu 26.04 cloud image, given user and
# password `ubuntu` by a cloud-init seed disk. debian: the Debian sid daily cloud image, user and password `debian`;
# `install` installs the published release through install.sh, as a user does. ACPI S3 is on: `suspend` sleeps the guest
# through logind and `wake` resumes it from the QEMU monitor.
set -euo pipefail
DISTRO=arch
if [[ ${1:-} == --distro ]]; then
  DISTRO=${2:?--distro needs arch or ubuntu}
  shift 2
fi
case $DISTRO in
  arch)
    URL=https://geo.mirror.pkgbuild.com/images/latest/Arch-Linux-x86_64-basic.qcow2
    SUMS=$URL.SHA256
    VMUSER=arch
    DEFAULT_DIR=$HOME/.cache/andromeda-vm
    ;;
  ubuntu)
    UBUNTU_VERSION=${ANDROMEDA_VM_UBUNTU:-26.04}
    URL=https://cloud-images.ubuntu.com/releases/$UBUNTU_VERSION/release/ubuntu-$UBUNTU_VERSION-server-cloudimg-amd64.img
    SUMS=$(dirname "$URL")/SHA256SUMS
    VMUSER=ubuntu
    DEFAULT_DIR=$HOME/.cache/andromeda-vm-ubuntu-$UBUNTU_VERSION
    ;;
  debian)
    URL=https://cloud.debian.org/images/cloud/sid/daily/latest/debian-sid-genericcloud-amd64-daily.qcow2
    SUMS=$(dirname "$URL")/SHA512SUMS
    VMUSER=debian
    DEFAULT_DIR=$HOME/.cache/andromeda-vm-debian-sid
    ;;
  *) echo "unknown distro: $DISTRO" >&2; exit 2 ;;
esac
DIR=${ANDROMEDA_VM_DIR:-$DEFAULT_DIR}
PORT=${ANDROMEDA_VM_SSH_PORT:-2222}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=error)
# The fixed password, given by askpass so no prompt stops a script.
export SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=$DIR/askpass
SSH=(ssh -p "$PORT" "${OPTS[@]}" "$VMUSER@localhost")

base() {
  mkdir -p "$DIR"
  printf '#!/bin/sh\necho %s\n' "$VMUSER" > "$DIR/askpass" && chmod +x "$DIR/askpass"
  [[ -e $DIR/base.qcow2 ]] && return
  curl -fL --progress-bar -o "$DIR/base.qcow2.part" "$URL"
  local sum
  sum=$(curl -fsSL "$SUMS" | awk -v f="$(basename "$URL")" '$2 == f || $2 == "*" f || NF == 1 {print $1; exit}')
  local check=sha256sum
  [[ $SUMS == *SHA512SUMS ]] && check=sha512sum
  (cd "$DIR" && echo "$sum  base.qcow2.part" | "$check" -c --quiet)
  mv "$DIR/base.qcow2.part" "$DIR/base.qcow2"
}

# NoCloud seed: a FAT disk labelled CIDATA. The cloud image has no password login until cloud-init sets one.
seed() {
  local tmp
  tmp=$(mktemp -d)
  cat > "$tmp/user-data" << EOF
#cloud-config
password: $VMUSER
chpasswd: { expire: false }
ssh_pwauth: true
growpart: { mode: auto, devices: ["/"] }
EOF
  echo "instance-id: andromeda-$(date +%s)" > "$tmp/meta-data"
  rm -f "$DIR/seed.img"
  mkfs.vfat -C -n CIDATA "$DIR/seed.img" 2048 > /dev/null
  mcopy -i "$DIR/seed.img" "$tmp/user-data" "$tmp/meta-data" ::
  rm -rf "$tmp"
}

case ${1:-} in
  reset)
    base
    rm -f "$DIR/run.qcow2"
    qemu-img create -q -f qcow2 -b base.qcow2 -F qcow2 "$DIR/run.qcow2"
    if [[ $DISTRO != arch ]]; then
      # The cloud image root is ~3 GB; the session and its dependencies need more.
      qemu-img resize -q "$DIR/run.qcow2" 24G
      seed
    fi
    echo "clean VM at $DIR/run.qcow2"
    ;;
  start)
    [[ -e $DIR/run.qcow2 ]] || "$0" --distro "$DISTRO" reset
    display=(-device virtio-vga-gl -display gtk,gl=on)
    [[ ${2:-} == --headless ]] && display=(-display none -vga none -serial file:"$DIR/serial.log")
    # QEMU's screendump has no surface for a GL scanout: `screenshot` reads the frame over VNC instead.
    # The GPU sits behind a PCIe root port with no soft reset, so it keeps its resources over S3 (QEMU resets it
    # otherwise and every scanout after resume fails with an invalid resource id).
    [[ ${2:-} == --gl-headless ]] && display=(-device pcie-root-port,id=gpu-port,chassis=1
      -device virtio-vga-gl,bus=gpu-port,x-pcie-pm-no-soft-reset=on -display egl-headless -vnc unix:"$DIR/vnc.sock"
      -serial file:"$DIR/serial.log")
    rm -f "$DIR/vnc.sock"
    extra=()
    [[ $DISTRO != arch ]] && extra=(-drive file="$DIR/seed.img",if=virtio,format=raw,readonly=on)
    # S3 on (q35 turns it off by default): logind then offers suspend, as on real hardware.
    exec qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4G -machine q35 -global ICH9-LPC.disable_s3=0 \
      -drive file="$DIR/run.qcow2",if=virtio "${extra[@]}" \
      -nic user,model=virtio-net-pci,hostfwd=tcp::"$PORT"-:22 \
      -device virtio-tablet-pci -monitor unix:"$DIR/monitor.sock",server,nowait "${display[@]}"
    ;;
  install)
    if [[ $DISTRO == debian ]]; then
      scp -P "$PORT" "${OPTS[@]}" "$ROOT/install.sh" "$VMUSER@localhost:/tmp/install.sh"
      # install.sh reads apt's questions from /dev/tty: give it one.
      ssh -tt -p "$PORT" "${OPTS[@]}" "$VMUSER@localhost" "yes | sudo DEBIAN_FRONTEND=noninteractive bash /tmp/install.sh"
      exit
    fi
    if [[ $DISTRO == ubuntu ]]; then
      # The debs from tools/ubuntu-deb.sh and the extra packages our apt repo carries (tools/deb-xwayland-satellite.sh).
      CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/andromeda-ubuntu
      mapfile -t pkgs < <(ls "$CACHE/deb-$UBUNTU_VERSION"/*.deb "$CACHE/xwayland-satellite-$UBUNTU_VERSION"/*.deb)
      # The andromeda metapackage needs andromeda-link from the Link release; leave it out when that deb is absent.
      if ! printf '%s\n' "${pkgs[@]}" | grep -q '/andromeda-link_'; then
        mapfile -t pkgs < <(printf '%s\n' "${pkgs[@]}" | grep -v '/andromeda_')
      fi
      "${SSH[@]}" "rm -rf /tmp/debs && mkdir /tmp/debs"
      scp -P "$PORT" "${OPTS[@]}" "${pkgs[@]}" "$VMUSER@localhost:/tmp/debs/"
      "${SSH[@]}" "sudo apt-get update -qq && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y /tmp/debs/*.deb"
      exit
    fi
    # The newest build of each Andromeda package in pkg/, without the opt-in power policy and the old umbriel names.
    mapfile -t pkgs < <(ls -t "$ROOT"/pkg/*.pkg.tar.zst | awk -F/ '{n=$NF; sub(/-[^-]+-[^-]+-[^-]+\.pkg\.tar\.zst$/, "", n)}
      n !~ /^umbriel|power/ && !seen[n]++')
    scp -P "$PORT" "${OPTS[@]}" "${pkgs[@]}" arch@localhost:/tmp/
    "${SSH[@]}" "sudo pacman -Syu --noconfirm && sudo pacman -U --noconfirm /tmp/*.pkg.tar.zst"
    ;;
  ssh) shift; exec "${SSH[@]}" "$@" ;;
  suspend) "${SSH[@]}" "sudo systemd-run --quiet --on-active=1 systemctl suspend" ;;
  wake) python3 -c 'import socket, sys, time; s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.settimeout(5)
buf = b""
while b"(qemu)" not in buf: buf += s.recv(4096)
s.sendall(b"system_wakeup\n"); time.sleep(1)' "$DIR/monitor.sock" ;;
  # screenshot FILE.png: the VM display from outside (works for the greeter, which refuses screencopy).
  screenshot)
    if [[ -S $DIR/vnc.sock ]]; then
      python3 "$ROOT/tools/vnc-shot.py" "$DIR/vnc.sock" "$DIR/shot.ppm"
      magick "$DIR/shot.ppm" "${2:?screenshot needs a .png path}" && rm -f "$DIR/shot.ppm"
      exit
    fi
    # The monitor drops input sent before its "(qemu)" prompt.
    python3 -c 'import socket, sys, time; s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.settimeout(5)
buf = b""
while b"(qemu)" not in buf: buf += s.recv(4096)
s.sendall(("screendump " + sys.argv[2] + "\n").encode()); time.sleep(2)' "$DIR/monitor.sock" "$DIR/shot.ppm"
    for _ in $(seq 50); do [[ -s $DIR/shot.ppm ]] && break; sleep 0.1; done
    magick "$DIR/shot.ppm" "${2:?screenshot needs a .png path}" && rm -f "$DIR/shot.ppm"
    ;;
  *) sed -n 2p "$0" >&2; exit 2 ;;
esac
