// Colour quantization: RGBA images to 256-colour palettes, as the shell's
// LiveArea images expect.
#pragma once

#include <stdint.h>

// Reduces an RGBA image to at most 256 colours (median cut). Writes one
// palette index per pixel to `indices` (w*h bytes) and the palette as RGBA
// to `palette` (256*4 bytes). With `dither`, error diffusion softens banding
// in gradients. Returns the number of palette entries, or -1 when out of memory.
int quant_rgba(const uint8_t *rgba, int w, int h, int dither, uint8_t *indices, uint8_t *palette);
