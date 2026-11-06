# serenity-input-bridge: evdev -> SerenityOS input records.
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11

serenity-input-bridge: serenity_input_bridge.c serenity_keycode.h
	$(CC) $(CFLAGS) -o $@ serenity_input_bridge.c

clean:
	rm -f serenity-input-bridge

install: serenity-input-bridge
	install -m755 serenity-input-bridge /usr/local/bin/

.PHONY: clean install
