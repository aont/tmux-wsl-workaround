#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp" tests/cmd-stub' EXIT HUP INT TERM
mkdir -p "$tmp/bin"
: "${WRAPPER:=$root/wrapper}"

cat >"$tmp/bin/tmux" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >>"$LOG"
case " $* " in *' show-options -g exit-empty '*) exit "${PROBE_STATUS:-1}";; esac
exit 0
EOF
cat >tests/cmd-stub <<'EOF'
#!/bin/sh
printf 'CMD:%s\n' "$*" >>"$LOG"
exit "${BOOTSTRAP_STATUS:-0}"
EOF
chmod +x "$tmp/bin/tmux" tests/cmd-stub
export PATH="$tmp/bin:$PATH" LOG="$tmp/log" WSL_DISTRO_NAME='Test Distro'

: >"$LOG"; PROBE_STATUS=1 "$WRAPPER" -V
grep -qx -- '-V' "$LOG"
! grep -q '^CMD:' "$LOG"

: >"$LOG"; PROBE_STATUS=1 "$WRAPPER" -h
grep -qx -- '-h' "$LOG"
! grep -q '^CMD:' "$LOG"

: >"$LOG"; PROBE_STATUS=0 "$WRAPPER" -L foo new-session -d
grep -q 'show-options' "$LOG"
tail -4 "$LOG" | grep -qx -- '-d'
! grep -q '^CMD:' "$LOG"

: >"$LOG"; PROBE_STATUS=1 "$WRAPPER" -f 'a b' -L foo -f bar new-session
grep -q '^CMD:.*"-f" "a b" "-L" "foo" "-f" "bar" "start-server"' "$LOG"

: >"$LOG"; TMUX_TMPDIR='/tmp/a b&c' PROBE_STATUS=1 "$WRAPPER" -S '/tmp/custom.sock' new-session
grep -q '^CMD:.*"env" "TMUX_TMPDIR=/tmp/a b&c" "tmux" "-S" "/tmp/custom.sock"' "$LOG"

: >"$LOG"; env -u TMUX_TMPDIR PROBE_STATUS=1 "$WRAPPER" new-session
grep -q '^CMD:' "$LOG"
! grep -q 'TMUX_TMPDIR=' "$LOG"

: >"$LOG"
if BOOTSTRAP_STATUS=9 PROBE_STATUS=1 "$WRAPPER" new-session; then
    echo 'bootstrap failure unexpectedly succeeded' >&2
    exit 1
fi
grep -q '^CMD:' "$LOG"
! grep -qx 'new-session' "$LOG"

echo 'all tests passed'
