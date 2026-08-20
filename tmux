#!/bin/ash
# tmux WSL workaround wrapper.  This file is intentionally written for ash,
# not bash, so do not use arrays or bash-only syntax here.

TMUX_BIN=${TMUX_BIN:-/usr/bin/tmux}
CONSOLE_REDIRECT_EXE=${CONSOLE_REDIRECT_EXE:-/usr/local/bin/console_redirect.exe}
WSL_DISTRO_NAME=${WSL_DISTRO_NAME:-}

quote_arg() {
    printf "'%s'" "$(printf "%s" "$1" | sed "s/'/'\\\\''/g")"
}

append_word() {
    eval "$1=\${$1}\$(quote_arg \"\$2\")' '"
}

set_terminal_title() {
    printf '\033]0;tmux\007'
}

passthrough() {
    set_terminal_title
    eval "exec $(quote_arg "$TMUX_BIN") $original_words"
}

original_words=
for arg do
    append_word original_words "$arg"
done

# Command sequences can make an inner, minimized tmux attach and block.
for arg do
    if [ "$arg" = ';' ]; then
        passthrough "$@"
    fi
done

start_dir=$PWD
global_words=
global_attach_words=

# Parse only global options that are safe for special handling.  Anything else
# is left to the real tmux unchanged.
OPTIND=1
while getopts :L:S:CDhVc: opt; do
    case $opt in
        L|S)
            opt="-$opt"
            append_word global_words "$opt"
            append_word global_words "$OPTARG"
            append_word global_attach_words "$opt"
            append_word global_attach_words "$OPTARG"
            ;;
        C|D|h|V|c|?|:)
            passthrough "$@"
            ;;
    esac
done
shift $((OPTIND - 1))

if [ $# -eq 0 ]; then
    subcmd=new-session
else
    subcmd=$1
    case $subcmd in
        new-session|new)
            shift
            ;;
        *)
            passthrough "$subcmd" "$@"
            ;;
    esac
fi

user_detached=0
user_print=0
user_format_specified=0
user_format=
client_flags=
no_update_env=0
new_words=
tail_words=

OPTIND=1
while getopts :AdEPXc:e:F:f:n:s:t:x:y: opt; do
    opt_word="-$opt"
    case $opt in
        A)
            # -A can attach instead of creating, so let tmux handle it.
            passthrough "$subcmd" "$@"
            ;;
        d)
            user_detached=1
            append_word new_words "$opt_word"
            ;;
        D|E|P|X)
            [ "$opt" = E ] && no_update_env=1
            [ "$opt" = P ] && user_print=1
            append_word new_words "$opt_word"
            ;;
        F)
            user_format_specified=1
            user_format=$OPTARG
            ;;
        c|e|f|n|s|t|x|y)
            append_word new_words "$opt_word"
            append_word new_words "$OPTARG"
            [ "$opt" = f ] && client_flags=$OPTARG
            ;;
        ?|:)
            passthrough "$subcmd" "$@"
            ;;
    esac
done
shift $((OPTIND - 1))

while [ $# -gt 0 ]; do
    append_word tail_words "$1"
    shift
done

if [ -z "$WSL_DISTRO_NAME" ]; then
    printf '%s\n' 'tmux wrapper: WSL_DISTRO_NAME is required' >&2
    exit 1
fi

cmd_words=
append_word cmd_words "$CONSOLE_REDIRECT_EXE"
append_word cmd_words wsl.exe
append_word cmd_words -d
append_word cmd_words "$WSL_DISTRO_NAME"
append_word cmd_words --cd
append_word cmd_words "$start_dir"
append_word cmd_words "--exec"
append_word cmd_words "$TMUX_BIN"
cmd_words=${cmd_words}${global_words}
append_word cmd_words new-session
cmd_words=${cmd_words}${new_words}
append_word cmd_words -d
append_word cmd_words -P
append_word cmd_words -F
append_word cmd_words '#{session_id}'
cmd_words=${cmd_words}${tail_words}

session_id=$(eval "$cmd_words")
launch_status=$?
if [ $launch_status -ne 0 ]; then
    printf '%s\n' "$session_id" >&2
    exit $launch_status
fi

if [ -z "$session_id" ]; then
    printf '%s\n' 'tmux wrapper: session ID was not returned' >&2
    exit 1
fi
case $session_id in
    \$[0-9]*) ;;
    *)
        printf 'tmux wrapper: invalid session ID: %s\n' "$session_id" >&2
        exit 1
        ;;
esac

if [ $user_print -eq 1 ]; then
    if [ $user_format_specified -eq 1 ]; then
        visible_format=$user_format
    else
        visible_format='#{session_name}:'
    fi
    display_words=
    append_word display_words "$TMUX_BIN"
    display_words=${display_words}${global_words}
    append_word display_words display-message
    append_word display_words -p
    append_word display_words -t
    append_word display_words "$session_id"
    append_word display_words -F
    append_word display_words "$visible_format"
    eval "$display_words" || exit $?
fi

[ $user_detached -eq 1 ] && exit 0

attach_words=
append_word attach_words "$TMUX_BIN"
attach_words=${attach_words}${global_attach_words}
append_word attach_words attach-session
if [ $no_update_env -eq 1 ]; then
    append_word attach_words -E
fi
if [ -n "$client_flags" ]; then
    append_word attach_words -f
    append_word attach_words "$client_flags"
fi
append_word attach_words -t
append_word attach_words "$session_id"

set_terminal_title
eval "exec $attach_words"
