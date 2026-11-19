#include <time.h>
#include "net.h"
#include <curl/curl.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

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
static char proxy[256], cafile[256] = "app0:certs.bin";

#include <pthread.h>
#ifdef LV_DEBUG
#define LOG(...) do { char lb_[256]; int ln_ = snprintf(lb_, sizeof lb_, __VA_ARGS__); SceUID lf_ = sceIoOpen("ux0:data/LWDV00001/log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666); if (lf_ >= 0) { sceIoWrite(lf_, lb_, ln_ > 255 ? 255 : ln_); sceIoClose(lf_); } } while (0)
#else
#define LOG(...) do { } while (0)
#endif
static pthread_mutex_t init_mx = PTHREAD_MUTEX_INITIALIZER;
// The certificate list: read once with the Vita's own file calls and kept as bytes (each one's DER
// and the hash of its subject), never as OpenSSL certificates: on a real Vita each certificate
// OpenSSL holds takes a lock, each lock is one of the system's objects, and a process has only so
// many. Holding the whole list (by path, as a blob, or read here and kept) used them all up: the list
// failed part way ("asn1_do_lock: malloc failure", "error adding trust anchors ... 77") and then
// nothing else could have a lock either ("insufficient randomness"). Vita3K has no such limit.
// So each connection's trust store gets a lookup instead (CURLOPT_SSL_CTX_FUNCTION): when a site's
// chain asks for an issuer, the one certificate with that subject is made then (usually one or two
// a connection), held by that connection's store and freed with it. What happened is kept
// (lv_net_ca_status: the log and the test kit's diagnostics).
typedef struct { unsigned long hash; unsigned off, len; } CaEnt;
static uint8_t *ca_der;
static CaEnt *ca_ent;
static int ca_n, ca_bad, ca_tried;
static volatile int ca_made, ca_found, ca_missed; // (certificates made on demand, issuers found, not found)
static char ca_status[200];
static X509_LOOKUP_METHOD *ca_meth;
static pthread_mutex_t ca_mx = PTHREAD_MUTEX_INITIALIZER;
static int b64v(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
// (the lookup: the issuer a chain asks for, made from its bytes and put in the asking store)
static int ca_by_subject(X509_LOOKUP *lu, X509_LOOKUP_TYPE type, X509_NAME *name, X509_OBJECT *ret) {
  if (type != X509_LU_X509 || !name) return 0;
  unsigned long h = X509_NAME_hash(name);
  X509_STORE *st = X509_LOOKUP_get_store(lu);
  int got = 0;
  // (every one with that name goes into the store: two can share a name (a root renewed, an
  //  intermediate cross-signed), and OpenSSL then tries each against the signature)
  for (int k = 0; k < ca_n && st; k++) {
    if (ca_ent[k].hash != h) continue;
    const unsigned char *dp = ca_der + ca_ent[k].off;
    X509 *x = d2i_X509(NULL, &dp, (long)ca_ent[k].len);
    if (!x) continue;
    ca_made++;
    if (X509_NAME_cmp(X509_get_subject_name(x), name) != 0 || !X509_STORE_add_cert(st, x)) { X509_free(x); ERR_clear_error(); continue; }
    // (references: 1 ours, 1 the store's; the first one found also goes in ret, whose reference the
    //  caller keeps and never gives back (as OpenSSL's own lookups arrange it): so that one's let go too)
    if (!got) {
      if (X509_OBJECT_set1_X509(ret, x)) { got = 1; X509_free(x); }
    }
    X509_free(x);
  }
  if (got) ca_found++; else ca_missed++;
  ERR_clear_error();
  return got;
}
static int ca_load(void) {
  pthread_mutex_lock(&ca_mx);
  if (!ca_tried) {
    ca_tried = 1;
    char *pem = NULL;
    size_t got = 0;
    SceUID fd = sceIoOpen(cafile, SCE_O_RDONLY, 0);
    if (fd >= 0) {
      SceOff n = sceIoLseek(fd, 0, SCE_SEEK_END);
      sceIoLseek(fd, 0, SCE_SEEK_SET);
      pem = n > 0 && n < (8 << 20) ? malloc((size_t)n + 1) : NULL;
      while (pem && got < (size_t)n) { int r = sceIoRead(fd, pem + got, (SceSize)((size_t)n - got)); if (r <= 0) break; got += (size_t)r; }
      sceIoClose(fd);
      if (pem) pem[got] = 0;
    }
    if (!pem || !got) snprintf(ca_status, sizeof ca_status, "certificates: %s couldn't be read (%08x)", cafile, (unsigned)fd);
    else if (got > 5 && !memcmp(pem, "LVCA1", 5)) {
      // certs.bin (tools/make-certs.py): kind, subject hash, length, DER; nothing to parse, the bytes kept
      int cap = 0, roots = 0;
      for (size_t at = 5; at + 9 <= got; cap++) { uint32_t len; memcpy(&len, pem + at + 5, 4); at += 9 + len; }
      ca_ent = calloc(cap + 1, sizeof *ca_ent);
      if (ca_ent) {
        ca_der = (uint8_t *)pem; pem = NULL; // (the file is the store of bytes)
        for (size_t at = 5; at + 9 <= got && ca_n < cap;) {
          uint32_t h, len;
          memcpy(&h, ca_der + at + 1, 4); memcpy(&len, ca_der + at + 5, 4);
          if (at + 9 + len > got) break;
          roots += ca_der[at] == 0;
          ca_ent[ca_n].hash = h; ca_ent[ca_n].off = (unsigned)(at + 9); ca_ent[ca_n].len = len;
          ca_n++;
          at += 9 + len;
        }
        ca_meth = X509_LOOKUP_meth_new("lewd vault certificates");
        if (ca_meth) X509_LOOKUP_meth_set_get_by_subject(ca_meth, ca_by_subject);
      }
      snprintf(ca_status, sizeof ca_status, "certificates: %d roots, %d intermediates", roots, ca_n - roots);
    } else {
      int cap = 0;
      for (char *p = pem; (p = strstr(p, "-----BEGIN CERTIFICATE-----")); p += 27) cap++;
      ca_ent = calloc(cap + 1, sizeof *ca_ent);
      ca_der = malloc(got); // (DER is smaller than its base64)
      size_t used = 0;
      unsigned long first_err = 0;
      int first_bad = -1, k = 0;
      for (char *p = pem; ca_ent && ca_der && ca_n < cap && (p = strstr(p, "-----BEGIN CERTIFICATE-----")); k++) {
        p += 27;
        char *e = strstr(p, "-----END CERTIFICATE-----");
        if (!e) break;
        // (base64 to DER, by hand)
        uint8_t *der = ca_der + used;
        size_t dn = 0;
        unsigned acc = 0;
        int bits = 0;
        for (char *q = p; q < e && used + dn < got; q++) {
          int v = b64v((unsigned char)*q);
          if (v < 0) continue;
          acc = (acc << 6) | (unsigned)v;
          bits += 6;
          if (bits >= 8) { bits -= 8; der[dn++] = (uint8_t)(acc >> bits); }
        }
        p = e + 25;
        // (made once, for its subject's hash, and let go at once: one lock at a time)
        const unsigned char *dp = der;
        ERR_clear_error();
        X509 *x = d2i_X509(NULL, &dp, (long)dn);
        if (!x) { ca_bad++; if (first_bad < 0) { first_bad = k; first_err = ERR_peek_error(); } continue; }
        ca_ent[ca_n].hash = X509_NAME_hash(X509_get_subject_name(x));
        X509_free(x);
        ca_ent[ca_n].off = (unsigned)used;
        ca_ent[ca_n].len = (unsigned)dn;
        ca_n++;
        used += dn;
      }
      if (ca_n) {
        ca_meth = X509_LOOKUP_meth_new("lewd vault certificates");
        if (ca_meth) X509_LOOKUP_meth_set_get_by_subject(ca_meth, ca_by_subject);
      }
      if (first_bad >= 0) {
        char eb[120];
        ERR_error_string_n(first_err, eb, sizeof eb);
        snprintf(ca_status, sizeof ca_status, "certificates: %d read, %d not (the first, #%d: %s)", ca_n, ca_bad, first_bad, eb);
      } else snprintf(ca_status, sizeof ca_status, "certificates: all %d read%s", ca_n, ca_meth ? "" : " (no lookup: out of memory)");
    }
    free(pem);
    LOG("%s\n", ca_status);
  }
  pthread_mutex_unlock(&ca_mx);
  return ca_n > 0 && ca_meth ? 0 : -1;
}
const char *lv_net_ca_status(void) {
  ca_load(); // (read now if nothing has asked yet)
  static char full[300];
  snprintf(full, sizeof full, "%s; used: %d found, %d not  ·  %s", ca_status[0] ? ca_status : "certificates: not read", ca_found, ca_missed,
           OpenSSL_version(OPENSSL_VERSION));
  return full;
}
// (each new connection's trust store: the lookup above, nothing made until it's asked)
static CURLcode ca_into_ctx(CURL *c, void *ssl_ctx, void *arg) {
  (void)c; (void)arg;
  X509_STORE *st = SSL_CTX_get_cert_store((SSL_CTX *)ssl_ctx);
  if (!st || !ca_meth || !X509_STORE_add_lookup(st, ca_meth)) return CURLE_SSL_CACERT_BADFILE;
  ERR_clear_error();
  return CURLE_OK;
}
static int net_init_locked(void);
static volatile int base_up;
// The Vita's own network, quickly (its module, sceNetInit, sceNetCtlInit): enough for the online
// light and the Wi-Fi page's server. Once.
int lv_net_base(void) {
  if (base_up) return 0;
  pthread_mutex_lock(&init_mx);
  if (!base_up) {
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    static char mem[1024 * 1024]; // (sceNetInit's memory: it keeps using it)
    SceNetInitParam p = {mem, sizeof mem, 0};
    int r = sceNetInit(&p);
    if (r >= 0 || r == (int)0x80410201) { sceNetCtlInit(); base_up = 1; } // (0x80410201: already)
#ifdef LV_DEBUG
    // (debug builds in an emulator behind a proxy: its address, and its authority for HTTPS)
    FILE *f = fopen("ux0:data/LWDV00001/debug_proxy.txt", "r");
    if (f) {
      if (fgets(proxy, sizeof proxy, f)) proxy[strcspn(proxy, "\r\n")] = 0;
      fclose(f);
      snprintf(cafile, sizeof cafile, "ux0:data/LWDV00001/debug_ca.pem");
    }
#endif
  }
  pthread_mutex_unlock(&init_mx);
  return base_up ? 0 : -1;
}
// All of it, with libcurl and OpenSSL (seconds on a cold start): only ever on the threads that fetch,
// the first time one does (perform), never on the drawing thread, and never before anything's fetched
int lv_net_init(void) {
  if (up) return 0;
  if (lv_net_base() < 0) return -1;
  pthread_mutex_lock(&init_mx);
  int r = up ? 0 : net_init_locked();
  pthread_mutex_unlock(&init_mx);
  return r;
}
static int net_init_locked(void) {
  curl_global_init(CURL_GLOBAL_ALL);
  up = 1;
  return 0;
}

// The base started on a thread of its own (loading the module and starting it takes a few hundred ms):
// asked for at the app's start, so the drawing thread never waits for it
static void *net_warm_main(void *arg) { (void)arg; lv_net_base(); return NULL; }
void lv_net_warm(void) {
  static volatile int started;
  if (base_up || started) return;
  started = 1;
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 64 * 1024);
  if (pthread_create(&t, &at, net_warm_main, NULL) == 0) pthread_detach(t);
  else started = 0;
}
int lv_net_online(void) {
  int state = 0;
  if (!base_up) { lv_net_warm(); return 0; } // (not up yet: offline for now, never a wait)
  // (every request asks: the answer kept for a second, not a system call for each one)
  static volatile uint64_t at;
  static volatile int last;
  uint64_t now = sceKernelGetProcessTimeWide();
  if (at && now - at < 1000000) return last;
  if (sceNetCtlInetGetState(&state) < 0) { last = proxy[0] != 0; at = now; return last; }
  last = state == SCE_NETCTL_STATE_CONNECTED || proxy[0];
  at = now;
  return last;
}

struct LvHttp { CURL *c; int gen, open; }; // (open: a request made since the connection was last let go)
static volatile int net_gen;
void lv_net_reset(void) { net_gen++; }
static volatile int no_compress;
LvHttp *lv_http_new(void) {
  lv_net_init(); // (libcurl ready before its first handle: on the worker that asks)
  LvHttp *h = calloc(1, sizeof *h);
  if (!h) return NULL;
  h->c = curl_easy_init();
  h->gen = net_gen;
  if (!h->c) { free(h); return NULL; }
  return h;
}
int lv_http_has_conn(const LvHttp *h) { return h && h->open; }
void lv_http_drop(LvHttp *h) {
  if (!h || !h->open) return;
  curl_easy_cleanup(h->c); // (its connection closed: the socket and its TLS state given back)
  h->c = curl_easy_init();
  h->open = 0;
}
void lv_http_free(LvHttp *h) {
  if (!h) return;
  curl_easy_cleanup(h->c);
  free(h);
}

typedef struct {
  uint8_t *b;
  FILE *f; // (to a file instead of memory)
  size_t n, cap, max;
  volatile int *cancel;
  volatile float *progress;
  int too_big;
  volatile int64_t *have, *total; // (a file played as it comes: what's on the card, and the whole)
  size_t flushed;                  // (written through to the card up to here)
  CURL *curl;                      // (its request: the size it said, for the first allocation)
} Sink;

static size_t on_data(char *p, size_t sz, size_t nm, void *u) {
  Sink *s = u;
  size_t n = sz * nm;
  if (s->cancel && *s->cancel) return 0;
  if (s->n + n > s->max) { s->too_big = 1; return 0; }
  if (s->f) {
    if (fwrite(p, 1, n, s->f) != n) return 0;
    s->n += n;
    // (on the card before it's said to be; a quarter of a megabyte at a time: the card is slow at
    //  many small writes)
    if (s->have && (s->n - s->flushed >= 256 * 1024 || s->n < 256 * 1024)) { fflush(s->f); s->flushed = s->n; *s->have = (int64_t)s->n; }
    return n;
  }
  if (s->n + n + 1 > s->cap) {
    size_t cap = s->cap ? s->cap : 64 * 1024;
    // (the first time, room for all of it when the server said how much: one allocation, not a
    //  doubling and a copy at each)
    if (!s->cap && s->curl) {
      curl_off_t len = -1;
      if (curl_easy_getinfo(s->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len) == CURLE_OK && len > 0 && (size_t)len < s->max) cap = (size_t)len + 1;
    }
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
// (the bytes behind each progress pointer: a transfer's size and how far, for those who show it)
static struct { volatile float *p; volatile int64_t now, total; } prog_bytes[4];
void lv_http_progress_bytes(volatile float *p, int64_t *now, int64_t *total) {
  *now = *total = 0;
  for (int k = 0; k < 4; k++) if (prog_bytes[k].p == p) { *now = prog_bytes[k].now; *total = prog_bytes[k].total; return; }
}
static int on_progress(void *u, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un) {
  Sink *s = u;
  if (s->progress && total > 0) *s->progress = (float)now / (float)total;
  if (s->progress) {
    int k = 0;
    while (k < 4 && prog_bytes[k].p && prog_bytes[k].p != s->progress) k++;
    if (k == 4) k = 0;
    prog_bytes[k].p = s->progress; prog_bytes[k].now = now; prog_bytes[k].total = total;
  }
  if (s->total && total > 0) *s->total = (int64_t)total;
  return s->cancel && *s->cancel;
}

static int perform(LvHttp *h, const char *url, Sink *sp, const char **err, const LvReq *rq);
int lv_http_get(LvHttp *h, const char *url, uint8_t **out, size_t *n, size_t max, volatile int *cancel,
                volatile float *progress, const char **err) {
  Sink s = {NULL, NULL, 0, 0, max, cancel, progress, 0};
  *out = NULL;
  *n = 0;
  int st = perform(h, url, &s, err, NULL);
  if (st < 0) { free(s.b); return st; }
  if (!s.b) { s.b = malloc(1); if (!s.b) return -1; s.cap = 1; }
  s.b[s.n] = 0;
  *out = s.b;
  *n = s.n;
  return st;
}

int lv_http_get_file(LvHttp *h, const char *url, const char *path, size_t *n, size_t max, volatile int *cancel,
                     volatile float *progress, const char **err) {
  char tmp[600];
  snprintf(tmp, sizeof tmp, "%s.part", path);
  Sink s = {NULL, fopen(tmp, "wb"), 0, 0, max, cancel, progress, 0};
  *n = 0;
  if (!s.f) { if (err) *err = "No room on the memory card"; return -1; }
  int st = perform(h, url, &s, err, NULL);
  int ok = fclose(s.f) == 0 && st == 200;
  if (!ok) { remove(tmp); return st < 0 ? st : st == 200 ? -1 : st; }
  remove(path);
  if (rename(tmp, path) != 0) { remove(tmp); return -1; }
  *n = s.n;
  return st;
}

// Straight into path (no .part: a player reads it as it grows), *have and *total kept up to date.
int lv_http_stream_file(LvHttp *h, const char *url, const char *path, size_t max, volatile int *cancel,
                        volatile float *progress, volatile int64_t *have, volatile int64_t *total, const char **err) {
  Sink s = {NULL, fopen(path, "wb"), 0, 0, max, cancel, progress, 0, have, total};
  if (!s.f) { if (err) *err = "No room on the memory card"; return -1; }
  int st = perform(h, url, &s, err, NULL);
  int ok = fclose(s.f) == 0 && st == 200;
  if (have) *have = (int64_t)s.n;
  if (!ok) return st < 0 ? st : st == 200 ? -1 : st;
  if (total) *total = (int64_t)s.n;
  return st;
}

// ---- requests with a method, headers and a body (Google Drive's API) ----
typedef struct { FILE *f; uint64_t left; volatile int *cancel; } Piece;
static size_t on_read(char *p, size_t sz, size_t nm, void *u) {
  Piece *pc = u;
  if (pc->cancel && *pc->cancel) return CURL_READFUNC_ABORT;
  size_t n = sz * nm;
  if (n > pc->left) n = (size_t)pc->left;
  n = n ? fread(p, 1, n, pc->f) : 0;
  pc->left -= n;
  return n;
}
typedef struct { char *out; size_t cap; } Hdr;
static size_t on_header(char *p, size_t sz, size_t nm, void *u) {
  Hdr *hd = u;
  size_t n = sz * nm;
  if (n > 9 && !strncasecmp(p, "location:", 9) && hd->out && hd->cap) {
    const char *v = p + 9;
    size_t m = n - 9;
    while (m && (*v == ' ' || *v == '\t')) { v++; m--; }
    while (m && (v[m - 1] == '\r' || v[m - 1] == '\n' || v[m - 1] == ' ')) m--;
    if (m >= hd->cap) m = hd->cap - 1;
    memcpy(hd->out, v, m);
    hd->out[m] = 0;
  }
  return n;
}
int lv_http_req(LvHttp *h, const char *url, const LvReq *rq, uint8_t **out, size_t *n, size_t max, volatile int *cancel, const char **err) {
  Sink s = {NULL, NULL, 0, 0, max, cancel, NULL, 0};
  *out = NULL;
  *n = 0;
  int st = perform(h, url, &s, err, rq);
  if (st < 0) { free(s.b); return st; }
  if (!s.b) { s.b = malloc(1); if (!s.b) return -1; s.cap = 1; }
  s.b[s.n] = 0;
  *out = s.b;
  *n = s.n;
  return st;
}
int lv_http_req_file(LvHttp *h, const char *url, const LvReq *rq, const char *path, size_t *n, volatile int *cancel,
                     volatile float *progress, const char **err) {
  Sink s = {NULL, fopen(path, "wb"), 0, 0, (size_t)-1, cancel, progress, 0};
  *n = 0;
  if (!s.f) { if (err) *err = "No room on the memory card"; return -1; }
  int st = perform(h, url, &s, err, rq);
  if (fclose(s.f) != 0 && st >= 200 && st < 300) st = -1;
  if (st < 200 || st >= 300) remove(path);
  *n = s.n;
  return st;
}

static int perform(LvHttp *h, const char *url, Sink *sp, const char **err, const LvReq *rq) {
  static __thread char why[CURL_ERROR_SIZE];
  Sink s0 = *sp;
  why[0] = 0;
  if (err) *err = why;
  if (!up) lv_net_init(); // (the first request: libcurl and OpenSSL started here, on this worker)
  if (!up || !h) { snprintf(why, sizeof why, "No network"); return -1; }
  // (no Wi-Fi: said at once, rather than each request waiting out its connect timeout)
  if (!lv_net_online()) { snprintf(why, sizeof why, "Offline: turn on Wi-Fi in the Vita's settings"); return -1; }
  // (woken up since the last request: a fresh handle, no connections from before)
  if (h->gen != net_gen) {
    curl_easy_cleanup(h->c);
    h->c = curl_easy_init();
    h->gen = net_gen;
    if (!h->c) { snprintf(why, sizeof why, "No network"); return -1; }
  }
  if (!h->c && !(h->c = curl_easy_init())) { snprintf(why, sizeof why, "No network"); return -1; }
  h->open = 1;
  Sink *s = sp;
  CURL *c = h->c;
  s->curl = c;
  curl_easy_reset(c); // (the connection itself stays open for the next request)
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
  curl_easy_setopt(c, CURLOPT_COOKIEFILE, ""); // (cookies kept per connection, in memory: a video page's cookie for its file)
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https,http");
  // (the JSON comes compressed: a fraction of its size; if unpacking ever fails, everything after
  // comes plain, as Vita3K's zlib needs)
  if (!no_compress) curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "gzip");
  if ((ca_tried && ca_meth) || ca_load() == 0) {
    curl_easy_setopt(c, CURLOPT_CAINFO, NULL);
    curl_easy_setopt(c, CURLOPT_CAPATH, NULL);
    curl_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, ca_into_ctx);
  } else curl_easy_setopt(c, CURLOPT_CAINFO, cafile); // (none could be read: OpenSSL tries, and says why)
  curl_easy_setopt(c, CURLOPT_CA_CACHE_TIMEOUT, 0L); // (the store is filled by ca_into_ctx: nothing of curl's to keep)
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 200L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
  curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 64L * 1024);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, s);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, s);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_ERRORBUFFER, why);
  if (proxy[0]) { curl_easy_setopt(c, CURLOPT_PROXY, proxy); curl_easy_setopt(c, CURLOPT_NOPROXY, "127.0.0.1,localhost"); }
  struct curl_slist *hl = NULL;
  Piece piece = {NULL, 0, s->cancel};
  Hdr hd = {NULL, 0};
  if (rq) {
    for (const char *const *x = rq->headers; x && *x; x++) hl = curl_slist_append(hl, *x);
    hl = curl_slist_append(hl, "Expect:"); // (no waiting for "100 Continue")
    if (rq->body_path) {
      piece.f = fopen(rq->body_path, "rb");
      if (!piece.f || fseeko(piece.f, (off_t)rq->body_off, SEEK_SET) != 0) {
        if (piece.f) fclose(piece.f);
        curl_slist_free_all(hl);
        snprintf(why, sizeof why, "The file couldn't be read");
        return -1;
      }
      piece.left = rq->body_n;
      curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
      curl_easy_setopt(c, CURLOPT_READFUNCTION, on_read);
      curl_easy_setopt(c, CURLOPT_READDATA, &piece);
      curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)rq->body_n);
      if (rq->method) curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, rq->method);
    } else if (rq->body) {
      curl_easy_setopt(c, CURLOPT_POSTFIELDS, rq->body);
      curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)rq->body_len);
      if (rq->method && strcmp(rq->method, "POST")) curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, rq->method);
    } else if (rq->method) {
      curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, rq->method);
      if (!strcmp(rq->method, "PUT")) { curl_easy_setopt(c, CURLOPT_UPLOAD, 1L); curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)0); }
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    if (rq->location && rq->location_cap) {
      rq->location[0] = 0;
      hd.out = rq->location;
      hd.cap = rq->location_cap;
      curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
      curl_easy_setopt(c, CURLOPT_HEADERDATA, &hd);
      curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    }
  }
  CURLcode r = curl_easy_perform(c);
  // (a connection that broke before anything came, an old kept-alive one the server had closed or
  //  one that died in a sleep: once more on a new connection, never the same socket. Only requests
  //  that send nothing, and only when nothing was written yet.)
  int fresh_ok = !rq || (!rq->body && !rq->body_path && (!rq->method || !strcmp(rq->method, "GET")));
  if (fresh_ok && s->n == 0 && !(s->cancel && *s->cancel) &&
      (r == CURLE_RECV_ERROR || r == CURLE_SEND_ERROR || r == CURLE_GOT_NOTHING || r == CURLE_SSL_CONNECT_ERROR || r == CURLE_PARTIAL_FILE || r == CURLE_COULDNT_CONNECT)) {
    if (s->f) rewind(s->f);
    curl_easy_setopt(c, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(c, CURLOPT_FORBID_REUSE, 1L);
    why[0] = 0;
    r = curl_easy_perform(c);
    curl_easy_setopt(c, CURLOPT_FRESH_CONNECT, 0L);
    curl_easy_setopt(c, CURLOPT_FORBID_REUSE, 0L);
  }
  if (piece.f) fclose(piece.f);
  curl_slist_free_all(hl);
  if (r == CURLE_BAD_CONTENT_ENCODING && !no_compress) {
    no_compress = 1;
    free(s->b);
    if (s->f) rewind(s->f);
    *s = s0;
    if (s0.f) s->f = s0.f;
    return perform(h, url, s, err, rq);
  }
  long status = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  if (r != CURLE_OK) {
    if (s->too_big) snprintf(why, sizeof why, "Too big for the Vita");
    else if (s->cancel && *s->cancel) snprintf(why, sizeof why, "Cancelled");
    else if (!why[0]) snprintf(why, sizeof why, "%s", curl_easy_strerror(r));
    { // (which address failed, and why: in the log, for the reports; the address cut at its path)
      char host[120];
      const char *hp = strstr(url, "://");
      snprintf(host, sizeof host, "%s", hp ? hp + 3 : url);
      host[strcspn(host, "/?")] = 0;
      LOG("net fail %s: %d %s\n", host, (int)r, why);
    }
    // (a certificate "not yet valid" or "expired" everywhere is usually the Vita's own clock: it's
    //  said so, and the check stays on)
    if ((r == CURLE_PEER_FAILED_VERIFICATION || r == CURLE_SSL_CACERT_BADFILE || r == CURLE_SSL_CERTPROBLEM) &&
        (strcasestr(why, "not yet valid") || strcasestr(why, "expired") || time(NULL) < 1735689600 /* 2025-01-01 */))
      snprintf(why, sizeof why, "The Vita's date or time looks wrong: set it in the Vita's Settings (secure sites check it)");
    return -1;
  }
  if (status >= 400) snprintf(why, sizeof why, "The site answered %ld", status);
  return (int)status;
}
