CC ?= cc
CFLAGS ?= -Os -Wall -Wextra -Werror -D_FORTIFY_SOURCE=3 -fstack-protector-strong
PREFIX ?= $(HOME)/.local

thaw: thaw.c
	$(CC) $(CFLAGS) -o $@ thaw.c -lX11 -lX11-xcb -lxcb -lxcb-screensaver -lXext -lXRes -lpthread

install: thaw
	install -Dm755 thaw $(PREFIX)/bin/thaw

clean:
	rm -f thaw

.PHONY: install clean
