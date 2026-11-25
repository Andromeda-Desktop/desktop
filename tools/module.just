# Build-dir recipes shared by the module justfiles (import '../tools/module.just'); each module keeps its configure flags.
mode := "debug"
prefix := "/usr/local"
cpp-std := "c++23"

# Sets up or reconfigures build-<m> with the module's meson args. wipe=wipe regenerates a dir that rejects them.
_meson-setup m wipe *args:
    #!/usr/bin/env bash
    set -euo pipefail
    dir="build-$1"
    shift 2
    if [[ ! -d "$dir" ]]; then
        ../tools/build.sh meson setup "$dir" "$@"
    elif [[ "{{wipe}}" == wipe ]]; then
        # A dir rejects an option added since it was configured until Meson re-reads the option file;
        # subproject options are accepted only by a fresh setup.
        ../tools/build.sh meson setup "$dir" "$@" --reconfigure \
          || { ../tools/build.sh meson setup "$dir" --reconfigure && ../tools/build.sh meson setup "$dir" "$@" --reconfigure; } \
          || ../tools/build.sh meson setup "$dir" "$@" --wipe
    else
        ../tools/build.sh meson setup "$dir" "$@" --reconfigure
    fi
    ln -sfn "$dir/compile_commands.json" compile_commands.json

[no-exit-message]
_ensure-configured m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-{{m}}/build.ninja" ]]; then
        just configure {{m}}
    fi

clean m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -L compile_commands.json && "$(readlink compile_commands.json)" == "build-{{m}}/compile_commands.json" ]]; then
        rm -f compile_commands.json
    fi
    rm -rf build-{{m}}

rebuild m=mode: (clean m) (build m)
