// The Browser's downloads: libcurl over the Vita's network, with HTTPS checked against Mozilla's
// list of authorities (app0:cacert.pem), compressed answers accepted and connections kept open
// between requests (each thread keeps its own handle).
#pragma once
#include <stddef.h>
#include <stdint.h>

int lv_net_init(void); // 0 when the network is up
int lv_net_online(void); // 1 when connected (Wi-Fi)
typedef struct LvHttp LvHttp;
LvHttp *lv_http_new(void);
void lv_http_free(LvHttp *h);
// GET into memory (malloc'd, NUL-terminated). Returns the HTTP status (200...), or -1 when nothing
// came back; *err gets a short reason. Stops at max bytes, or when *cancel turns non-zero;
// *progress (0..1) follows the download when the size is known.
// The same into a file (big videos never have to fit in memory); *n gets its size.
int lv_http_get_file(LvHttp *h, const char *url, const char *path, size_t *n, size_t max, volatile int *cancel,
                     volatile float *progress, const char **err);
// After the Vita wakes up: every handle drops its connections before its next request.
void lv_net_reset(void);
int lv_http_get(LvHttp *h, const char *url, uint8_t **out, size_t *n, size_t max, volatile int *cancel,
                volatile float *progress, const char **err);

// A request with a method, headers and a body (Google Drive's API). The body is in memory, or a piece
// of a file (uploads in parts); the answer's Location header can be kept (resumable uploads).
typedef struct {
  const char *method;         // NULL: GET, or POST with a body
  const char *const *headers; // "Name: value" ..., NULL-terminated (or NULL)
  const void *body;
  size_t body_len;
  const char *body_path;      // (or the body from this file: body_n bytes from body_off)
  uint64_t body_off, body_n;
  char *location;             // (the Location header goes here, if any)
  size_t location_cap;
} LvReq;
int lv_http_req(LvHttp *h, const char *url, const LvReq *rq, uint8_t **out, size_t *n, size_t max, volatile int *cancel, const char **err);
// The answer into a file (removed unless it's a 2xx).
int lv_http_req_file(LvHttp *h, const char *url, const LvReq *rq, const char *path, size_t *n, volatile int *cancel,
                     volatile float *progress, const char **err);
