#!/bin/bash
#
# Builds Path Master.
#
# Pure C89, linked only against SDL 2 and OpenGL (plus the C math lib).

FLAGS="-O2 -std=c89 -pedantic -Wall -Wextra -Wshadow -Wstrict-prototypes -Wold-style-definition -Wmissing-prototypes -Wdeclaration-after-statement"

gcc $FLAGS $(sdl2-config --cflags) -o pathMaster \
    main.c gfx.c art.c text.c sound.c logic.c ui.c \
    $(sdl2-config --libs) -lGL -lm
