# Andromeda Terminal shell integration for fish: OSC 133 command marks with the command line, OSC 7 cwd.
# The terminal puts this directory first in XDG_DATA_DIRS; this file removes it again for child processes.
if set -q ANDROMEDA_FISH_DATA_DIR
    set -gx XDG_DATA_DIRS (string replace -- "$ANDROMEDA_FISH_DATA_DIR:" "" "$XDG_DATA_DIRS")
    set -e ANDROMEDA_FISH_DATA_DIR
end

status is-interactive; or exit

function __andromeda_urlencode
    string escape --style=url -- $argv[1] | string replace -a %2F /
end

function __andromeda_preexec --on-event fish_preexec
    printf '\e]133;C;cmdline_url=%s\a' (__andromeda_urlencode "$argv[1]")
end

function __andromeda_postexec --on-event fish_postexec
    printf '\e]133;D;%s\a' $status
end

function __andromeda_prompt --on-event fish_prompt
    printf '\e]7;file://%s%s\a\e]133;A\a' (prompt_hostname) (__andromeda_urlencode "$PWD")
end

# Only the stock prompt becomes the blank row and `λ  `; a user prompt stays.
# Stock means fish's own prompt (its data dir, or embedded in the binary); a distro or user prompt stays.
set -l __andromeda_source (functions --details fish_prompt)
if string match -q -- "$__fish_data_dir/functions/fish_prompt.fish" $__andromeda_source; or string match -q -- 'embedded:*' $__andromeda_source
    function fish_prompt
        printf '\nλ  \e]133;B\a'
    end
end
