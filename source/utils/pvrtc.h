#ifndef DM_PVRTC_H
#define DM_PVRTC_H
#include <stdint.h>
/* Decode PVRTC 1 data (bpp 2 or 4) of a width x height level to a new malloc'd RGBA8888 buffer (NULL on failure). */
uint8_t *pvrtc_decode(const void *data, int width, int height, int bpp);
#endif
