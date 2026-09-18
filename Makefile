CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

.PHONY: all clean test
all: wrapper

wrapper: tmux-wsl-wrapper.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

test: wrapper
	$(CC) $(CFLAGS) -DCMD_EXE_PATH='"$(CURDIR)/tests/cmd-stub"' \
		-DCMD_WORKDIR='"$(CURDIR)"' $(LDFLAGS) -o wrapper-test tmux-wsl-wrapper.c
	WRAPPER=$(CURDIR)/wrapper-test ./tests/test.sh
	rm -f wrapper-test

clean:
	rm -f wrapper wrapper-test
