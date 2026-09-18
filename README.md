# tmux WSL wrapper

This wrapper prevents the first tmux server in a WSL distribution from becoming
a child of the current Windows terminal.  If the selected server does not exist,
it starts it through `cmd.exe` and `wsl.exe`, disables `exit-empty`, and then
replaces itself with the requested tmux command.

## Build

```sh
make
```

Put the resulting `wrapper` somewhere in `PATH` (under a name other than
`tmux`) and invoke it with ordinary tmux arguments.

The server probe uses `show-options -g exit-empty`, rather than `has-session`,
because it succeeds for a live server with zero sessions. `-L` and `-S` are
copied to that probe. Bootstrap preserves every `-f`, `-L`, and `-S`, including
duplicates and their original order. Global parsing stops at the first command,
so `new-session -d` is not treated as a wrapper option. `-h` and `-V` go directly
to tmux without probing or bootstrapping.

If `TMUX_TMPDIR` exists (including when its value is empty), the local probe
inherits it and bootstrap explicitly invokes `env TMUX_TMPDIR=value tmux ...`.
If it is absent, bootstrap omits both `env` and the assignment. User-controlled
arguments are individually quoted for `cmd.exe`; delayed expansion is disabled.

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
the direct `-h`/`-V` paths, option boundary at `new-session -d`, carry-over
ordering, socket selection, and defined-versus-undefined `TMUX_TMPDIR` behavior.
