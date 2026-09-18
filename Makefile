CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
TMUX_PATH ?= $(shell command -v tmux 2>/dev/null)

.PHONY: all clean test
all: wrapper

wrapper: tmux-wsl-wrapper.c
	@test -n '$(TMUX_PATH)' || { \
		echo 'error: tmux not found in PATH; set TMUX_PATH explicitly' >&2; \
		exit 1; \
	}
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTMUX_PATH='"$(TMUX_PATH)"' $(LDFLAGS) -o $@ $<


test:
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTMUX_PATH='"$(CURDIR)/tests/tmux-stub"' \
		-DCMD_EXE_PATH='"$(CURDIR)/tests/cmd-stub"' \
		-DCMD_WORKDIR='"$(CURDIR)"' $(LDFLAGS) -o wrapper-test tmux-wsl-wrapper.c
	WRAPPER=$(CURDIR)/wrapper-test ./tests/test.sh
	rm -f wrapper-test

clean:
	rm -f wrapper wrapper-test
