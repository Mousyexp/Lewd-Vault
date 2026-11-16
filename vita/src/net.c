#include "net.h"
#include <curl/curl.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UA "LewdVault/1.0 (PS Vita)"

// curl asks inet_pton whether a host name is an IP address, and takes any non-zero answer for "yes";
// the Vita's C library answers -1 for a name (the system call's error), so curl took every site for
// an address and sent no server name (SNI) in its TLS hello, which sites behind Cloudflare and most
// CDNs refuse. This one answers as POSIX says (1: an address, 0: not one), and being in the app it
// is the one the linker uses. (The Vita has no IPv6: no name is an IPv6 address.)
int inet_pton(int af, const char *src, void *dst) {
  if (af != 2 /* AF_INET */) return 0;
  unsigned char out[4];
  int parts = 0;
  const char *p = src;
  while (parts < 4) {
    if (*p < '0' || *p > '9') return 0;
    unsigned v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p++ - '0'); if (++digits > 3 || v > 255) return 0; }
    out[parts++] = (unsigned char)v;
    if (parts < 4) { if (*p != '.') return 0; p++; }
  }
  if (*p) return 0;
  memcpy(dst, out, 4);
  return 1;
}
static int up;
static char proxy[256], cafile[256] = "app0:cacert.pem";

int lv_net_init(void) {
  if (up) return 0;
  sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
  static char mem[1024 * 1024];
  SceNetInitParam p = {mem, sizeof mem, 0};
  int r = sceNetInit(&p);
  if (r < 0 && r != (int)0x80410201) return -1; // (already: fine)
  sceNetCtlInit();
  curl_global_init(CURL_GLOBAL_ALL);
#ifdef LV_DEBUG
  // (debug builds in an emulator behind a proxy: its address, and its authority for HTTPS)
  FILE *f = fopen("ux0:data/LWDV00001/debug_proxy.txt", "r");
  if (f) {
    if (fgets(proxy, sizeof proxy, f)) proxy[strcspn(proxy, "\r\n")] = 0;
    fclose(f);
    snprintf(cafile, sizeof cafile, "ux0:data/LWDV00001/debug_ca.pem");
  }
#endif
  up = 1;
  return 0;
}

int lv_net_online(void) {
  int state = 0;
  if (sceNetCtlInetGetState(&state) < 0) return proxy[0] != 0;
  return state == SCE_NETCTL_STATE_CONNECTED || proxy[0];
}

struct LvHttp { CURL *c; };
static volatile int no_compress;
LvHttp *lv_http_new(void) {
  LvHttp *h = calloc(1, sizeof *h);
  if (!h) return NULL;
  h->c = curl_easy_init();
  if (!h->c) { free(h); return NULL; }
  return h;
}
void lv_http_free(LvHttp *h) {
  if (!h) return;
  curl_easy_cleanup(h->c);
  free(h);
}

typedef struct {
  uint8_t *b;
  size_t n, cap, max;
  volatile int *cancel;
  volatile float *progress;
  int too_big;
} Sink;

static size_t on_data(char *p, size_t sz, size_t nm, void *u) {
  Sink *s = u;
  size_t n = sz * nm;
  if (s->cancel && *s->cancel) return 0;
  if (s->n + n > s->max) { s->too_big = 1; return 0; }
  if (s->n + n + 1 > s->cap) {
    size_t cap = s->cap ? s->cap : 64 * 1024;
    while (cap < s->n + n + 1) cap *= 2;
    uint8_t *b = realloc(s->b, cap);
    if (!b) return 0;
    s->b = b;
    s->cap = cap;
  }
  memcpy(s->b + s->n, p, n);
  s->n += n;
  return n;
}
static int on_progress(void *u, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un) {
  Sink *s = u;
  if (s->progress && total > 0) *s->progress = (float)now / (float)total;
  return s->cancel && *s->cancel;
}

int lv_http_get(LvHttp *h, const char *url, uint8_t **out, size_t *n, size_t max, volatile int *cancel,
                volatile float *progress, const char **err) {
  static __thread char why[CURL_ERROR_SIZE];
  *out = NULL;
  *n = 0;
  why[0] = 0;
  if (err) *err = why;
  if (!up || !h) { snprintf(why, sizeof why, "No network"); return -1; }
  Sink s = {NULL, 0, 0, max, cancel, progress, 0};
  CURL *c = h->c;
  curl_easy_reset(c); // (the connection itself stays open for the next request)
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https,http");
  // (the JSON comes compressed: a fraction of its size; if unpacking ever fails, everything after
  // comes plain, as Vita3K's zlib needs)
  if (!no_compress) curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "gzip");
  curl_easy_setopt(c, CURLOPT_CAINFO, cafile);
  curl_easy_setopt(c, CURLOPT_CA_CACHE_TIMEOUT, 24L * 3600);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 200L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
  curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 64L * 1024);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, &s);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_ERRORBUFFER, why);
  if (proxy[0]) curl_easy_setopt(c, CURLOPT_PROXY, proxy);
  CURLcode r = curl_easy_perform(c);
  if (r == CURLE_BAD_CONTENT_ENCODING && !no_compress) {
    no_compress = 1;
    free(s.b);
    return lv_http_get(h, url, out, n, max, cancel, progress, err);
  }
  long status = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  if (r != CURLE_OK) {
    if (s.too_big) snprintf(why, sizeof why, "Too big for the Vita");
    else if (cancel && *cancel) snprintf(why, sizeof why, "Cancelled");
    else if (!why[0]) snprintf(why, sizeof why, "%s", curl_easy_strerror(r));
    free(s.b);
    return -1;
  }
  if (!s.b) { s.b = malloc(1); s.cap = 1; }
  s.b[s.n] = 0;
  *out = s.b;
  *n = s.n;
  if (status >= 400) snprintf(why, sizeof why, "The site answered %ld", status);
  return (int)status;
}
