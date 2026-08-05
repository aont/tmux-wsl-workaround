CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -std=c99 -Wall -Wextra -O2
LDFLAGS ?=
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
TMUX_BIN ?= /usr/bin/tmux
CMD_EXE ?= /mnt/c/Windows/System32/cmd.exe

.PHONY: all clean install

all: tmux-wsl-workaround

tmux-wsl-workaround: tmux.c
	$(CC) $(CPPFLAGS) -DTMUX_BIN=\"$(TMUX_BIN)\" -DCMD_EXE=\"$(CMD_EXE)\" $(CFLAGS) $(LDFLAGS) -o $@ tmux.c

install: tmux-wsl-workaround
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 tmux-wsl-workaround $(DESTDIR)$(BINDIR)/tmux

clean:
	rm -f tmux-wsl-workaround
