# tmux WSL wrapper

This wrapper prevents the first tmux server in a WSL distribution from becoming
a child of the current Windows terminal. If the selected server does not exist,
it uses `newwin_launcher.exe` to start `wsl.exe`, disables tmux's `exit-empty`
option, and then replaces itself with the requested tmux command.

`newwin_launcher.exe` creates the child in a separate, minimized Windows console
with no inherited standard handles. It waits for the child and returns the
child's exit status. The wrapper invokes it directly, with `wsl.exe` and every
argument passed as a separate argv element:

```text
newwin_launcher.exe wsl.exe -d <distro> --cd <pwd> --exec tmux ... \
  start-server ; set-option -g exit-empty off
```

## Build the Windows launcher

Build the launcher on Windows before installing the wrapper. With Microsoft
Visual C++ in a Developer Command Prompt, run:

```bat
nmake /f Makefile.msvc
```

Alternatively, from an **MSYS2 UCRT64** shell with MinGW-w64 installed, run:

```sh
make -f Makefile.mingw
```

Both commands produce `newwin_launcher.exe` in the project directory. The
executable is a build artifact and is not stored in the repository.

## Build and test the Linux wrapper

From Linux or WSL, run:

```sh
make -f Makefile.linux
make -f Makefile.linux test
```

At build time, the Makefile resolves `tmux` from `PATH` and embeds its absolute
path. Override it with, for example,
`make -f Makefile.linux TMUX_PATH=/opt/tmux/bin/tmux`. The server probe,
bootstrap command, and final execution use that path, so an installed wrapper
can safely be named independently of the real tmux executable.

The server probe uses `show-options -g exit-empty`, rather than `has-session`,
because it succeeds for a live server with zero sessions. `-L` and `-S` are
copied to the probe. Bootstrap preserves every `-f`, `-L`, and `-S`, including
duplicates and their original order. Global parsing stops at the first command,
so `new-session -d` is not treated as a wrapper option. `-h` and `-V` go
directly to tmux without probing or bootstrapping.

If `TMUX_TMPDIR` exists (including when its value is empty), the local probe
inherits it and bootstrap explicitly invokes `env TMUX_TMPDIR=value tmux ...`.
If it is absent, bootstrap omits both `env` and the assignment. Values containing
spaces or shell metacharacters remain single argv elements; no command shell or
shell-specific escaping is involved.

## Install

After copying or building `newwin_launcher.exe` in the project directory, run:

```sh
make -f Makefile.linux install
```

The default layout is:

```text
/usr/local/bin/tmux-wsl-wrapper
/usr/local/libexec/newwin_launcher.exe
```

`PREFIX` defaults to `/usr/local`; `BINDIR` and `LIBEXECDIR` default to
`$(PREFIX)/bin` and `$(PREFIX)/libexec`. The final, non-staged launcher path is
compiled into the wrapper, so the default build uses
`/usr/local/libexec/newwin_launcher.exe` and does not rely on `PATH` or runtime
path discovery. Change the prefix consistently at build and install time, for
example:

```sh
make -f Makefile.linux PREFIX=/opt/tmux-wsl clean all
make -f Makefile.linux PREFIX=/opt/tmux-wsl install
```

Packagers can stage files with `DESTDIR`:

```sh
make -f Makefile.linux PREFIX=/usr DESTDIR="$pkgdir" install
```

This installs beneath `$pkgdir/usr`, but the path compiled into the wrapper is
still `/usr/libexec/newwin_launcher.exe`; `DESTDIR` is never part of a runtime
path. The Linux install target only copies an already-built launcher and does
not invoke a Windows compiler.

## Verification

The Linux test suite substitutes recording POSIX scripts for tmux and the
launcher. It checks exact bootstrap argv, direct `-h`/`-V` handling, the option
boundary at `new-session -d`, option ordering and socket selection,
defined/empty/undefined `TMUX_TMPDIR`, special characters, probe behavior, and
bootstrap failure propagation. Its compile-time launcher-path override means it
does not require Windows, Wine, MSVC, MinGW, or a Windows executable.

On WSL, useful smoke tests after building and installing both components are:

```sh
tmux-wsl-wrapper -V
tmux-wsl-wrapper -h
tmux-wsl-wrapper new-session
tmux-wsl-wrapper -L foo new-session
tmux-wsl-wrapper -S /tmp/custom.sock new-session
tmux-wsl-wrapper -f ~/.tmux.conf new-session
TMUX_TMPDIR=/tmp/tmux-test tmux-wsl-wrapper new-session
env -u TMUX_TMPDIR tmux-wsl-wrapper new-session
```
