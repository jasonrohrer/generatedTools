# Makefile for cwave -- a fast, stable audio editor.
#
# Application code (cwave.c, audio.c) is pure C89.
# Dear ImGui and its SDL2/OpenGL2 backends are C++ (compiled here).
# stb_vorbis.c is C.  Everything is statically linked into one binary.

CC      = gcc
CXX     = g++

SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LIBS   = $(shell sdl2-config --libs)

# strict C89 for our own code; warnings on
CFLAGS   = -std=c89 -Wall -Wextra -O2 $(SDL_CFLAGS)
# stb_vorbis is not c89-clean; build it with a relaxed standard
STBFLAGS = -std=gnu99 -O2 -Wno-unused-value

IMGUI_DIR = imgui
CXXFLAGS  = -O2 -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends $(SDL_CFLAGS) \
            -DIMGUI_IMPL_OPENGL_LOADER_CUSTOM

LIBS = $(SDL_LIBS) -lGL -lpthread -lm

IMGUI_SRC = $(IMGUI_DIR)/imgui.cpp \
            $(IMGUI_DIR)/imgui_draw.cpp \
            $(IMGUI_DIR)/imgui_tables.cpp \
            $(IMGUI_DIR)/imgui_widgets.cpp \
            $(IMGUI_DIR)/backends/imgui_impl_sdl2.cpp \
            $(IMGUI_DIR)/backends/imgui_impl_opengl2.cpp

IMGUI_OBJ = $(IMGUI_SRC:.cpp=.o)

OBJ = cwave.o audio.o gui.o stb_vorbis.o $(IMGUI_OBJ)

all: cwave

cwave: $(OBJ)
	$(CXX) -o $@ $(OBJ) $(LIBS)

cwave.o: cwave.c audio.h gui.h
	$(CC) $(CFLAGS) -c -o $@ cwave.c

audio.o: audio.c audio.h
	$(CC) $(CFLAGS) -c -o $@ audio.c

stb_vorbis.o: stb_vorbis.c
	$(CC) $(STBFLAGS) -c -o $@ stb_vorbis.c

gui.o: gui.cpp gui.h
	$(CXX) $(CXXFLAGS) -c -o $@ gui.cpp

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f cwave $(OBJ)

.PHONY: all clean
