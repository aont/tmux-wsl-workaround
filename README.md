# tmux-wsl-workaround

A narrow `tmux` wrapper for Windows Subsystem for Linux.

When a new tmux session is requested, the wrapper starts the real tmux through a separate Windows-side `wsl.exe` launch hosted by the included ConPTY helper. It obtains the newly created session ID through private FIFOs and then attaches the original terminal to that session.

Most other tmux commands are passed directly to the real tmux.

## When to use this

Use this wrapper when you need new tmux servers to be created through a separate Windows-side WSL launch rather than directly by the current WSL client.

This is not a replacement implementation of the tmux command-line interface. It deliberately applies special handling only to a small set of session-creation forms and delegates everything else to tmux.

## Requirements

* Windows Subsystem for Linux with Windows executable interoperability enabled
* A normal WSL distribution session with `WSL_DISTRO_NAME` set
* The Windows C drive mounted at `/mnt/c`
* tmux installed in the same WSL distribution
* The wrapper running as the distribution's default WSL user
* A C99 compiler, a MinGW-w64 cross-compiler, and `make` when building the compiled implementation
* Windows 10 version 1809 or later (required by ConPTY)

The wrapper and the real tmux must be in the **same WSL distribution**. The FIFO paths used to return the session ID and exit status are local to that distribution.

## Implementations

The repository contains two implementations.

### Compiled C implementation

`tmux.c` is built and installed by the Makefile. This is the recommended implementation. The build also installs a small `tmux-wsl-launch` helper that redirects the inner command's output and reports its exit status without launching a shell, plus the Windows `conpty.exe` host built from the included `conpty.c` source. The ConPTY source comes from [vast-cow's helper](https://gist.github.com/vast-cow/3ea08fdfc2caba6ac2df1939dd316268), with fallback declarations added for older MinGW-w64 headers.

The real tmux, ConPTY host, and inner launch-helper paths are fixed at build time.

### BusyBox ash implementation

The `tmux` file is an implementation written for BusyBox `ash`. It uses the compiled `tmux-wsl-launch` helper for the inner WSL process.

The Makefile does not install the ash implementation automatically, but it does install the launch and ConPTY helpers that the script requires.

## Installation

First install the real tmux and the tools required to build the wrapper.

Clone the repository and run:

```sh
make
sudo make install
```

By default, this produces the following arrangement:

```text
/usr/local/bin/tmux   wrapper
/usr/bin/tmux         real tmux
/usr/local/libexec/tmux-wsl-launch   inner launch helper
/usr/local/libexec/conpty.exe        Windows ConPTY host
```

Ensure `/usr/local/bin` precedes `/usr/bin` in `PATH`:

```sh
type -a tmux
```

The wrapper should be listed first, followed by the real tmux.

### Custom paths

The following Make variables are available:

| Variable   | Default                           | Purpose                            |
| ---------- | --------------------------------- | ---------------------------------- |
| `TMUX_BIN` | `/usr/bin/tmux`                   | Absolute path to the real tmux     |
| `CONPTY_EXE` | `$(LIBEXECDIR)/conpty.exe`      | Windows ConPTY host path           |
| `LAUNCH_BIN` | `$(LIBEXECDIR)/tmux-wsl-launch` | Inner WSL launch helper path      |
| `WINDOWS_CC` | `x86_64-w64-mingw32-gcc`       | Compiler used to build `conpty.exe` |
| `PREFIX`   | `/usr/local`                      | Installation prefix                |
| `BINDIR`   | `$(PREFIX)/bin`                   | Wrapper installation directory     |
| `LIBEXECDIR` | `$(PREFIX)/libexec`             | Launch helper installation directory |

For example:

```sh
make clean
make TMUX_BIN=/usr/bin/tmux \
     CONPTY_EXE=/usr/local/libexec/conpty.exe
sudo make install PREFIX=/usr/local
```

Do not install the wrapper over the real tmux binary. `TMUX_BIN` must never resolve to the wrapper itself, or invocation will recurse.

### Installing the ash implementation

To use the shell implementation instead:

```sh
sudo install -m 0755 ./tmux /usr/local/bin/tmux
```

It recognizes these optional environment variables:

```sh
TMUX_BIN=/usr/bin/tmux
CONPTY_EXE=/usr/local/libexec/conpty.exe
TMUX_WSL_LAUNCH_BIN=/usr/local/libexec/tmux-wsl-launch
TMUX_WSL_LAUNCH_TIMEOUT=15
```

The timeout value for the ash implementation is in seconds.

## Optional dedicated Alpine WSL distribution

Using a small Alpine Linux WSL distribution exclusively as a tmux host is a convenient arrangement.

This keeps the wrapper, the tmux server, and its configuration isolated from larger development distributions. It also avoids replacing or modifying the `tmux` command in those distributions.

Install or import Alpine using an appropriate WSL installation method. From PowerShell, inspect the distributions available to your WSL installation with:

```powershell
wsl.exe --list --online
```

After installation, find the exact registered distribution name with:

```powershell
wsl.exe --list --verbose
```

Inside Alpine, the compiled implementation can be installed with:

```sh
apk add --no-cache git tmux build-base mingw-w64-gcc

git clone https://github.com/aont/tmux-wsl-workaround.git
cd tmux-wsl-workaround

make
make install
```

Run the installation command as root, or through an appropriately configured privilege-elevation tool.

For the ash implementation, build and install the launch helper before replacing the compiled wrapper with the script:

```sh
apk add --no-cache git tmux build-base mingw-w64-gcc

git clone https://github.com/aont/tmux-wsl-workaround.git
cd tmux-wsl-workaround

make
make install
install -m 0755 ./tmux /usr/local/bin/tmux
```

When Alpine is used as a dedicated host, tmux panes initially run Alpine commands and shells. To enter another installed WSL distribution from a pane, invoke it explicitly, for example:

```sh
wsl.exe -d Ubuntu
```

The wrapper itself must still run in Alpine so that its client, server, and FIFO transport all use the same distribution.

## Usage

Normal session creation requires no new syntax:

```sh
tmux
```

Create and attach to a named session:

```sh
tmux new-session -s work
```

The official `new` alias is also recognized:

```sh
tmux new -s work
```

Create a detached session:

```sh
tmux new-session -d -s background
```

Other tmux commands are normally passed through unchanged:

```sh
tmux list-sessions
tmux attach-session -t work
tmux kill-session -t work
```

Socket selection is preserved for specially handled creation and the subsequent attachment:

```sh
tmux -L private new-session -s work
```

## Command behavior

| Invocation                                          | Behavior                                               |
| --------------------------------------------------- | ------------------------------------------------------ |
| `tmux`                                              | Create a new session through the workaround and attach |
| `tmux new-session ...`                              | Apply the workaround                                   |
| `tmux new ...`                                      | Apply the workaround                                   |
| `tmux new-session -d ...`                           | Create through the workaround without attaching        |
| `tmux new-session -P ...`                           | Preserve tmux's printable session output               |
| `tmux -L name new-session ...`                      | Use the same named socket for creation and attachment  |
| `tmux -S path new-session ...`                      | Use the same socket path for creation and attachment   |
| Any other tmux command                              | Pass through to the real tmux                          |
| `new-session -A ...`                                | Pass through to the real tmux                          |
| A tmux command sequence containing a standalone `;` | Pass through to the real tmux                          |
| An abbreviated command such as `new-s`              | Pass through to the real tmux                          |
| Unsupported, malformed, or unknown options          | Pass through so tmux handles validation and errors     |

No arguments are deliberately interpreted as `new-session`, even if tmux's `default-client-command` option has been customized.

## How it works

For specially handled session creation, the wrapper:

1. Records the current WSL distribution and working directory.

2. Creates a private temporary directory containing result and status FIFOs.

3. Starts a Windows command through the included ConPTY host using approximately this launch chain:

   ```text
   conpty.exe -- wsl.exe -d <current-distribution>
       --cd <current-directory>
       --exec /usr/local/libexec/tmux-wsl-launch
       --output <result-fifo> --status <status-fifo> -- ...
   ```

4. Runs the real tmux with internal `-d`, `-P`, and `-F '#{session_id}'` options.

5. Receives the immutable tmux session ID and tmux exit status through the FIFOs.

6. When the user requested `-P`, obtains the printable result separately with
   `display-message -p -t <session-id> -F <user-format>`. This prevents the
   user's format from interfering with retrieval of the session ID.

7. Returns immediately if the original command included `-d`.

8. Otherwise, replaces the wrapper with:

   ```sh
   /usr/bin/tmux attach-session -t '<session-id>'
   ```

Attachment uses the immutable session ID rather than the potentially ambiguous session name.

## Limitations

* The special handling recognizes only `new-session`, its official `new` alias, and an omitted command.
* Abbreviated command names are delegated without the workaround.
* `new-session -A` is delegated because it can turn session creation into an attachment.
* tmux command sequences are delegated because a later attachment command could block inside the inner WSL process.
* The wrapper requires `WSL_DISTRO_NAME`.
* The inner WSL launch uses the distribution's configured default user. Running the wrapper as another user can prevent access to its private FIFOs.
* `/mnt/c` must exist because the wrapper uses it as the working directory while starting the Windows executable.
* The C implementation currently uses a fixed 30-second launch timeout.
* Because the inner session is initially created detached, its initial size follows tmux's detached-session rules. Supply `-x` and `-y` when an explicit initial size is required.
* The C and ash implementations have different timeout configuration and may not fail identically in every edge case.

## Troubleshooting

### `WSL_DISTRO_NAME must be set`

Run the wrapper from a normal WSL distribution session:

```sh
printf '%s\n' "$WSL_DISTRO_NAME"
```

A container, chroot, or manually constructed Linux environment may not provide this variable.

### The wrapper times out

Check that Windows interoperability and the expected paths work:

```sh
test -x /usr/local/libexec/conpty.exe
test -x /usr/bin/tmux
```

Check that WSL can relaunch the current distribution:

```sh
wsl.exe -d "$WSL_DISTRO_NAME" \
    --exec /bin/sh -c 'printf "WSL relaunch works\n"'
```

Also verify that:

* The registered distribution name matches `WSL_DISTRO_NAME`.
* The caller is the distribution's default WSL user.
* The current directory exists and is accessible to that user.
* `/tmp`, or the configured `TMPDIR`, supports FIFOs.
* `/mnt/c` is mounted.

### tmux invocation recurses

Inspect all commands named `tmux`:

```sh
type -a tmux
```

The wrapper and the real binary must be different files. With the default installation:

```text
/usr/local/bin/tmux
/usr/bin/tmux
```

Rebuild with the correct absolute real-tmux path if necessary:

```sh
make clean
make TMUX_BIN=/usr/bin/tmux
sudo make install
```

### A command does not use the workaround

The following are intentionally delegated:

* `new-session -A`
* Command sequences containing a standalone `;`
* Abbreviated commands such as `new-s`
* Unsupported global options
* All commands other than `new-session` and `new`

This lets the real tmux preserve its own validation and behavior.

## Uninstallation

Remove only the wrapper:

```sh
sudo rm /usr/local/bin/tmux
```

The real tmux at `/usr/bin/tmux` remains installed.

## License

[MIT](LICENSE)
