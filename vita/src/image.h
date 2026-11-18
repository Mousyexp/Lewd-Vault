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
// A JPEG of these pixels (saved pictures, thumbnails and the thumbnail cache: on the Vita a JPEG
// decodes faster than anything else, with libjpeg-turbo's NEON code). malloc'd, or NULL.
uint8_t *lv_encode_jpeg(const LvPixels *p, int quality, size_t *n);
// A JPEG's size without decoding it: 1 when it is one.
int lv_jpeg_size(const uint8_t *data, size_t n, int *w, int *h);
// A picture's size without decoding it (JPEG, PNG, WebP): 1 when known.
int lv_image_size(const uint8_t *data, size_t n, int *w, int *h);
// Rows y0 to y1 of a picture (its own pixels), fitted inside max_w x max_h: a slice of a tall strip,
// without decoding the rest (a webtoon's 700 x 22000 strip whole would be 60 MB). 0 on success.
int lv_decode_rows(const uint8_t *data, size_t n, int y0, int y1, int max_w, int max_h, LvPixels *out);
// A tall picture cut into bands of band_h of its own rows, top to bottom, each fitted to max_w and
// handed to cb (it owns px->rgba; returning non-zero stops). Returns how many bands, or -1.
int lv_decode_bands(const uint8_t *data, size_t n, int band_h, int max_w, int (*cb)(void *ctx, int k, LvPixels *px), void *ctx);
// The most a progressive JPEG's compressed coefficients took since the last ask (and reset).
size_t lv_jpeg_store_peak(void);
