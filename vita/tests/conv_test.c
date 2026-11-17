// The conversion services' requests and the readers of their answers (src/conv.c).
#include "../src/conv.h"
#include "../src/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static char *slurp(const char *p, size_t *n) {
  FILE *f = fopen(p, "rb");
  if (!f) { printf("missing %s\n", p); exit(1); }
  fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
  char *b = malloc(l + 1); *n = fread(b, 1, l, f); b[*n] = 0; fclose(f); return b;
}
int main(void) {
  const char *src = "https://cdn.donmai.us/original/ab/cd/x.mp4?a=\"b\"";
  for (int s = 0; s < CV_N; s++) {
    char url[300], body[4000], hdr[3][300];
    lv_conv_start(s, "KEY", src, url, sizeof url, body, sizeof body, hdr);
    Json j;
    CHECK(json_parse(&j, body, strlen(body)) == 0, "%s: the body is JSON: %s", lv_conv_name(s), body);
    json_free(&j);
    CHECK(strstr(body, "x.mp4?a=\\\"b\\\"") != NULL, "%s: the address escaped", lv_conv_name(s));
    CHECK(strstr(hdr[0], "KEY") && !strncmp(url, "https://", 8), "%s: key and address", lv_conv_name(s));
    CHECK(strstr(body, "854") && strstr(body, "480"), "%s: 480p asked for", lv_conv_name(s));
  }
  size_t n;
  char id[100], out[600];
  char *t = slurp("tests/conv/cloudconvert_start.json", &n);
  CHECK(lv_conv_parse_start(CV_CLOUDCONVERT, t, n, id, sizeof id) == 0 && !strcmp(id, "6559c281-ed85-4728-80db-414561c631e9"), "cc id %s", id);
  CHECK(lv_conv_parse_poll(CV_CLOUDCONVERT, t, n, out, sizeof out) == 0, "cc waiting");
  free(t);
  t = slurp("tests/conv/cloudconvert_done.json", &n);
  CHECK(lv_conv_parse_poll(CV_CLOUDCONVERT, t, n, out, sizeof out) == 1 && strstr(out, "storage.cloudconvert.com/tasks/x/a.mp4"), "cc done %s", out);
  free(t);
  t = slurp("tests/conv/freeconvert_done.json", &n);
  CHECK(lv_conv_parse_start(CV_FREECONVERT, t, n, id, sizeof id) == 0 && !strcmp(id, "5ff3e22059136b0023324b1f"), "fc id");
  CHECK(lv_conv_parse_poll(CV_FREECONVERT, t, n, out, sizeof out) == 1 && !strcmp(out, "https://s3.freeconvert.com/task/t3/out.mp4"), "fc done %s", out);
  free(t);
  t = slurp("tests/conv/rendi_done.json", &n);
  CHECK(lv_conv_parse_start(CV_RENDI, t, n, id, sizeof id) == 0 && !strcmp(id, "089dd36c-723c-4a0a-b68a-8e8cbcc1afd2"), "rendi id");
  CHECK(lv_conv_parse_poll(CV_RENDI, t, n, out, sizeof out) == 1 && strstr(out, "vita480.mp4"), "rendi done %s", out);
  free(t);
  const char *failed = "{\"data\":{\"status\":\"error\",\"tasks\":[]}}";
  CHECK(lv_conv_parse_poll(CV_CLOUDCONVERT, failed, strlen(failed), out, sizeof out) == -1, "cc failed");
  const char *rf = "{\"status\":\"FAILED\"}";
  CHECK(lv_conv_parse_poll(CV_RENDI, rf, strlen(rf), out, sizeof out) == -1, "rendi failed");
  printf(fails ? "conv: %d failed\n" : "conv: all passed\n", fails);
  return fails != 0;
}
