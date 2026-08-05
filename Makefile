CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -O2
LDFLAGS ?=
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean install

all: tmux-wsl-workaround

tmux-wsl-workaround: tmux.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tmux.c

install: tmux-wsl-workaround
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 tmux-wsl-workaround $(DESTDIR)$(BINDIR)/tmux

clean:
	rm -f tmux-wsl-workaround
