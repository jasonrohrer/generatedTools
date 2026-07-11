/* stbiw.c -- compile the stb_image_write PNG encoder as its own object.
 * stb headers are not strict-C89 clean, so the Makefile builds this with a
 * relaxed standard while the rest of the app stays -std=c89.  obliqueVoxels.c
 * declares stbi_write_png() itself rather than including the big header. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
