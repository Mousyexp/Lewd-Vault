// The Browser's site readers against answers saved from the real sites (tests/booru/*.json, from
// each kind's all-ages site), and the JSON reader on its own.
#include "../src/booru.h"
#include "../src/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *slurp(const char *p, size_t *n) {
  FILE *f = fopen(p, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long l = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc(l + 1);
  if (fread(b, 1, l, f) != (size_t)l) l = 0;
  b[l] = 0;
  fclose(f);
  *n = l;
  return b;
}

static const LvSite *site(const char *name) {
  for (int k = 0; k < LV_NSITES; k++) if (!strcmp(LV_SITES[k].name, name)) return &LV_SITES[k];
  return NULL;
}

static void check_kind(const char *file, const LvSite *s, int want) {
  char p[256];
  snprintf(p, sizeof p, "tests/booru/%s.json", file);
  size_t n = 0;
  char *t = slurp(p, &n);
  CHECK(t, "%s missing", p);
  if (!t) return;
  int count = 0;
  LvPost *posts = lv_booru_parse(s, t, n, LV_R_ALL, &count);
  CHECK(posts && count == want, "%s: %d posts, not %d", file, count, want);
  for (int i = 0; i < count; i++) {
    LvPost *q = &posts[i];
    CHECK(q->id > 0, "%s: id", file);
    CHECK(!strncmp(q->preview, "https://", 8) && !strncmp(q->sample, "https://", 8) && !strncmp(q->file, "https://", 8), "%s: urls %s | %s | %s", file, q->preview, q->sample, q->file);
    CHECK(q->ext[0], "%s: ext", file);
    CHECK(q->tags[0], "%s: tags", file);
    CHECK(q->rating & LV_R_ALL, "%s: rating", file);
  }
  printf("%-9s %d posts, first: %lld %s %dx%d score %d\n  preview %s\n  sample  %s\n", file, count, posts[0].id, posts[0].ext, posts[0].width, posts[0].height, posts[0].score, posts[0].preview, posts[0].sample);
  // only the ratings asked for
  int none = 0;
  LvPost *x = lv_booru_parse(s, t, n, LV_R_EXPLICIT, &none);
  CHECK(none == 0, "%s: an explicit-only filter on all-ages posts keeps %d", file, none);
  lv_posts_free(x, none);
  lv_posts_free(posts, count);
  free(t);
}

int main(void) {
  // working out what kind a site added by hand is, from its answers
  {
    size_t n1, n2, n3;
    char *e6 = slurp("tests/booru/e621.json", &n1), *dan = slurp("tests/booru/danbooru.json", &n2), *moe = slurp("tests/booru/moebooru.json", &n3);
    CHECK(lv_booru_probe(BK_E621, e6, n1) && !lv_booru_probe(BK_DANBOORU, e6, n1), "e621's answer is e621's kind only");
    CHECK(lv_booru_probe(BK_DANBOORU, dan, n2) && !lv_booru_probe(BK_E621, dan, n2), "danbooru's answer is danbooru's kind");
    CHECK(lv_booru_probe(BK_MOEBOORU, moe, n3), "moebooru's");
    CHECK(!lv_booru_probe(BK_GELBOORU, "<html>", 6), "a web page is no kind");
    free(e6); free(dan); free(moe);
    char u[256];
    lv_booru_probe_url(BK_GELBOORU, "https://example.org", u, sizeof u);
    CHECK(!strcmp(u, "https://example.org/index.php?page=dapi&s=post&q=index&json=1&limit=1"), "probe url: %s", u);
    int before = LV_NSITES;
    CHECK(lv_booru_add_site("My booru", BK_MOEBOORU, "https://my.example/") == 0 && LV_NSITES == before + 1, "added");
    CHECK(!strcmp(LV_SITES[before].base, "https://my.example") && !strcmp(LV_SITES[before].name, "My booru"), "its address, trimmed");
    lv_booru_url(&LV_SITES[before], "x", LV_R_ALL, LV_SORT_LATEST, 1, u, sizeof u);
    CHECK(!strncmp(u, "https://my.example/post.json?", 29), "its searches: %s", u);
    LV_NSITES = before;
  }
  // Top's spans and an account's key
  {
    char u[1024];
    lv_booru_url_ex(site("Danbooru"), "blue_hair", LV_R_ALL, LV_SORT_TOP, LV_SPAN_WEEK, NULL, 1, u, sizeof u);
    CHECK(strstr(u, "order%3Ascore+age%3A%3C1w"), "danbooru's week: %s", u);
    lv_booru_url_ex(site("e621"), "", LV_R_ALL, LV_SORT_TOP, LV_SPAN_DAY, NULL, 1, u, sizeof u);
    CHECK(strstr(u, "date%3Aday"), "e621's day: %s", u);
    lv_booru_url_ex(site("Yande.re"), "", LV_R_ALL, LV_SORT_TOP, LV_SPAN_MONTH, NULL, 1, u, sizeof u);
    CHECK(strstr(u, "date%3A%3E%3D20"), "moebooru's month: %s", u);
    lv_booru_url_ex(site("Danbooru"), "x", LV_R_ALL, LV_SORT_LATEST, LV_SPAN_WEEK, NULL, 1, u, sizeof u);
    CHECK(!strstr(u, "age%3A"), "a span only for Top: %s", u);
    lv_booru_url_ex(site("Gelbooru"), "x", LV_R_ALL, LV_SORT_LATEST, 0, "12345 abc+def", 2, u, sizeof u);
    CHECK(strstr(u, "https://gelbooru.com/index.php?page=dapi") && strstr(u, "&pid=1") && strstr(u, "&user_id=12345&api_key=abc%2Bdef"), "gelbooru's key: %s", u);
    CHECK(site("Rule34") && site("Rule34")->needs_key && !site("Danbooru")->needs_key, "which need a key");
  }
  // the JSON reader
  const char *js = "{\"a\":[1,2,{\"b\":\"x\\u00e9\\\"y\"}],\"c\":-3.5,\"d\":true,\"e\":null,\"f\":\"\\ud83d\\ude00\"}";
  Json j;
  CHECK(json_parse(&j, js, strlen(js)) == 0, "parse");
  char b[64];
  CHECK(json_gn(&j, 0, "c") == -3.5, "number");
  CHECK(json_gn(&j, 0, "d") == 1, "true");
  int a = json_get(&j, 0, "a");
  CHECK(j.t[a].size == 3 && json_num(&j, json_at(&j, a, 1)) == 2, "array");
  CHECK(!strcmp(json_gs(&j, json_at(&j, a, 2), "b", b, sizeof b), "x\xc3\xa9\"y"), "escapes: %s", b);
  CHECK(!strcmp(json_gs(&j, 0, "f", b, sizeof b), "\xf0\x9f\x98\x80"), "surrogates");
  CHECK(json_get(&j, 0, "zz") < 0 && !json_gs(&j, 0, "e", b, sizeof b)[0], "missing and null");
  json_free(&j);
  CHECK(json_parse(&j, "[1,2", 4) != 0, "truncated is an error");
  CHECK(json_parse(&j, "<!DOCTYPE html>", 15) != 0, "a web page is an error");

  check_kind("danbooru", site("Danbooru"), 5);
  check_kind("moebooru", site("Konachan"), 5);
  // (four of these five are tagged "young": never shown)
  check_kind("e621", site("e621"), 1);
  check_kind("gelbooru", site("Safebooru"), 5);
  check_kind("sakugabooru", site("Sakugabooru"), 5);

  // searches: what's never searched for is dropped; ratings and orders become the site's words
  char q[256], url[2048];
  CHECK(!strcmp(lv_booru_clean("blue_hair Loli -shota smile ~cub", q, sizeof q), "blue_hair smile"), "clean: %s", q);
  lv_booru_url(site("Danbooru"), "blue_hair loli", LV_R_GENERAL | LV_R_SENSITIVE, LV_SORT_TOP, 2, url, sizeof url);
  CHECK(!strcmp(url, "https://danbooru.donmai.us/posts.json?limit=40&page=2&tags=blue_hair+rating%3Ag%2Cs+order%3Ascore"), "danbooru url: %s", url);
  lv_booru_url(site("Yande.re"), "", LV_R_GENERAL | LV_R_SENSITIVE, LV_SORT_RANDOM, 1, url, sizeof url);
  CHECK(!strcmp(url, "https://yande.re/post.json?limit=40&page=1&tags=-rating%3Aq+-rating%3Ae+order%3Arandom"), "moebooru url: %s", url);
  lv_booru_url(site("Xbooru"), "a b", LV_R_ALL, LV_SORT_TOP, 3, url, sizeof url);
  CHECK(!strcmp(url, "https://xbooru.com/index.php?page=dapi&s=post&q=index&json=1&limit=40&pid=2&tags=a+b+sort%3Ascore"), "gelbooru url: %s", url);
  lv_booru_url(site("e621"), "", LV_R_EXPLICIT, LV_SORT_HOT, 1, url, sizeof url);
  CHECK(!strcmp(url, "https://e621.net/posts.json?limit=40&page=1&tags=-rating%3As+-rating%3Aq+order%3Ahot"), "e621 url: %s", url);

  printf(fails ? "%d FAILED\n" : "booru: all passed\n", fails);
  return fails != 0;
}
