default: check

[doc('Quiet build of one module (compositor shell portal files player viewer system link): one ok line, or the first 3 errors as file:line. Log in /tmp/andromeda-build. Reuses the last result when nothing changed; --force reruns. No module: every module, full output.')]
build module="" force="":
    #!/usr/bin/env bash
    set -euo pipefail
    [ -z {{quote(module)}} ] || exec tools/quiet.sh build {{quote(module)}} {{quote(force)}}
    (cd compositor && ../tools/build.sh just build)
    (cd shell && ../tools/build.sh just build)
    (cd portal && ../tools/build.sh just build)
    [ -e link/.git ] || git submodule update --init link
    (cd link && cargo build)
    (cd system && cargo build)

# Link's gate: format, pedantic clippy, the proto tests, and the supply-chain policy.
link:
    cd link && cargo fmt --check
    cd link && cargo clippy --all-targets -- -D warnings
    cd link && cargo test
    cd link && cargo deny check

# andromeda-system's gate: format, pedantic clippy, unit tests, and the daemon against the power mock on a test bus.
system:
    cd system && cargo fmt --check
    cd system && cargo clippy --all-targets -- -D warnings
    cd system && cargo test
    cd system && cargo build
    system/tests/mock_bus.sh

[doc('Quiet unit tests of one module, built first: failing tests with their first file:line error, then "N passed, M failed". Log in /tmp/andromeda-test. Reuses the last result when nothing changed; --force reruns. No module: build all, then compositor and shell suites.')]
test module="" filter="" force="":
    #!/usr/bin/env bash
    set -euo pipefail
    [ -z {{quote(module)}} ] || exec tools/quiet.sh test {{quote(module)}} {{quote(filter)}} {{quote(force)}}
    just build
    (cd compositor && ../tools/build.sh just test)
    (cd shell && ../tools/build.sh just test)

# Wait for a log line, pid, file or bus name in one blocking call (tools/wait.sh --help); never sleep or poll.
[positional-arguments]
wait *args:
    @tools/wait.sh "$@"

# Find where a feature lives: module, entry files, test and build module (docs/map.toml).
where word:
    @tools/where.sh {{quote(word)}}

# Build, test and scenario of one feature from docs/map.toml: one ok line, or the first failure with its log.
verify +feature:
    @tools/verify.sh {{quote(feature)}}

# Check one file after an edit: one-TU syntax compile (C/C++), cargo check -p (Rust), compile (Python), bash -n (shell).
check-file path:
    @tools/check-file.py {{quote(path)}}

# One scenario against a headless compositor (tests/scenario/*.toml): one ok line, or the expected/actual diff.
scenario file:
    @tools/scenario.py {{quote(file)}}

# Phone state in one call: screenshot, UI dump, logcat tail and top activity in /tmp/phone-state/<name>. Honors ANDROID_SERIAL.
phone-state name="now":
    #!/usr/bin/env bash
    set -uo pipefail
    adb() { "${ADB:-$HOME/Android/Sdk/platform-tools/adb}" ${ANDROID_SERIAL:+-s "$ANDROID_SERIAL"} "$@"; }
    adb get-state > /dev/null 2>&1 || { echo "FAIL: no phone attached (check adb devices, ANDROID_SERIAL)"; exit 1; }
    d=/tmp/phone-state/{{name}}; mkdir -p "$d"
    exec 9> /tmp/link-emulator.lock; flock 9
    adb exec-out screencap -p > "$d/screenshot.png"
    adb shell uiautomator dump /sdcard/ui.xml > /dev/null 2>&1; adb exec-out cat /sdcard/ui.xml > "$d/ui.xml"
    pid=$(adb shell pidof org.andromeda.link | tr -d '\r')
    adb logcat -d -t 2000 ${pid:+--pid="$pid"} | tail -n 200 > "$d/logcat.txt"
    adb shell dumpsys activity activities | grep -m1 -E 'topResumedActivity|mResumedActivity' | sed 's/^ *//' > "$d/activity.txt"
    printf '%s\n' "$d/screenshot.png" "$d/ui.xml" "$d/logcat.txt"; echo "top: $(cat "$d/activity.txt")"
    grep -E ' E |FATAL' "$d/logcat.txt" | tail -n 5 | cut -c1-200

# Every E2E script; a failure does not stop the run. Logs and failed.txt go to /tmp/andromeda-e2e.
e2e: build
    tools/build.sh meson compile -C compositor/build-debug harness-clients
    cd compositor && just asan
    rm -f /tmp/andromeda-e2e/failed.txt
    just e2e-run tests/e2e/*.sh

# Only the scripts that failed in the last run, without a rebuild.
e2e-failed:
    #!/usr/bin/env bash
    f=/tmp/andromeda-e2e/failed.txt
    [ -s "$f" ] || { echo "no failed scripts"; exit 0; }
    just e2e-run $(sed 's|.*|tests/e2e/&.sh|' "$f")

# One E2E script by name, without a rebuild: `just e2e-one link_app`.
e2e-one name:
    just e2e-run tests/e2e/{{name}}.sh

# Every E2E script on GitHub (workflow e2e.yml, pushed main), not on this machine: `just e2e-ci` or `just e2e-ci dock_basic`.
e2e-ci *names:
    gh workflow run e2e.yml --ref main -f names="{{names}}"

# The whole release on GitHub (workflow release.yml, pushed main): libdesk, its apps and the core, published only after every build and the index gate pass. `just release false` is a dry run.
release publish="true":
    gh workflow run release.yml --ref main -f publish={{publish}}

# Unit suites and headless E2E in an Ubuntu container, on the working tree: `just e2e-ubuntu` or `just e2e-ubuntu 26.04 dock_basic`.
e2e-ubuntu version="26.04" *names:
    tools/ubuntu-e2e.sh {{version}} {{names}}

[private]
e2e-run +scripts:
    #!/usr/bin/env bash
    # failed.txt keeps the failures of the last full run; a pass removes its name, a failure adds it.
    d=/tmp/andromeda-e2e; mkdir -p "$d"; touch "$d/failed.txt"; bad=0
    for t in {{scripts}}; do
      n=$(basename "$t" .sh); if grep -qx "$n" tests/e2e/helpers.txt; then echo "skip $n: a helper, not a test"; continue; fi
      sed -i "/^$n\$/d" "$d/failed.txt"
      # Exit 77: the machine lacks a tool or device the test needs; its last line says which.
      bash "$t" > "$d/$n.log" 2>&1; rc=$?
      case $rc in
        0) echo "pass $n" ;;
        77) echo "skip $n: $(tail -n 1 "$d/$n.log")" ;;
        *) echo "FAIL $n: $d/$n.log"; echo "$n" >> "$d/failed.txt"; bad=1 ;;
      esac
    done
    if [ -s "$d/failed.txt" ]; then echo "still failing: $(tr '\n' ' ' < "$d/failed.txt")"; fi
    exit $bad

# Module boundaries: fails on a dependency that docs/modules.toml does not allow.
modules:
    @tools/module-check.py

# Core owners: fails when code bypasses an owner (.agent/TASK-core.md); --update tightens tools/core-check-allow.txt.
core-check *flags:
    @tools/core-check.py {{flags}}

# Cross-builds and links the portable libdesk-app and libdesk-ui for arm64 macOS: fails when Linux-only code leaves the platform layer.
macos-check:
    #!/usr/bin/env bash
    set -euo pipefail
    cross=$HOME/.cache/andromeda-mac/cross.ini
    [ -f "$cross" ] || tools/macos/deps.py
    sdk=$(grep -m1 -o "'-isysroot', '[^']*" "$cross" | cut -d"'" -f4)
    [ -d "$sdk" ] || { echo "macos-check: no Darwin SDK at $sdk; run 'xtool sdk' (needs Xcode.xip)" >&2; exit 1; }
    [ -f libdesk-app/build-macos/build.ninja ] || tools/build.sh meson setup libdesk-app/build-macos libdesk-app --cross-file "$cross" -Dplatform=macos
    tools/build.sh ninja -C libdesk-app/build-macos
    tools/macos/link-check.sh libdesk-app/build-macos
    tools/build-libghostty.sh macos
    [ -f apps/terminal/build-macos/build.ninja ] || tools/build.sh meson setup apps/terminal/build-macos apps/terminal --cross-file "$cross" -Dplatform=macos --buildtype=release
    tools/build.sh ninja -C apps/terminal/build-macos
    tools/macos/link-check.sh apps/terminal/build-macos andromeda-terminal libdesk-term/libghostty-vt/prefix-macos/lib/libghostty-vt.dylib
    for crate in ptyd agentd; do cargo check -q --locked --target aarch64-apple-darwin --tests --manifest-path apps/terminal/$crate/Cargo.toml --target-dir /var/tmp/andromeda-macos-cargo; done

# Cross-builds "Andromeda Terminal.app" for arm64 macOS into /var/tmp/andromeda-macos (OUT overrides).
macos-terminal:
    tools/macos/terminal.sh

check: modules core-check test link system
    cd compositor && just check
    just e2e

# Incremental: pkg/src and its build dirs stay; the PKGBUILD wipes them when a meson file changes. `just package -C` builds clean.
package *flags:
    # flock: runs share pkg/src, so a second session's run waits.
    # One makepkg per release unit, as CI: each package takes its own unit's version, so a local build sorts above that
    # unit's last release and below its next one (one run would give every app the core version).
    cd pkg && for unit in core libdesk; do \
      if [ "$unit" = core ]; then sel=ANDROMEDA_CORE=1; else sel=ANDROMEDA_APP=$unit; fi; \
      env "$sel" ANDROMEDA_SOURCE="file://$(dirname "$(pwd -P)")#branch=$(git branch --show-current)" flock /tmp/andromeda-package.lock systemd-run --user --scope --quiet --collect -p CPUWeight=idle -p IOWeight=1 taskset -c 0-11 nice -n 19 makepkg -f {{flags}} || exit; \
    done
    # makepkg writes the resolved pkgver back into the PKGBUILD.
    git checkout pkg/PKGBUILD

# The design reference: tokens.css from protocol/design-tokens.toml, then docs/design/reference.png (1600 px wide).
# Rebuild docs/design/rulebook.html from rulebook.md, deviations.md and rulebook-specimen.html.
design-rulebook:
    python3 docs/design/build_rulebook.py

# Check a mockup or the specimen against the rulebook (R1, R5, R6, R7, component sizes).
design-check file='docs/design/rulebook.html':
    python3 docs/design/check_layout.py {{file}}

# Write the Link Android tokens (object Tok, committed in the link repo) from protocol/design-tokens.toml.
tokens-android:
    python3 protocol/gen-design-tokens.py protocol/design-tokens.toml link/apps/android/app/src/main/kotlin/org/andromeda/link/ui/theme/Tok.kt

design-reference:
    python3 protocol/gen-design-tokens.py protocol/design-tokens.toml /tmp/design_tokens.h docs/design/tokens.css
    google-chrome-stable --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 --window-size=1600,1010 --screenshot="$PWD/docs/design/reference.png" "file://$PWD/docs/design/reference.html"

# Live shell loop: the user session runs shell/build-f7/andromeda-shell; each new build-f7 binary (mold link) restarts the shell and captures the screen to /tmp/andromeda-live/last.png.
dev-live:
    tools/build.sh meson configure shell/build-f7 -Dcpp_link_args=-fuse-ld=mold -Dc_link_args=-fuse-ld=mold -Dbackend_max_links=1
    tools/f7.sh andromeda-shell noctalia-calc.so
    mkdir -p ~/.config/systemd/user/andromeda-shell.service.d /tmp/andromeda-live
    printf '[Service]\nEnvironment=ANDROMEDA_DEV_TOKENS=%s/protocol/design-tokens.toml\nExecStart=\nExecStart=%s/shell/build-f7/andromeda-shell\n' "$PWD" "$PWD" > ~/.config/systemd/user/andromeda-shell.service.d/dev-live.conf
    # The compositor grants the shell globals only to installed binaries: trust the repo from the next login (dev-comp does it too).
    mkdir -p ~/.config/systemd/user/andromeda.service.d
    [ -f ~/.config/systemd/user/andromeda.service.d/dev-live.conf ] || printf '[Service]\nExecStart=\nExecStart=/usr/bin/andromeda --trust-binary %s/\n' "$PWD" > ~/.config/systemd/user/andromeda.service.d/dev-live.conf
    systemctl --user daemon-reload && systemctl --user restart andromeda-shell.service
    # Restart only on a new binary: a source watch restarts the live shell on every half-done agent edit.
    watchexec -p -w shell/build-f7/andromeda-shell --debounce 500ms -- 'systemctl --user restart andromeda-shell.service && sleep 1 && grim /tmp/andromeda-live/last.png'

# Back to the packaged shell.
dev-live-off:
    rm -f ~/.config/systemd/user/andromeda-shell.service.d/dev-live.conf ~/.config/systemd/user/noctalia.service.d/dev-live.conf
    ! grep -qs 'ExecStart=/usr/bin/andromeda ' ~/.config/systemd/user/andromeda.service.d/dev-live.conf || rm ~/.config/systemd/user/andromeda.service.d/dev-live.conf
    systemctl --user daemon-reload && systemctl --user restart andromeda-shell.service

# The session compositor runs compositor/build-live/andromeda from the next login. A compositor cannot restart under a live session: rebuild, then log out and in.
dev-comp:
    [ -d compositor/build-live ] || tools/build.sh meson setup compositor/build-live compositor --buildtype=debugoptimized --prefix=/usr -Dcpp_link_args=-fuse-ld=mold -Dc_link_args=-fuse-ld=mold
    tools/build.sh ninja -C compositor/build-live andromeda
    # A relink drops the file cap; without it the GPU context is not high priority.
    sudo setcap cap_sys_nice=ep compositor/build-live/andromeda || echo 'run: sudo setcap cap_sys_nice=ep compositor/build-live/andromeda (GPU high priority)'
    mkdir -p ~/.config/systemd/user/andromeda.service.d
    printf '[Service]\nExecStart=\nExecStart=%s/compositor/build-live/andromeda --trust-binary %s/\n' "$PWD" "$PWD" > ~/.config/systemd/user/andromeda.service.d/dev-live.conf
    systemctl --user daemon-reload

# Back to the packaged compositor from the next login. From a TTY after a dev compositor crash: run this, then log in again.
dev-comp-off:
    rm -f ~/.config/systemd/user/andromeda.service.d/dev-live.conf
    systemctl --user daemon-reload

# A test desktop in a window: dev compositor + dev shell with copies of your config. Rerun after a compositor rebuild.
dev-nested:
    tests/dev-nested.sh

# Rulebook lint: no float size literal, no duration easing in shell and libdesk-ui; only new violations fail. `just design-lint --update` rewrites the baseline.
design-lint *args:
    python3 docs/design/design_lint.py {{args}}

# Terminal load suites, headless, logs in ~/.cache/term-load: `just term-load throughput` (also latency, chaos, lifecycle, soak, all).
# Env: TERMS="ours foot kitty ghostty", BIN, HOLDER=1, ASAN=1 with BIN=apps/terminal/build-asan/andromeda-terminal, MINUTES.
term-load suite="all":
    #!/usr/bin/env bash
    set -uo pipefail
    suites="{{suite}}"; [[ $suites == all ]] && suites="throughput latency chaos lifecycle soak"
    rc=0; mkdir -p "${XDG_CACHE_HOME:-$HOME/.cache}/term-load"
    for s in $suites; do echo "== $s"; apps/terminal/tests/load/$s.sh 2>&1 | tee ${XDG_CACHE_HOME:-$HOME/.cache}/term-load/$s.log | grep -E '^(PASS|FAIL|FAILS|INFO|info|skip)|^[a-z]+ +[a-z_]+ +[0-9]'; [[ ${PIPESTATUS[0]} -eq 0 ]] || rc=1; done
    exit $rc
