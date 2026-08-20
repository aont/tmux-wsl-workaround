CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -std=c99 -Wall -Wextra -O2
LDFLAGS ?=
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
TMUX_BIN ?= /usr/bin/tmux
CONSOLE_REDIRECT_EXE ?= $(BINDIR)/console_redirect.exe
WINDOWS_CC ?= x86_64-w64-mingw32-gcc
WINDOWS_CFLAGS ?= -std=c99 -Wall -Wextra -O2

.PHONY: all clean install

all: tmux-wsl-workaround console_redirect.exe

tmux-wsl-workaround: tmux.c
	$(CC) $(CPPFLAGS) -DTMUX_BIN=\"$(TMUX_BIN)\" -DCONSOLE_REDIRECT_EXE=\"$(CONSOLE_REDIRECT_EXE)\" $(CFLAGS) $(LDFLAGS) -o $@ tmux.c

console_redirect.exe: console_redirect.c
	$(WINDOWS_CC) $(WINDOWS_CFLAGS) -o $@ $<

install: tmux-wsl-workaround console_redirect.exe
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 tmux-wsl-workaround $(DESTDIR)$(BINDIR)/tmux
	install -m 0755 console_redirect.exe $(DESTDIR)$(BINDIR)/console_redirect.exe

clean:
	rm -f tmux-wsl-workaround console_redirect.exe
