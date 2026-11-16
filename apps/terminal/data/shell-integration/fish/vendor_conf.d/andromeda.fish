# Andromeda Terminal shell integration for fish: OSC 133 command marks with the command line, OSC 7 cwd, git branch (OSC 1337 SetUserVar).
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

# The git branch of $PWD (short hash when detached; nothing outside a repository).
function __andromeda_branch
    set -l dir $PWD
    while true
        set -l git
        if test -d "$dir/.git"
            set git "$dir/.git"
        else if test -f "$dir/.git"
            read -l line < "$dir/.git"; or return
            string match -q -- 'gitdir: *' $line; or return
            set git (string replace -- 'gitdir: ' '' $line)
            string match -q -- '/*' $git; or set git "$dir/$git"
        end
        if test -n "$git"
            read -l head < "$git/HEAD" 2> /dev/null; or return
            if string match -q -- 'ref: refs/heads/*' $head
                string replace -- 'ref: refs/heads/' '' $head
            else if test (string length -- $head) -ge 7
                string sub -l 7 -- $head
            end
            return
        end
        test -z "$dir"; and return
        set dir (string replace -r -- '/[^/]*$' '' $dir)
    end
end

function __andromeda_prompt --on-event fish_prompt
    printf '\e]7;file://%s%s\a\e]133;A\a' (prompt_hostname) (__andromeda_urlencode "$PWD")
    set -l branch (__andromeda_branch)
    test -n "$branch"; and set branch (printf %s $branch | base64 -w0)
    printf '\e]1337;SetUserVar=git_branch=%s\a' "$branch"
end

# Only the stock prompt becomes the blank row and three blank cells; a user prompt stays.
# Stock means fish's own prompt (its data dir, or embedded in the binary); a distro or user prompt stays.
set -l __andromeda_source (functions --details fish_prompt)
if string match -q -- "$__fish_data_dir/functions/fish_prompt.fish" $__andromeda_source; or string match -q -- 'embedded:*' $__andromeda_source
    function fish_prompt
        printf '\n   \e]133;B\a'
    end
end
