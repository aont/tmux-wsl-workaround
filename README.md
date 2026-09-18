# tmux WSL wrapper

This wrapper prevents the first tmux server in a WSL distribution from becoming
a child of the current Windows terminal. If the selected server does not exist,
it first checks tmux's default `exit-empty` value with a disposable server named
`check`, then starts the requested server through `cmd.exe` and `wsl.exe` with
`exit-empty` disabled.

When the detected default is `on`, the wrapper forks the requested tmux command
and, in a separate child, restores `exit-empty` to `on` on the new server. The
waiting parent ignores terminal signals while each child retains tmux's normal
signal handling. This keeps the server alive across the bootstrap gap without
changing its eventual option from the tmux default. If the detected default is
`off`, the wrapper retains the original direct-exec behavior.

## Build

```sh
make
```

At build time, `make` resolves `tmux` from `PATH` and embeds that absolute path
in the wrapper.  The server probe, Windows-side bootstrap, and final execution
all use the embedded path, so the resulting binary can safely be installed as
`tmux` without recursively invoking itself.  To select a particular binary,
set the path explicitly (for example, `make TMUX_PATH=/opt/tmux/bin/tmux`).

Put the resulting `wrapper` somewhere in `PATH` (under a name other than
`tmux`, or as `tmux` itself) and invoke it with ordinary tmux arguments.

The server probe uses `show-options -g exit-empty`, rather than `has-session`,
because it succeeds for a live server with zero sessions. `-L` and `-S` are
copied to that probe. Bootstrap preserves every `-f`, `-L`, and `-S`, including
duplicates and their original order. Global parsing stops at the first command,
so `new-session -d` is not treated as a wrapper option. `-h` and `-V` go directly
to tmux without probing or bootstrapping.

If `TMUX_TMPDIR` exists (including when its value is empty), the local probe
inherits it and bootstrap explicitly invokes `env TMUX_TMPDIR=value tmux ...`.
If it is absent, bootstrap omits both `env` and the assignment.

Bootstrap calls `cmd.exe` with a real argument vector:

```text
cmd.exe /d /v:off /c start "" /wait /min wsl.exe -d ... --exec tmux ...
```

It deliberately does not construct a quoted `/c` command string. WSL interop
performs the one required conversion from that vector to a Windows command
line, after which `cmd.exe`'s `start` command consumes the remaining words.
This avoids nesting CRT-style backslash quoting inside a string which
`cmd.exe` parses with different rules. Delayed expansion remains disabled so
literal exclamation marks are not expanded.

## Verification

Run the automated argv-level tests with `make test`. On WSL, useful smoke tests
are:

```sh
./wrapper -V
./wrapper -h
./wrapper new-session
./wrapper -L foo new-session
./wrapper -S /tmp/custom.sock new-session
./wrapper -f ~/.tmux.conf new-session
./wrapper -f ~/.tmux.conf -L foo new-session
./wrapper -L foo new-session -d
TMUX_TMPDIR=/tmp/tmux-test ./wrapper new-session
env -u TMUX_TMPDIR ./wrapper new-session
```

The test suite replaces `tmux` and `cmd.exe` with recording stubs, and checks
the exact argv boundary, direct `-h`/`-V` paths, option boundary at
`new-session -d`, carry-over ordering, socket selection, defined/empty/undefined
`TMUX_TMPDIR`, spaces, quotes, trailing backslashes, cmd metacharacters, the
disposable default check, and restoration of an `on` default.

The stub cannot emulate WSL's PE interop or `cmd.exe` parsing. Before a release,
run this additional integration check from an actual WSL shell (with no tmux
server already using the test socket):

```sh
test -x /mnt/c/Windows/System32/cmd.exe
make clean all
name='wsl smoke &|^%!"\\'
dir="/tmp/$name"
mkdir -p "$dir"
printf 'set -g @wsl_wrapper_smoke yes\n' >"$dir/config file.conf"
TMUX_TMPDIR="$dir" ./wrapper -f "$dir/config file.conf" \
  -L 'label with spaces' start-server
tmux -L 'label with spaces' show-options -gv @wsl_wrapper_smoke
tmux -L 'label with spaces' kill-server
```

The final `show-options` must print `yes`. This exercises the actual WSL
`execve` to Win32 conversion, `cmd.exe /c`, `start`, and `wsl.exe` parsing chain
which a POSIX stub cannot reproduce.
