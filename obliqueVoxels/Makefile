# Makefile for obliqueVoxels -- a voxel-to-pixel-art creation tool.
#
# Application code (obliqueVoxels.c) is pure C89.
# Dear ImGui and its SDL2/OpenGL2 backends are C++ (compiled here).
# The stb_image_write.h PNG writer is header-only C.
# Everything is statically linked into one binary.

CC      = gcc
CXX     = g++

SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LIBS   = $(shell sdl2-config --libs)

# strict C89 for our own code; warnings on
CFLAGS   = -std=c89 -Wall -Wextra -O2 $(SDL_CFLAGS)
# stb_image_write is not c89-clean; build it with a relaxed standard
STBFLAGS = -std=gnu99 -O2 -Wno-unused-parameter

IMGUI_DIR = imgui
CXXFLAGS  = -O2 -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends $(SDL_CFLAGS) \
            -DIMGUI_IMPL_OPENGL_LOADER_CUSTOM

# OpenGL + GLU (gluPerspective / gluLookAt / gluUnProject for the 3D view).
LIBS = $(SDL_LIBS) -lGLU -lGL -lm

IMGUI_SRC = $(IMGUI_DIR)/imgui.cpp \
            $(IMGUI_DIR)/imgui_draw.cpp \
            $(IMGUI_DIR)/imgui_tables.cpp \
            $(IMGUI_DIR)/imgui_widgets.cpp \
            $(IMGUI_DIR)/backends/imgui_impl_sdl2.cpp \
            $(IMGUI_DIR)/backends/imgui_impl_opengl2.cpp

IMGUI_OBJ = $(IMGUI_SRC:.cpp=.o)

OBJ = obliqueVoxels.o gui.o stbiw.o stbi.o fs.o $(IMGUI_OBJ)

all: obliqueVoxels

obliqueVoxels: $(OBJ)
	$(CXX) -o $@ $(OBJ) $(LIBS)

obliqueVoxels.o: obliqueVoxels.c gui.h
	$(CC) $(CFLAGS) -c -o $@ obliqueVoxels.c

stbiw.o: stbiw.c stb_image_write.h
	$(CC) $(STBFLAGS) -c -o $@ stbiw.c

stbi.o: stbi.c stb_image.h
	$(CC) $(STBFLAGS) -c -o $@ stbi.c

fs.o: fs.c fs.h
	$(CC) $(STBFLAGS) -c -o $@ fs.c

gui.o: gui.cpp gui.h
	$(CXX) $(CXXFLAGS) -c -o $@ gui.cpp

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f obliqueVoxels obliqueVoxels.o gui.o stbiw.o stbi.o fs.o

# Leave the (slow-to-build) imgui objects in place on clean; use realclean to
# force a full imgui rebuild.
realclean:
	rm -f obliqueVoxels $(OBJ)

.PHONY: all clean realclean
