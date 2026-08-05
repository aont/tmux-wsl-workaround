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
while [ $# -gt 0 ]; do
    case $1 in
        --)
            shift
            break
            ;;
        -L|-S)
            opt=$1
            shift
            [ $# -gt 0 ] || passthrough "$opt"
            append_word global_words "$opt"
            append_word global_words "$1"
            append_word global_attach_words "$opt"
            append_word global_attach_words "$1"
            shift
            ;;
        -L?*|-S?*)
            opt=$(printf '%s' "$1" | cut -c 1-2)
            val=$(printf '%s' "$1" | cut -c 3-)
            append_word global_words "$opt"
            append_word global_words "$val"
            append_word global_attach_words "$opt"
            append_word global_attach_words "$val"
            shift
            ;;
        -C|-CC|-D|-h|-V|-c|-c?*|-*)
            passthrough "$@"
            ;;
        *)
            break
            ;;
    esac
done

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
parsing_options=1

while [ $# -gt 0 ]; do
    if [ $parsing_options -eq 0 ]; then
        append_word tail_words "$1"
        shift
        continue
    fi

    case $1 in
        --)
            parsing_options=0
            shift
            while [ $# -gt 0 ]; do
                append_word tail_words "$1"
                shift
            done
            ;;
        -)
            parsing_options=0
            append_word tail_words "$1"
            shift
            ;;
        -*)
            optarg=$1
            shift
            cluster=$(printf '%s' "$optarg" | cut -c 2-)
            while [ -n "$cluster" ]; do
                ch=$(printf '%s' "$cluster" | cut -c 1)
                rest=$(printf '%s' "$cluster" | cut -c 2-)
                case $ch in
                    A)
                        # -A can attach instead of creating, so let tmux handle it.
                        passthrough "$subcmd" "$optarg" "$@"
                        ;;
                    d)
                        user_detached=1
                        append_word new_words "-$ch"
                        cluster=$rest
                        ;;
                    D|E|P|X)
                        [ "$ch" = E ] && no_update_env=1
                        [ "$ch" = P ] && user_print=1
                        append_word new_words "-$ch"
                        cluster=$rest
                        ;;
                    c|e|F|f|n|s|t|x|y)
                        append_word new_words "-$ch"
                        if [ -n "$rest" ]; then
                            val=$rest
                            cluster=
                        else
                            [ $# -gt 0 ] || passthrough "$subcmd" "$optarg"
                            val=$1
                            shift
                            cluster=
                        fi
                        append_word new_words "$val"
                        [ "$ch" = F ] && user_format_specified=1 && user_format=$val
                        [ "$ch" = f ] && client_flags=$val
                        ;;
                    *)
                        passthrough "$subcmd" "$optarg" "$@"
                        ;;
                esac
            done
            ;;
        *)
            parsing_options=0
            append_word tail_words "$1"
            shift
            ;;
    esac
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

tmp=${TMPDIR:-/tmp}/tmux-wsl-workaround.$$
trap 'rm -f "$tmp"' EXIT HUP INT TERM

cmd_words=
append_word cmd_words "$CMD_EXE"
append_word cmd_words /c
append_word cmd_words start
append_word cmd_words ""
append_word cmd_words /wait
append_word cmd_words /min
append_word cmd_words wsl.exe
if [ -n "$WSL_DISTRO_NAME" ]; then
    append_word cmd_words -d
    append_word cmd_words "$WSL_DISTRO_NAME"
fi
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
append_word cmd_words "$internal_format"
cmd_words=${cmd_words}${tail_words}

cd /mnt/c || exit 1
eval "$cmd_words" >"$tmp"
status=$?
if [ $status -ne 0 ]; then
    cat "$tmp"
    exit $status
fi

exec 3<"$tmp"
IFS= read -r -d "$SEP" session_id <&3 || {
    printf '%s\n' 'tmux wrapper: session ID was not returned' >&2
    exit 1
}
case $session_id in
    \$[0-9]*) ;;
    *)
        printf 'tmux wrapper: invalid session ID: %s\n' "$session_id" >&2
        exit 1
        ;;
esac

if [ $user_print -eq 1 ]; then
    cat <&3
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

eval "exec $attach_words"
