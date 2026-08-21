CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -std=c99 -Wall -Wextra -O2
LDFLAGS ?=
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/libexec
TMUX_BIN ?= /usr/bin/tmux
CMD_EXE ?= /mnt/c/Windows/System32/cmd.exe
LAUNCH_BIN ?= $(LIBEXECDIR)/tmux-wsl-launch

.PHONY: all clean install

all: tmux-wsl-workaround tmux-wsl-launch

tmux-wsl-workaround: tmux.c
	$(CC) $(CPPFLAGS) -DTMUX_BIN=\"$(TMUX_BIN)\" -DCMD_EXE=\"$(CMD_EXE)\" -DLAUNCH_BIN=\"$(LAUNCH_BIN)\" $(CFLAGS) $(LDFLAGS) -o $@ tmux.c

tmux-wsl-launch: tmux-wsl-launch.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tmux-wsl-launch.c

install: tmux-wsl-workaround tmux-wsl-launch
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(LIBEXECDIR)
	install -m 0755 tmux-wsl-workaround $(DESTDIR)$(BINDIR)/tmux
	install -m 0755 tmux-wsl-launch $(DESTDIR)$(LAUNCH_BIN)

clean:
	rm -f tmux-wsl-workaround tmux-wsl-launch
