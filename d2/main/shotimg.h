#ifndef _SHOTIMG_H
#define _SHOTIMG_H

#include <stddef.h>
#include "pstypes.h"
#include "gr.h"

// Decodes a screenshot (.png or .tga) or game art (.pcx) to tightly packed RGB or RGBA.
// Returns a d_malloc'd buffer the caller d_frees, or NULL.
ubyte *shotimg_decode(const char *path, int *width, int *height, int *channels);

// Same, for a PNG already in memory. Always returns RGBA (channels = 4).
ubyte *shotimg_decode_memory(const void *png, size_t size, int *width, int *height, int *channels);

#endif
