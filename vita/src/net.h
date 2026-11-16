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
int lv_http_get(LvHttp *h, const char *url, uint8_t **out, size_t *n, size_t max, volatile int *cancel,
                volatile float *progress, const char **err);
