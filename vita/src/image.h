// Pictures to pixels: WebP (the vault's thumbnails and most pictures), JPEG and PNG, decoded at no
// more than the size they're shown at (decoding a 900 x 1200 thumbnail whole for a 152 px card
// wastes the Vita's time and memory).
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t *rgba; // malloc'd, w * h * 4, rows packed
  int w, h;
} LvPixels;

// Fits inside max_w x max_h (never larger than the picture itself). 0 on success.
int lv_decode(const uint8_t *data, size_t n, int max_w, int max_h, LvPixels *out);
// A small WebP of these pixels (the thumbnail cache). malloc'd, or NULL.
uint8_t *lv_encode_webp(const LvPixels *p, int quality, size_t *n);
