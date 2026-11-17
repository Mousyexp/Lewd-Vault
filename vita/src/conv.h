// Making a video 480p somewhere else, for an offline copy: the Vita can't re-encode video, so the
// video's address goes to a conversion service (one of yours, by its API key), which fetches it,
// makes an H.264/AAC MP4 of at most 854 x 480, and gives back an address to download it from.
// Several services, tried in turn: one that fails or has run out of free conversions is skipped.
// Portable C (tests/conv_test.c): the requests and the readers of the answers.
#pragma once
#include <stddef.h>

enum { CV_CLOUDCONVERT, CV_FREECONVERT, CV_RENDI, CV_N };
const char *lv_conv_name(int svc);
const char *lv_conv_site(int svc); // (where to get a key)

// The request that starts a conversion of src: its address, its JSON body, and its headers (an array
// of "Name: value" strings, NULL-ended, in hdr_buf).
void lv_conv_start(int svc, const char *key, const char *src, char *url, size_t ucap, char *body, size_t bcap,
                   char hdr[3][300]);
// Its answer: 0 and the job's id, or -1.
int lv_conv_parse_start(int svc, const char *json, size_t len, char *id, size_t cap);
// Asking how it's going: the address (the same headers).
void lv_conv_poll_url(int svc, const char *id, char *url, size_t cap);
// The answer: 1 done (the result's address in out), 0 still working, -1 failed.
int lv_conv_parse_poll(int svc, const char *json, size_t len, char *out, size_t cap);
