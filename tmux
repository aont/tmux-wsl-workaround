#!/bin/ash
# tmux WSL workaround wrapper.  This file is intentionally written for ash,
# not bash, so do not use arrays or bash-only syntax here.

TMUX_BIN=${TMUX_BIN:-/usr/bin/tmux}
CMD_EXE=${CMD_EXE:-/mnt/c/Windows/System32/cmd.exe}
WSL_DISTRO_NAME=${WSL_DISTRO_NAME:-}
SEP=$(printf '\037')

quote_arg() {
    printf "'%s'" "$(printf "%s" "$1" | sed "s/'/'\\\\''/g")"
}

append_word() {
    eval "$1=\${$1}\$(quote_arg \"\$2\")' '"
}

passthrough() {
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
        c|e|F|f|n|s|t|x|y)
            append_word new_words "$opt_word"
            append_word new_words "$OPTARG"
            [ "$opt" = F ] && user_format_specified=1 && user_format=$OPTARG
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

if [ $user_print -eq 1 ]; then
    if [ $user_format_specified -eq 1 ]; then
        visible_format=$user_format
    else
        visible_format='#{session_name}:'
    fi
else
    visible_format=
fi
internal_format="#{session_id}${SEP}${visible_format}"

if [ -z "$WSL_DISTRO_NAME" ]; then
    printf '%s\n' 'tmux wrapper: WSL_DISTRO_NAME is required for FIFO transport' >&2
    exit 1
fi

LAUNCH_TIMEOUT=${TMUX_WSL_LAUNCH_TIMEOUT:-15}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/tmux-wsl-workaround.XXXXXX") || exit 1
result_fifo=$tmpdir/result.fifo
status_fifo=$tmpdir/status.fifo
mkfifo "$result_fifo" "$status_fifo" || {
    rm -rf "$tmpdir"
    exit 1
}
watchdog_pid=
cleanup() {
    if [ -n "$watchdog_pid" ]; then
        kill "$watchdog_pid" 2>/dev/null
    fi
    rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

cmd_words=
append_word cmd_words "$CMD_EXE"
append_word cmd_words /c
append_word cmd_words start
append_word cmd_words ""
append_word cmd_words /min
append_word cmd_words wsl.exe
append_word cmd_words -d
append_word cmd_words "$WSL_DISTRO_NAME"
append_word cmd_words --cd
append_word cmd_words "$start_dir"
append_word cmd_words "--exec"
append_word cmd_words /bin/sh
append_word cmd_words -c
append_word cmd_words 'result_fifo=$1; status_fifo=$2; shift 2; "$@" >"$result_fifo"; status=$?; printf "%s\n" "$status" >"$status_fifo"; exit "$status"'
append_word cmd_words sh
append_word cmd_words "$result_fifo"
append_word cmd_words "$status_fifo"
append_word cmd_words "$TMUX_BIN"
cmd_words=${cmd_words}${global_words}
append_word cmd_words new-session
cmd_words=${cmd_words}${new_words}
append_word cmd_words -d
append_word cmd_words -P
append_word cmd_words -F
append_word cmd_words "$internal_format"
cmd_words=${cmd_words}${tail_words}

cd /mnt/c || exit 1
eval "$cmd_words"
launch_status=$?
if [ $launch_status -ne 0 ]; then
    exit $launch_status
fi

(
    sleep "$LAUNCH_TIMEOUT"
    printf '%s\n' 'tmux wrapper: timed out waiting for WSL launch' >"$result_fifo"
    printf '%s\n' 124 >"$status_fifo"
) &
watchdog_pid=$!

exec 3<"$result_fifo"
IFS= read -r -d "$SEP" session_id <&3
read_session_status=$?

result_status=
if read -r result_status <"$status_fifo"; then
    :
else
    result_status=1
fi

if [ -n "$watchdog_pid" ]; then
    kill "$watchdog_pid" 2>/dev/null
    watchdog_pid=
fi

case $result_status in
    ''|*[!0-9]*) result_status=1 ;;
esac

if [ "$result_status" -ne 0 ]; then
    if [ $read_session_status -ne 0 ]; then
        printf '%s\n' 'tmux wrapper: session ID was not returned' >&2
    fi
    cat <&3
    exit "$result_status"
fi

if [ $read_session_status -ne 0 ]; then
    printf '%s\n' 'tmux wrapper: session ID was not returned' >&2
    exit 1
fi
case $session_id in
    \$[0-9]*) ;;
    *)
        printf 'tmux wrapper: invalid session ID: %s\n' "$session_id" >&2
        cat <&3 >/dev/null
        exit 1
        ;;
esac

if [ $user_print -eq 1 ]; then
    cat <&3
else
    cat <&3 >/dev/null
fi
exec 3<&-

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

eval "exec $attach_words"
