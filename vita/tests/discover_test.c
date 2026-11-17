// The site finder's readers (src/discover.c) against saved copies of the lists it reads.
#include "../src/discover.h"
#include "../src/booru.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static char *slurp(const char *p, size_t *n) {
  FILE *f = fopen(p, "rb");
  if (!f) { printf("missing %s\n", p); exit(1); }
  fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
  char *b = malloc(l + 1);
  *n = fread(b, 1, l, f); b[*n] = 0;
  fclose(f);
  return b;
}
static const LvCand *find(const LvCand *c, int n, const char *host) {
  for (int k = 0; k < n; k++) if (strstr(c[k].base, host)) return &c[k];
  return NULL;
}
int main(void) {
  LvCand c[200];
  int n = 0;
  size_t len;
  char *md = slurp("tests/discover/supportedsites.md", &len);
  int a = lv_disc_gallerydl(md, len, c, &n, 200);
  CHECK(a >= 8);
  const LvCand *x = find(c, n, "danbooru.donmai.us");
  CHECK(x && x->kind == BK_DANBOORU && !strcmp(x->name, "Danbooru") && !strcmp(x->base, "https://danbooru.donmai.us"));
  x = find(c, n, "yande.re");
  CHECK(x && x->kind == BK_MOEBOORU);
  x = find(c, n, "hypnohub.net");
  CHECK(x && x->kind == BK_GELBOORU);
  CHECK(find(c, n, "sakugabooru.com") != NULL);
  // never: minors, furry, real people, the hidden mixed ones
  CHECK(!find(c, n, "allthefallen")); CHECK(!find(c, n, "lolibooru")); CHECK(!find(c, n, "e621"));
  CHECK(!find(c, n, "e926")); CHECK(!find(c, n, "rule34")); CHECK(!find(c, n, "tbib"));
  // (not of the three kinds: Shimmie, szurubooru, Philomena)
  CHECK(!find(c, n, "derpibooru")); CHECK(!find(c, n, "paheal"));
  int before = n;
  char *js = slurp("tests/discover/sites.json", &len);
  lv_disc_boorujs(js, len, c, &n, 200);
  CHECK(find(c, n, "konachan.net") && find(c, n, "konachan.net")->kind == BK_MOEBOORU);
  CHECK(find(c, n, "gelbooru.com") && find(c, n, "gelbooru.com")->kind == BK_GELBOORU);
  CHECK(!find(c, n, "realbooru")); CHECK(!find(c, n, "e926"));
  int dups = 0;
  for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (lv_disc_same_site(c[i].base, c[j].base)) dups++;
  CHECK(dups == 0);
  CHECK(n > before);
  char *gr = slurp("tests/discover/grabber_danbooru.txt", &len);
  CHECK(lv_disc_lines(gr, len, BK_DANBOORU, c, &n, 200) == 0); // (donmai.us mirrors: Danbooru again)
  char *gg = slurp("tests/discover/grabber_gelbooru.txt", &len);
  lv_disc_lines(gg, len, BK_GELBOORU, c, &n, 200);
  CHECK(!find(c, n, "rule34"));
  CHECK(lv_disc_never("Lolibooru", "https://lolibooru.moe"));
  CHECK(lv_disc_never("x", "https://furry.booru.org"));
  CHECK(!lv_disc_never("Future booru", "https://futurebooru.example"));
  CHECK(lv_disc_same_site("https://danbooru.donmai.us", "https://sonohara.donmai.us/"));
  CHECK(!lv_disc_same_site("https://konachan.com", "https://konachan.net"));
  for (int k = 0; k < n; k++) printf("  %-24s %-40s %d\n", c[k].name, c[k].base, c[k].kind);
  free(md); free(js); free(gr); free(gg);
  printf(fails ? "discover: %d failed\n" : "discover: all passed\n", fails);
  return fails != 0;
}
