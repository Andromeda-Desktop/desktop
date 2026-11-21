#!/usr/bin/env bash
# Prints the install notes at the top of an app release; GitHub appends the generated change list below them.
# Usage: tools/release-notes.sh APP VERSION     (APP: files, player, viewer, terminal, link-app)
set -euo pipefail
app=$1 version=$2
releases=https://github.com/${GITHUB_REPOSITORY:-Andromeda-Desktop/desktop}/releases
cat <<EOF
## Install

**Linux.** Add the Andromeda repository once with the installer in the [README](https://github.com/${GITHUB_REPOSITORY:-Andromeda-Desktop/desktop}#install), then install or update the app:

- Arch: \`sudo pacman -Syu andromeda-$app-git\`
- Ubuntu, Debian, Kali: \`sudo apt update && sudo apt install andromeda-$app\`
EOF
if [[ $app == terminal ]]; then
  cat <<EOF

**macOS** (Apple silicon, macOS 26 or later). Install or update from the Terminal app:

\`\`\`
curl -fsSL https://raw.githubusercontent.com/${GITHUB_REPOSITORY:-Andromeda-Desktop/desktop}/main/tools/macos/install-terminal.sh | sh
\`\`\`

Or download [Andromeda-Terminal-$version-macos-arm64.zip]($releases/download/terminal-v$version/Andromeda-Terminal-$version-macos-arm64.zip) and drag **Andromeda Terminal** to Applications.
The app is not notarized, so macOS blocks a downloaded copy on first launch: open **System Settings > Privacy & Security** and click **Open Anyway**.
EOF
fi
