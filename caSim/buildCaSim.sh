#!/bin/bash
#
# Builds the caSim headless simulator.
#
# caSim.c          the simulator and scripted player (pure C89)
# caSimMingin.c    compiles the null (headless) mingin platform
# maxigin_imp.c    the stock maxigin implementation from the game source
#
# Uses the same strict C89 flags as the game's hardcoreC89Compile.sh,
# plus -O3 (the chess AI dominates run time).
# -Wno-maybe-uninitialized is needed because at -O3 gcc's flow analysis
# reports a false positive inside chess.h's getGreedyDepthMove (the game
# itself is compiled without optimization, where this doesn't fire).

CA=Chessamphetamine

FLAGS="-O3 -std=c89 -fno-builtin -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes -Wold-style-definition -Wmissing-prototypes -Wmissing-declarations -Wdeclaration-after-statement -Wno-maybe-uninitialized"

gcc $FLAGS -I $CA -o caSim caSim.c caSimMingin.c $CA/maxigin_imp.c
