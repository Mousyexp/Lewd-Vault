#include "image.h"
#include <png.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>
#include <jpeglib.h>
#include <webp/decode.h>

static void fit(int w, int h, int max_w, int max_h, int *ow, int *oh) {
  double s = 1.0;
  if (w > max_w) s = (double)max_w / w;
  if (h * s > max_h) s = (double)max_h / h;
  *ow = (int)(w * s + 0.5);
  *oh = (int)(h * s + 0.5);
  if (*ow < 1) *ow = 1;
  if (*oh < 1) *oh = 1;
}

// Box-filtered shrink (for JPEG's last step and PNG): each output pixel averages its source area.
static uint8_t *shrink(const uint8_t *restrict src, int w, int h, int ow, int oh) {
  uint8_t *restrict out = malloc((size_t)ow * oh * 4);
  if (!out) return NULL;
  for (int y = 0; y < oh; y++) {
    int y0 = (int)((long long)y * h / oh), y1 = (int)((long long)(y + 1) * h / oh);
    if (y1 <= y0) y1 = y0 + 1;
    for (int x = 0; x < ow; x++) {
      int x0 = (int)((long long)x * w / ow), x1 = (int)((long long)(x + 1) * w / ow);
      if (x1 <= x0) x1 = x0 + 1;
      unsigned s[4] = {0}, n = 0;
      // (a big shrink samples a 4 x 4 grid of its area: enough for a picture, and quick)
      int sy = (y1 - y0 + 3) / 4, sx = (x1 - x0 + 3) / 4;
      for (int yy = y0; yy < y1; yy += sy)
        for (int xx = x0; xx < x1; xx += sx) {
          const uint8_t *p = src + ((size_t)yy * w + xx) * 4;
          s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; s[3] += p[3]; n++;
        }
      uint8_t *o = out + ((size_t)y * ow + x) * 4;
      o[0] = s[0] / n; o[1] = s[1] / n; o[2] = s[2] / n; o[3] = s[3] / n;
    }
  }
  return out;
}

static int webp(const uint8_t *d, size_t n, int max_w, int max_h, LvPixels *out) {
  WebPDecoderConfig c;
  if (!WebPInitDecoderConfig(&c) || WebPGetFeatures(d, n, &c.input) != VP8_STATUS_OK) return -1;
  int ow, oh;
  fit(c.input.width, c.input.height, max_w, max_h, &ow, &oh);
  if (ow != c.input.width || oh != c.input.height) {
    c.options.use_scaling = 1;
    c.options.scaled_width = ow;
    c.options.scaled_height = oh;
  }
  // (a thumbnail doesn't need the finest filtering)
  if (ow * oh < 300 * 300) { c.options.bypass_filtering = 1; c.options.no_fancy_upsampling = 1; }
  c.output.colorspace = MODE_RGBA;
  c.output.is_external_memory = 1;
  out->rgba = malloc((size_t)ow * oh * 4);
  if (!out->rgba) return -1;
  c.output.u.RGBA.rgba = out->rgba;
  c.output.u.RGBA.stride = ow * 4;
  c.output.u.RGBA.size = (size_t)ow * oh * 4;
  if (WebPDecode(d, n, &c) != VP8_STATUS_OK) { free(out->rgba); out->rgba = NULL; return -1; }
  out->w = ow;
  out->h = oh;
  return 0;
}

static int jpeg(const uint8_t *d, size_t n, int max_w, int max_h, LvPixels *out) {
  tjhandle t = tjInitDecompress();
  if (!t) return -1;
  int w, h, sub, cs;
  if (tjDecompressHeader3(t, d, (unsigned long)n, &w, &h, &sub, &cs) != 0) { tjDestroy(t); return -1; }
  int ow, oh;
  fit(w, h, max_w, max_h, &ow, &oh);
  // the decoder shrinks by 1/2, 1/4, 1/8 for free: the smallest one still at least the size wanted
  int nf = 0;
  tjscalingfactor *sf = tjGetScalingFactors(&nf);
  int dw = w, dh = h;
  for (int k = 0; k < nf; k++) {
    int sw = TJSCALED(w, sf[k]), sh = TJSCALED(h, sf[k]);
    if (sw >= ow && sh >= oh && sw * sh < dw * dh) { dw = sw; dh = sh; }
  }
  uint8_t *buf = malloc((size_t)dw * dh * 4);
  if (!buf || tjDecompress2(t, d, (unsigned long)n, buf, dw, 0, dh, TJPF_RGBA, TJFLAG_FASTDCT) != 0) {
    free(buf); tjDestroy(t); return -1;
  }
  tjDestroy(t);
  if (dw != ow || dh != oh) {
    uint8_t *s = shrink(buf, dw, dh, ow, oh);
    free(buf);
    if (!s) return -1;
    buf = s;
  }
  out->rgba = buf;
  out->w = ow;
  out->h = oh;
  return 0;
}

static int png(const uint8_t *d, size_t n, int max_w, int max_h, LvPixels *out) {
  png_image im;
  memset(&im, 0, sizeof im);
  im.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&im, d, n)) return -1;
  // (a PNG has no cheap way to decode small: refuse the absurd rather than run out of memory)
  if ((unsigned long long)im.width * im.height > 12ull * 1000 * 1000) { png_image_free(&im); return -1; }
  im.format = PNG_FORMAT_RGBA;
  uint8_t *buf = malloc(PNG_IMAGE_SIZE(im));
  if (!buf || !png_image_finish_read(&im, NULL, buf, 0, NULL)) { free(buf); png_image_free(&im); return -1; }
  int ow, oh;
  fit((int)im.width, (int)im.height, max_w, max_h, &ow, &oh);
  if (ow != (int)im.width || oh != (int)im.height) {
    uint8_t *s = shrink(buf, (int)im.width, (int)im.height, ow, oh);
    free(buf);
    if (!s) return -1;
    buf = s;
  }
  out->rgba = buf;
  out->w = ow;
  out->h = oh;
  return 0;
}

int lv_decode(const uint8_t *d, size_t n, int max_w, int max_h, LvPixels *out) {
  memset(out, 0, sizeof *out);
  if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return webp(d, n, max_w, max_h, out);
  if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) return jpeg(d, n, max_w, max_h, out);
  if (n >= 8 && !memcmp(d, "\x89PNG", 4)) return png(d, n, max_w, max_h, out);
  return -1;
}

uint8_t *lv_encode_jpeg(const LvPixels *p, int quality, size_t *n) {
  tjhandle t = tjInitCompress();
  if (!t) return NULL;
  unsigned char *o = NULL;
  unsigned long len = 0;
  int r = tjCompress2(t, p->rgba, p->w, p->w * 4, p->h, TJPF_RGBA, &o, &len, TJSAMP_420, quality, TJFLAG_FASTDCT);
  tjDestroy(t);
  if (r != 0 || !o) { if (o) tjFree(o); return NULL; }
  // (turbojpeg's own allocation: copied so the caller frees it with free())
  uint8_t *b = malloc(len);
  if (b) memcpy(b, o, len);
  tjFree(o);
  *n = len;
  return b;
}

int lv_jpeg_size(const uint8_t *d, size_t n, int *w, int *h) {
  if (n < 3 || d[0] != 0xFF || d[1] != 0xD8) return 0;
  tjhandle t = tjInitDecompress();
  if (!t) return 0;
  int sub, cs;
  int ok = tjDecompressHeader3(t, d, (unsigned long)n, w, h, &sub, &cs) == 0;
  tjDestroy(t);
  return ok;
}

int lv_image_size(const uint8_t *d, size_t n, int *w, int *h) {
  if (lv_jpeg_size(d, n, w, h)) return 1;
  if (n >= 24 && !memcmp(d, "\x89PNG", 4)) {
    *w = d[16] << 24 | d[17] << 16 | d[18] << 8 | d[19];
    *h = d[20] << 24 | d[21] << 16 | d[22] << 8 | d[23];
    return 1;
  }
  if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return WebPGetInfo(d, n, w, h) ? 1 : 0;
  return 0;
}

// ----- a band of rows: a slice of a webtoon's strip (700 x 22000 and more), never decoded whole -----
static int band_shrink(uint8_t *buf, int dw, int dh, int ow, int oh, LvPixels *out) {
  if (dw != ow || dh != oh) {
    uint8_t *s = shrink(buf, dw, dh, ow, oh);
    free(buf);
    if (!s) return -1;
    buf = s;
  }
  out->rgba = buf; out->w = ow; out->h = oh;
  return 0;
}
struct jerr { struct jpeg_error_mgr m; jmp_buf jb; };
static void jerr_exit(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }
static void jerr_quiet(j_common_ptr c) { (void)c; }
// (JPEG: the rows above are skipped, not turned into pixels; those below never read)
static int jpeg_rows(const uint8_t *d, size_t n, int y0, int y1, int max_w, int max_h, LvPixels *out) {
  struct jpeg_decompress_struct c;
  struct jerr e;
  uint8_t *volatile buf = NULL;
  c.err = jpeg_std_error(&e.m);
  e.m.error_exit = jerr_exit;
  e.m.output_message = jerr_quiet;
  if (setjmp(e.jb)) { jpeg_destroy_decompress(&c); free(buf); return -1; }
  jpeg_create_decompress(&c);
  jpeg_mem_src(&c, (unsigned char *)d, (unsigned long)n);
  jpeg_read_header(&c, TRUE);
  int w = (int)c.image_width, h = (int)c.image_height;
  if (y1 > h) y1 = h;
  if (y0 < 0) y0 = 0;
  if (y1 <= y0) { jpeg_destroy_decompress(&c); return -1; }
  int ow, oh;
  fit(w, y1 - y0, max_w, max_h, &ow, &oh);
  // (shrunk by the decoder for free, 1/2 to 1/8, while still at least the size wanted)
  int den = 1;
  while (den < 8 && w / (den * 2) >= ow && (y1 - y0) / (den * 2) >= oh) den *= 2;
  c.scale_num = 1; c.scale_denom = den;
  c.out_color_space = JCS_EXT_RGBA;
  c.dct_method = JDCT_IFAST;
  jpeg_start_decompress(&c);
  int dw = (int)c.output_width, sy0 = y0 / den, sy1 = (y1 + den - 1) / den;
  if (sy1 > (int)c.output_height) sy1 = (int)c.output_height;
  int dh = sy1 - sy0;
  if (dh < 1 || !(buf = malloc((size_t)dw * dh * 4))) { jpeg_destroy_decompress(&c); return -1; }
  if (sy0 > 0) jpeg_skip_scanlines(&c, (JDIMENSION)sy0);
  while ((int)c.output_scanline < sy1) {
    JSAMPROW r = buf + (size_t)((int)c.output_scanline - sy0) * dw * 4;
    if (jpeg_read_scanlines(&c, &r, 1) != 1) break;
  }
  jpeg_abort_decompress(&c);
  jpeg_destroy_decompress(&c);
  if (ow > dw) ow = dw;
  if (oh > dh) oh = dh;
  return band_shrink(buf, dw, dh, ow, oh, out);
}
// (WebP: the decoder crops it)
static int webp_rows(const uint8_t *d, size_t n, int y0, int y1, int max_w, int max_h, LvPixels *out) {
  WebPDecoderConfig c;
  if (!WebPInitDecoderConfig(&c) || WebPGetFeatures(d, n, &c.input) != VP8_STATUS_OK) return -1;
  int w = c.input.width, h = c.input.height;
  y0 &= ~1; // (lossy WebP crops at an even row)
  if (y1 > h) y1 = h;
  if (y0 < 0) y0 = 0;
  if (y1 <= y0) return -1;
  int ow, oh;
  fit(w, y1 - y0, max_w, max_h, &ow, &oh);
  c.options.use_cropping = 1;
  c.options.crop_left = 0; c.options.crop_top = y0; c.options.crop_width = w; c.options.crop_height = y1 - y0;
  if (ow != w || oh != y1 - y0) { c.options.use_scaling = 1; c.options.scaled_width = ow; c.options.scaled_height = oh; }
  c.output.colorspace = MODE_RGBA;
  c.output.is_external_memory = 1;
  uint8_t *buf = malloc((size_t)ow * oh * 4);
  if (!buf) return -1;
  c.output.u.RGBA.rgba = buf; c.output.u.RGBA.stride = ow * 4; c.output.u.RGBA.size = (size_t)ow * oh * 4;
  if (WebPDecode(d, n, &c) != VP8_STATUS_OK) { free(buf); return -1; }
  out->rgba = buf; out->w = ow; out->h = oh;
  return 0;
}
// (PNG: read a row at a time, the rows of the band kept)
struct pngsrc { const uint8_t *d; size_t n, at; };
static void png_from_mem(png_structp p, png_bytep to, png_size_t len) {
  struct pngsrc *s = png_get_io_ptr(p);
  if (s->at + len > s->n) png_error(p, "short");
  memcpy(to, s->d + s->at, len);
  s->at += len;
}
static int png_rows(const uint8_t *d, size_t n, int y0, int y1, int max_w, int max_h, LvPixels *out) {
  png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!p) return -1;
  png_infop info = png_create_info_struct(p);
  uint8_t *volatile buf = NULL, *volatile row = NULL;
  struct pngsrc src = {d, n, 0};
  if (!info || setjmp(png_jmpbuf(p))) { png_destroy_read_struct(&p, info ? &info : NULL, NULL); free(buf); free(row); return -1; }
  png_set_read_fn(p, &src, png_from_mem);
  png_read_info(p, info);
  int w = (int)png_get_image_width(p, info), h = (int)png_get_image_height(p, info);
  if (png_get_interlace_type(p, info) != PNG_INTERLACE_NONE) png_error(p, "interlaced"); // (needs it all)
  png_set_expand(p); png_set_strip_16(p); png_set_gray_to_rgb(p); png_set_filler(p, 0xFF, PNG_FILLER_AFTER);
  png_read_update_info(p, info);
  if (y1 > h) y1 = h;
  if (y0 < 0) y0 = 0;
  if (y1 <= y0) png_error(p, "empty");
  int dh = y1 - y0;
  buf = malloc((size_t)w * dh * 4);
  row = malloc((size_t)w * 4);
  if (!buf || !row) png_error(p, "memory");
  for (int y = 0; y < y1; y++) png_read_row(p, y >= y0 ? buf + (size_t)(y - y0) * w * 4 : row, NULL);
  png_destroy_read_struct(&p, &info, NULL);
  free(row);
  int ow, oh;
  fit(w, dh, max_w, max_h, &ow, &oh);
  return band_shrink(buf, w, dh, ow, oh, out);
}
int lv_decode_rows(const uint8_t *d, size_t n, int y0, int y1, int max_w, int max_h, LvPixels *out) {
  memset(out, 0, sizeof *out);
  if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return webp_rows(d, n, y0, y1, max_w, max_h, out);
  if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) return jpeg_rows(d, n, y0, y1, max_w, max_h, out);
  if (n >= 8 && !memcmp(d, "\x89PNG", 4)) return png_rows(d, n, y0, y1, max_w, max_h, out);
  return -1;
}

// A tall picture cut into bands of band_h of its rows, top to bottom, each fitted to max_w and given
// to cb (which owns its pixels; non-zero stops). A JPEG is read once, a row at a time: a progressive
// one has to be (libjpeg keeps the whole strip's coefficients for it), and it's quickest anyway.
// Returns how many bands, or -1.
int lv_decode_bands(const uint8_t *d, size_t n, int band_h, int max_w, int (*cb)(void *ctx, int k, LvPixels *px), void *ctx) {
  if (band_h < 16) return -1;
  int w = 0, h = 0;
  if (!lv_image_size(d, n, &w, &h) || w < 1 || h < 1) return -1;
  int nb = (h + band_h - 1) / band_h;
  if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) {
    // (WebP: decoded once, whole (it can't be more than 16383 rows), then cut: a crop is decoded from
    //  the top each time)
    LvPixels all;
    if (webp(d, n, max_w, 1 << 20, &all) != 0) return -1;
    int bh = (int)((long long)band_h * all.h / h), done = 0;
    if (bh < 1) bh = 1;
    for (int k = 0; k * bh < all.h; k++) {
      int dh = (k + 1) * bh > all.h ? all.h - k * bh : bh;
      LvPixels px = {malloc((size_t)all.w * dh * 4), all.w, dh};
      if (!px.rgba) break;
      memcpy(px.rgba, all.rgba + (size_t)k * bh * all.w * 4, (size_t)all.w * dh * 4);
      done = k + 1;
      if (cb(ctx, k, &px)) break;
    }
    free(all.rgba);
    return done ? done : -1;
  }
  if (!(n >= 3 && d[0] == 0xFF && d[1] == 0xD8)) {
    // (PNG: each band on its own)
    for (int k = 0; k < nb; k++) {
      LvPixels px;
      if (lv_decode_rows(d, n, k * band_h, (k + 1) * band_h, max_w, 1 << 20, &px) != 0) return -1;
      if (cb(ctx, k, &px)) return k + 1;
    }
    return nb;
  }
  struct jpeg_decompress_struct c;
  struct jerr e;
  uint8_t *volatile buf = NULL;
  volatile int done = 0;
  c.err = jpeg_std_error(&e.m);
  e.m.error_exit = jerr_exit;
  e.m.output_message = jerr_quiet;
  if (setjmp(e.jb)) { jpeg_destroy_decompress(&c); free(buf); return done ? done : -1; }
  jpeg_create_decompress(&c);
  jpeg_mem_src(&c, (unsigned char *)d, (unsigned long)n);
  jpeg_read_header(&c, TRUE);
  int den = 1;
  while (den < 8 && w / (den * 2) >= max_w) den *= 2;
  c.scale_num = 1; c.scale_denom = den;
  c.out_color_space = JCS_EXT_RGBA;
  c.dct_method = JDCT_IFAST;
  jpeg_start_decompress(&c);
  int dw = (int)c.output_width, oh_all = (int)c.output_height, bh = (band_h + den - 1) / den;
  for (int k = 0; k < nb; k++) {
    int y0 = k * bh, y1 = y0 + bh > oh_all ? oh_all : y0 + bh, dh = y1 - y0;
    if (dh < 1) break;
    if (!(buf = malloc((size_t)dw * dh * 4))) break;
    while ((int)c.output_scanline < y1) {
      JSAMPROW r = buf + (size_t)((int)c.output_scanline - y0) * dw * 4;
      if (jpeg_read_scanlines(&c, &r, 1) != 1) break;
    }
    int ow, oh;
    fit(dw, dh, max_w, 1 << 20, &ow, &oh);
    LvPixels px;
    uint8_t *b = buf;
    buf = NULL;
    if (band_shrink(b, dw, dh, ow, oh, &px) != 0) break;
    done = k + 1;
    if (cb(ctx, k, &px)) break;
  }
  jpeg_abort_decompress(&c);
  jpeg_destroy_decompress(&c);
  return done ? done : -1;
}
