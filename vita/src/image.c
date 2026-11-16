#include "image.h"
#include <png.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>
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
