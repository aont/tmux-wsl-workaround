#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
original_tmux=$root/tests/tmux-stub
tmp=$(mktemp -d)
trap 'rm -rf "$tmp" tests/launcher-stub tests/tmux-stub' EXIT HUP INT TERM
mkdir -p "$tmp/bin" "$tmp/empty"
: "${WRAPPER:=$root/wrapper}"

cat >tests/tmux-stub <<'EOF'
#!/bin/sh
printf 'TMUX:<%s>\n' "$@" >>"$LOG"
case " $* " in
*' show-options -g exit-empty '*)
    status=${PROBE_STATUS:-1}
    [ "$status" -eq 0 ] || echo 'no server running on test socket' >&2
    exit "$status"
    ;;
esac
exit 0
EOF
cat >tests/launcher-stub <<'EOF'
#!/bin/sh
printf 'LAUNCHER-BEGIN\n' >>"$LOG"
printf 'LAUNCHER-ARG:<%s>\n' "$@" >>"$LOG"
printf 'LAUNCHER-END\n' >>"$LOG"
exit "${BOOTSTRAP_STATUS:-0}"
EOF
chmod +x tests/tmux-stub tests/launcher-stub
export PATH="$tmp/bin:$PATH" LOG="$tmp/log" WSL_DISTRO_NAME='Test Distro'

assert_launcher()
{
    {
        echo LAUNCHER-BEGIN
        printf 'LAUNCHER-ARG:<%s>\n' "$@"
        echo LAUNCHER-END
    } >"$tmp/expected"
    grep '^LAUNCHER-' "$LOG" >"$tmp/actual"
    diff -u "$tmp/expected" "$tmp/actual"
}

assert_probe()
{
    printf 'TMUX:<%s>\n' "$@" >"$tmp/expected"
    sed '/^LAUNCHER-BEGIN$/,$d' "$LOG" >"$tmp/actual"
    diff -u "$tmp/expected" "$tmp/actual"
}

# Help and version are direct operations.
: >"$LOG"; PROBE_STATUS=1 "$WRAPPER" -V
grep -qx -- 'TMUX:<-V>' "$LOG"
! grep -q '^LAUNCHER-' "$LOG"
: >"$LOG"; PROBE_STATUS=1 "$WRAPPER" -h
grep -qx -- 'TMUX:<-h>' "$LOG"
! grep -q '^LAUNCHER-' "$LOG"

# An existing server is selected with the same socket options and needs no
# Windows bootstrap.  Options after the command remain command arguments.
: >"$LOG"; PROBE_STATUS=0 "$WRAPPER" -L foo new-session -d
grep -qx -- 'TMUX:<-L>' "$LOG"
grep -qx -- 'TMUX:<foo>' "$LOG"
grep -qx -- 'TMUX:<-d>' "$LOG"
! grep -q '^LAUNCHER-' "$LOG"

# Ordinary bootstrap: the launcher receives wsl.exe and each argument separately.
: >"$LOG"; env -u TMUX_TMPDIR PWD=/work PROBE_STATUS=1 "$WRAPPER" new-session 2>"$tmp/stderr"
[ ! -s "$tmp/stderr" ]
set -- wsl.exe -d 'Test Distro' --cd /work --exec "$original_tmux" \
    start-server ';' set-option -g exit-empty off
assert_launcher "$@"

# Spaces survive in the distro, working directory, and config path.
: >"$LOG"; env -u TMUX_TMPDIR WSL_DISTRO_NAME='Distro With Spaces' \
    PWD='/work dir/with spaces' PROBE_STATUS=1 "$WRAPPER" -f '/cfg dir/tmux conf' new-session
set -- wsl.exe -d 'Distro With Spaces' --cd '/work dir/with spaces' \
    --exec "$original_tmux" -f '/cfg dir/tmux conf' start-server ';' set-option -g exit-empty off
assert_launcher "$@"

# The probe and bootstrap retain duplicate -f/-L/-S options in input order.
: >"$LOG"; env -u TMUX_TMPDIR PWD=/work PROBE_STATUS=1 "$WRAPPER" \
    -f one -L 'first label' -S '/tmp/first sock' -f two -L second -S /tmp/final new-session
set -- wsl.exe -d 'Test Distro' --cd /work --exec "$original_tmux" \
    -f one -L 'first label' -S '/tmp/first sock' -f two -L second -S /tmp/final \
    start-server ';' set-option -g exit-empty off
assert_launcher "$@"
assert_probe -L 'first label' -S '/tmp/first sock' -L second -S /tmp/final \
    show-options -g exit-empty

# Defined, empty, and undefined TMUX_TMPDIR are distinct.
: >"$LOG"; TMUX_TMPDIR='/tmp/a b' PWD=/work PROBE_STATUS=1 "$WRAPPER" new-session
set -- wsl.exe -d 'Test Distro' --cd /work --exec env 'TMUX_TMPDIR=/tmp/a b' \
    "$original_tmux" start-server ';' set-option -g exit-empty off
assert_launcher "$@"
: >"$LOG"; TMUX_TMPDIR='' PWD=/work PROBE_STATUS=1 "$WRAPPER" new-session
set -- wsl.exe -d 'Test Distro' --cd /work --exec env 'TMUX_TMPDIR=' \
    "$original_tmux" start-server ';' set-option -g exit-empty off
assert_launcher "$@"

# Spaces, shell metacharacters, quotes, and trailing backslashes are unchanged
# at the argv boundary; no shell escaping is involved.
special='a&b|c^d%e!f"g\\'
: >"$LOG"; TMUX_TMPDIR="$special" WSL_DISTRO_NAME="$special" PWD="/$special" \
    PROBE_STATUS=1 "$WRAPPER" -f "$special" -L "$special" -S "$special" new-session
set -- wsl.exe -d "$special" --cd "/$special" --exec env "TMUX_TMPDIR=$special" \
    "$original_tmux" -f "$special" -L "$special" -S "$special" \
    start-server ';' set-option -g exit-empty off
assert_launcher "$@"

# Expected probe stderr is quiet, but inability to execute tmux is diagnosed.
mv tests/tmux-stub "$tmp/tmux.saved"
if PATH="$tmp/empty" "$WRAPPER" new-session 2>"$tmp/stderr"; then
    echo 'missing tmux unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'could not execute tmux' "$tmp/stderr"
mv "$tmp/tmux.saved" tests/tmux-stub

: >"$LOG"
if BOOTSTRAP_STATUS=9 PROBE_STATUS=1 "$WRAPPER" new-session 2>"$tmp/stderr"; then
    echo 'bootstrap failure unexpectedly succeeded' >&2
    exit 1
fi
grep -q '^LAUNCHER-BEGIN' "$LOG"
grep -q 'launcher exited with status 9' "$tmp/stderr"
! grep -qx 'TMUX:<new-session>' "$LOG"

echo 'all tests passed'
