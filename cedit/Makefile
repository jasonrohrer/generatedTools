# Makefile for cedit -- a fast editor for C89 code.
#
# Application code (*.c) is pure C89, compiled with zero warnings.
# Dear ImGui and its SDL2 / OpenGL2 backends are C++ (compiled here),
# walled off behind gui.h / gui.cpp.
# Only linked dependencies:  SDL2 and OpenGL.

CC      = gcc
CXX     = g++

SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LIBS   = $(shell sdl2-config --libs)

# _DEFAULT_SOURCE:  POSIX bits (fork, pipe, realpath, opendir) under -std=c89
CFLAGS   = -std=c89 -pedantic -Wall -Wextra -Wshadow \
           -Wdeclaration-after-statement -Wstrict-prototypes \
           -Wmissing-prototypes -Wno-long-long \
           -D_DEFAULT_SOURCE -O2 -g $(SDL_CFLAGS)

IMGUI_DIR = imgui
CXXFLAGS  = -O2 -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends $(SDL_CFLAGS)

LIBS = $(SDL_LIBS) -lGL -lm

IMGUI_SRC = $(IMGUI_DIR)/imgui.cpp \
            $(IMGUI_DIR)/imgui_draw.cpp \
            $(IMGUI_DIR)/imgui_tables.cpp \
            $(IMGUI_DIR)/imgui_widgets.cpp \
            $(IMGUI_DIR)/backends/imgui_impl_sdl2.cpp \
            $(IMGUI_DIR)/backends/imgui_impl_opengl2.cpp

IMGUI_OBJ = $(IMGUI_SRC:.cpp=.o)

C_OBJ = cedit.o editor.o textBuffer.o syntax.o indent.o format.o \
        config.o compile.o fontData.o

OBJ = $(C_OBJ) gui.o $(IMGUI_OBJ)

all: cedit

cedit: $(OBJ)
	$(CXX) -o $@ $(OBJ) $(LIBS)

cedit.o: cedit.c editor.h gui.h config.h syntax.h indent.h format.h \
         compile.h textBuffer.h
editor.o: editor.c editor.h syntax.h indent.h format.h config.h compile.h \
          textBuffer.h
textBuffer.o: textBuffer.c textBuffer.h
syntax.o: syntax.c syntax.h textBuffer.h
indent.o: indent.c indent.h syntax.h config.h textBuffer.h
format.o: format.c format.h indent.h syntax.h config.h textBuffer.h
config.o: config.c config.h syntax.h textBuffer.h
compile.o: compile.c compile.h textBuffer.h

# the embedded font is just data;  no need for the strict flags
fontData.o: fontData.c
	$(CC) -O0 -c -o $@ fontData.c

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

gui.o: gui.cpp gui.h
	$(CXX) $(CXXFLAGS) -c -o $@ gui.cpp

$(IMGUI_DIR)/%.o: $(IMGUI_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f cedit $(OBJ)

.PHONY: all clean
