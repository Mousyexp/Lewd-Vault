#include "image.h"
#include <png.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>
#include <jpeglib.h>
#include <jerror.h>
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

// ----- libjpeg's memory: its system part (jmemnobs.c's, replaced here) -----
// A progressive JPEG is drawn by several scans over the whole picture, so libjpeg keeps every
// block's coefficients until the last one: for a webtoon's 700 x 23000 strip that's ~50 MB. What
// doesn't fit the budget it's given (max_memory_to_use) it hands to a "backing store" in pieces, a
// band of block rows at a time. Ours keeps them in RAM, compressed: coefficients are mostly zeros, so
// a page of them is a few runs. The decoder then holds one window of rows, plus the compressed rest.
typedef struct backing_store_struct {
  void (*read_backing_store)(j_common_ptr cinfo, struct backing_store_struct *info, void *buffer_address, long file_offset, long byte_count);
  void (*write_backing_store)(j_common_ptr cinfo, struct backing_store_struct *info, void *buffer_address, long file_offset, long byte_count);
  void (*close_backing_store)(j_common_ptr cinfo, struct backing_store_struct *info);
  void *store; // (where jmemnobs.c keeps its FILE *: the struct is libjpeg's, no bigger than that)
} lv_backing_store;
void *jpeg_get_small(j_common_ptr cinfo, size_t n) { (void)cinfo; return malloc(n); }
void jpeg_free_small(j_common_ptr cinfo, void *p, size_t n) { (void)cinfo; (void)n; free(p); }
void *jpeg_get_large(j_common_ptr cinfo, size_t n) { (void)cinfo; return malloc(n); }
void jpeg_free_large(j_common_ptr cinfo, void *p, size_t n) { (void)cinfo; (void)n; free(p); }
size_t jpeg_mem_available(j_common_ptr cinfo, size_t min_bytes_needed, size_t max_bytes_needed, size_t already_allocated) {
  (void)min_bytes_needed;
  long cap = cinfo->mem->max_memory_to_use;
  if (!cap) return max_bytes_needed; // (no budget: all of it in memory, as before)
  return (size_t)cap > already_allocated ? (size_t)cap - already_allocated : 0;
}
long jpeg_mem_init(j_common_ptr cinfo) { (void)cinfo; return 0; }
void jpeg_mem_term(j_common_ptr cinfo) { (void)cinfo; }

#define CS_PAGE 8192 // (bytes of coefficients a compressed page holds)
typedef struct { long npages; uint8_t **pg; uint16_t *len; uint8_t tmp[CS_PAGE], out[CS_PAGE + 8]; size_t held; } Cstore;
static size_t cs_peak; // (the most the compressed pages took: for the log)
// a page of int16s as runs: [zeros][count][that many values], or raw when that's no smaller
static size_t cs_pack(const uint8_t *in, uint8_t *out) {
  const int16_t *v = (const int16_t *)in;
  const int nv = CS_PAGE / 2;
  size_t o = 0;
  for (int i = 0; i < nv;) {
    int z = 0, l = 0;
    while (i + z < nv && !v[i + z]) z++;
    while (i + z + l < nv && v[i + z + l]) l++;
    if (o + 4 + l * 2 >= CS_PAGE) return CS_PAGE; // (raw is smaller)
    uint16_t hz = (uint16_t)z, hl = (uint16_t)l;
    memcpy(out + o, &hz, 2); memcpy(out + o + 2, &hl, 2);
    memcpy(out + o + 4, v + i + z, (size_t)l * 2);
    o += 4 + (size_t)l * 2;
    i += z + l;
  }
  return o;
}
static void cs_unpack(const uint8_t *in, size_t n, uint8_t *out) {
  if (n == CS_PAGE) { memcpy(out, in, CS_PAGE); return; }
  memset(out, 0, CS_PAGE);
  size_t o = 0, i = 0;
  while (i + 4 <= n) {
    uint16_t z, l;
    memcpy(&z, in + i, 2); memcpy(&l, in + i + 2, 2);
    o += (size_t)z * 2;
    if (o + (size_t)l * 2 > CS_PAGE) break;
    memcpy(out + o, in + i + 4, (size_t)l * 2);
    o += (size_t)l * 2;
    i += 4 + (size_t)l * 2;
  }
}
static void cs_fail(j_common_ptr cinfo) { cinfo->err->msg_code = JERR_OUT_OF_MEMORY; cinfo->err->msg_parm.i[0] = 99; cinfo->err->error_exit(cinfo); }
static void cs_read(j_common_ptr cinfo, struct backing_store_struct *info, void *buf, long off, long count) {
  (void)cinfo;
  Cstore *c = info->store;
  uint8_t *b = buf;
  while (count > 0) {
    long p = off / CS_PAGE, at = off % CS_PAGE, take = CS_PAGE - at < count ? CS_PAGE - at : count;
    if (p < c->npages && c->pg[p]) {
      if (at == 0 && take == CS_PAGE) cs_unpack(c->pg[p], c->len[p], b);
      else { cs_unpack(c->pg[p], c->len[p], c->tmp); memcpy(b, c->tmp + at, (size_t)take); }
    } else memset(b, 0, (size_t)take);
    b += take; off += take; count -= take;
  }
}
static void cs_write(j_common_ptr cinfo, struct backing_store_struct *info, void *buf, long off, long count) {
  Cstore *c = info->store;
  const uint8_t *b = buf;
  while (count > 0) {
    long p = off / CS_PAGE, at = off % CS_PAGE, take = CS_PAGE - at < count ? CS_PAGE - at : count;
    if (p >= c->npages) cs_fail(cinfo);
    const uint8_t *src = b;
    if (at || take < CS_PAGE) { // (part of a page: the rest of it as it was)
      if (c->pg[p]) cs_unpack(c->pg[p], c->len[p], c->tmp); else memset(c->tmp, 0, CS_PAGE);
      memcpy(c->tmp + at, b, (size_t)take);
      src = c->tmp;
    }
    size_t n = cs_pack(src, c->out);
    const uint8_t *keep = n == CS_PAGE ? src : c->out;
    if (!c->pg[p] || c->len[p] != n) {
      uint8_t *np = realloc(c->pg[p], n ? n : 1);
      if (!np) cs_fail(cinfo);
      c->held += n - (c->pg[p] ? c->len[p] : 0);
      c->pg[p] = np;
    }
    memcpy(c->pg[p], keep, n);
    c->len[p] = (uint16_t)n;
    if (c->held > cs_peak) cs_peak = c->held;
    b += take; off += take; count -= take;
  }
}
static void cs_close(j_common_ptr cinfo, struct backing_store_struct *info) {
  (void)cinfo;
  Cstore *c = info->store;
  if (!c) return;
  for (long k = 0; k < c->npages; k++) free(c->pg[k]);
  free(c->pg); free(c->len); free(c);
  info->store = NULL;
}
void jpeg_open_backing_store(j_common_ptr cinfo, struct backing_store_struct *info, long total_bytes_needed) {
  Cstore *c = calloc(1, sizeof *c);
  if (!c) cs_fail(cinfo);
  c->npages = (total_bytes_needed + CS_PAGE - 1) / CS_PAGE;
  c->pg = calloc((size_t)c->npages, sizeof *c->pg);
  c->len = calloc((size_t)c->npages, sizeof *c->len);
  if (!c->pg || !c->len) { free(c->pg); free(c->len); free(c); cs_fail(cinfo); }
  info->store = c;
  info->read_backing_store = cs_read;
  info->write_backing_store = cs_write;
  info->close_backing_store = cs_close;
}
// (a progressive JPEG's budget: its window of rows in memory, the rest compressed)
#define JPEG_PROG_BUDGET (1l << 20)
size_t lv_jpeg_store_peak(void) { size_t p = cs_peak; cs_peak = 0; return p; }

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
  if (c.progressive_mode) c.mem->max_memory_to_use = JPEG_PROG_BUDGET;
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

// (PNG: one pass, a row at a time, a band handed on as it fills)
static int png_bands(const uint8_t *d, size_t n, int w, int h, int band_h, int max_w, int (*cb)(void *, int, LvPixels *), void *ctx) {
  png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!p) return -1;
  png_infop info = png_create_info_struct(p);
  uint8_t *volatile buf = NULL;
  volatile int done = 0;
  struct pngsrc src = {d, n, 0};
  if (!info || setjmp(png_jmpbuf(p))) { png_destroy_read_struct(&p, info ? &info : NULL, NULL); free(buf); return done ? done : -1; }
  png_set_read_fn(p, &src, png_from_mem);
  png_read_info(p, info);
  if (png_get_interlace_type(p, info) != PNG_INTERLACE_NONE) png_error(p, "interlaced");
  png_set_expand(p); png_set_strip_16(p); png_set_gray_to_rgb(p); png_set_filler(p, 0xFF, PNG_FILLER_AFTER);
  png_read_update_info(p, info);
  for (int k = 0; k * band_h < h; k++) {
    int y0 = k * band_h, dh = y0 + band_h > h ? h - y0 : band_h;
    if (!(buf = malloc((size_t)w * dh * 4))) png_error(p, "memory");
    for (int y = 0; y < dh; y++) png_read_row(p, buf + (size_t)y * w * 4, NULL);
    int ow, oh;
    fit(w, dh, max_w, 1 << 20, &ow, &oh);
    LvPixels px;
    uint8_t *b = buf;
    buf = NULL;
    if (band_shrink(b, w, dh, ow, oh, &px) != 0) break;
    done = k + 1;
    if (cb(ctx, k, &px)) break;
  }
  png_destroy_read_struct(&p, &info, NULL);
  return done ? done : -1;
}
// A tall picture cut into bands of band_h of its rows, top to bottom, each fitted to max_w and given
// to cb (which owns its pixels; non-zero stops). Never the whole strip in memory: a JPEG is read
// once, a row at a time (a progressive one's coefficients mostly compressed, above), a PNG too, a
// WebP a few bands per pass. Returns how many bands, or -1.
int lv_decode_bands(const uint8_t *d, size_t n, int band_h, int max_w, int (*cb)(void *ctx, int k, LvPixels *px), void *ctx) {
  if (band_h < 16) return -1;
  int w = 0, h = 0;
  if (!lv_image_size(d, n, &w, &h) || w < 1 || h < 1) return -1;
  int nb = (h + band_h - 1) / band_h;
  if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) {
    // (WebP: a few bands at a time, cropped by the decoder, at most BAND_BUDGET of pixels each pass:
    //  each pass decodes from the top again (that's how VP8 works) but never holds the whole strip)
    const size_t BAND_BUDGET = 6u << 20;
    int ow, oh;
    fit(w, band_h, max_w, 1 << 20, &ow, &oh);
    int per = (int)(BAND_BUDGET / ((size_t)ow * oh * 4));
    if (per < 1) per = 1;
    int done = 0;
    for (int k = 0; k < nb; k += per) {
      int y0 = k * band_h, y1 = (k + per) * band_h > h ? h : (k + per) * band_h;
      LvPixels chunk;
      if (webp_rows(d, n, y0, y1, max_w, 1 << 20, &chunk) != 0) return done ? done : -1;
      // (cut at the bands' own rows, scaled as the chunk was)
      for (int b = k; b < k + per && b < nb; b++) {
        int r0 = (int)((long long)(b * band_h - y0) * chunk.h / (y1 - y0));
        int r1 = (int)((long long)((b + 1) * band_h > h ? h - y0 : (b + 1) * band_h - y0) * chunk.h / (y1 - y0));
        if (r1 <= r0) continue;
        LvPixels px = {malloc((size_t)chunk.w * (r1 - r0) * 4), chunk.w, r1 - r0};
        if (!px.rgba) { free(chunk.rgba); return done ? done : -1; }
        memcpy(px.rgba, chunk.rgba + (size_t)r0 * chunk.w * 4, (size_t)chunk.w * (r1 - r0) * 4);
        done = b + 1;
        if (cb(ctx, b, &px)) { free(chunk.rgba); return done; }
      }
      free(chunk.rgba);
    }
    return done ? done : -1;
  }
  if (n >= 8 && !memcmp(d, "\x89PNG", 4)) return png_bands(d, n, w, h, band_h, max_w, cb, ctx);
  if (!(n >= 3 && d[0] == 0xFF && d[1] == 0xD8)) return -1;
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
  if (c.progressive_mode) c.mem->max_memory_to_use = JPEG_PROG_BUDGET;
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

// A big picture at most max_w x max_h without its full size ever in memory: a JPEG of more than ~3
// million pixels is read in bands of rows, each shrunk into its place in the result (a 1200 x 6000
// page was 29 MB decoded whole first); anything else as lv_decode does.
typedef struct { LvPixels *out; int y; } LowMem;
static int lowmem_band(void *ctx, int k, LvPixels *px) {
  (void)k;
  LowMem *m = ctx;
  int rows = px->h;
  if (m->y + rows > m->out->h) rows = m->out->h - m->y;
  int w = px->w < m->out->w ? px->w : m->out->w;
  for (int y = 0; y < rows; y++) memcpy(m->out->rgba + (size_t)(m->y + y) * m->out->w * 4, px->rgba + (size_t)y * px->w * 4, (size_t)w * 4);
  m->y += rows > 0 ? rows : 0;
  free(px->rgba);
  return m->y >= m->out->h;
}
int lv_decode_lowmem(const uint8_t *d, size_t n, int max_w, int max_h, LvPixels *out) {
  memset(out, 0, sizeof *out);
  int w = 0, h = 0;
  if (!(n >= 3 && d[0] == 0xFF && d[1] == 0xD8) || !lv_image_size(d, n, &w, &h) || (long long)w * h <= 3000000) return lv_decode(d, n, max_w, max_h, out);
  int ow, oh;
  fit(w, h, max_w, max_h, &ow, &oh);
  out->rgba = calloc((size_t)ow * oh, 4);
  if (!out->rgba) return -1;
  out->w = ow; out->h = oh;
  LowMem m = {out, 0};
  // (bands of about 256 rows of the result: each shrunk to the result's width, so to its share of rows)
  int band = (int)((long long)256 * h / oh);
  if (band < 16) band = 16;
  if (lv_decode_bands(d, n, band, ow, lowmem_band, &m) < 0 || m.y < oh / 2) { free(out->rgba); memset(out, 0, sizeof *out); return lv_decode(d, n, max_w, max_h, out); }
  for (int y = m.y; y < oh && y > 0; y++) memcpy(out->rgba + (size_t)y * ow * 4, out->rgba + (size_t)(m.y - 1) * ow * 4, (size_t)ow * 4); // (a row short at the end: the last one again)
  return 0;
}
