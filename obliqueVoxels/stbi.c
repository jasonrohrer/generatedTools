/* stbi.c -- compile the stb_image PNG reader as its own object.
 *
 * Kept in a separate translation unit (like stbiw.c) because stb_image.h is
 * not C89-clean; the Makefile builds it with a relaxed standard.  The main
 * app only forward-declares the two symbols it uses (stbi_load /
 * stbi_image_free).  We only need PNG decoding, so trim the other codecs. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
