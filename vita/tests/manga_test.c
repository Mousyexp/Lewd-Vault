// The manga readers (src/manga.c) against answers saved from MangaDex and E-Hentai (tests/manga/:
// MangaDex's all-ages titles, an E-Hentai Non-H gallery), and the never-shown check.
#include "../src/manga.h"
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

int main(void) {
  // never shown
  CHECK(lv_manga_never("female:lolicon|language:english"), "e-hentai's lolicon");
  CHECK(lv_manga_never("Romance|Loli|Action"), "mangadex's Loli");
  CHECK(lv_manga_never("female:oppai loli"), "a word inside a tag");
  CHECK(!lv_manga_never("female:big breasts|parody:smiling friends|Childhood Friends"), "nothing of the kind (childhood isn't child)");

  size_t n;
  char *t = slurp("tests/manga/mangadex_list.json", &n);
  LvBook b[40];
  int total = 0, got = t ? lv_md_parse_list(t, n, b, 40, &total) : -1;
  CHECK(got >= 3 && total > 100, "mangadex list: %d of %d", got, total);
  for (int k = 0; k < got; k++) CHECK(b[k].id[0] && b[k].title[0] && !strncmp(b[k].cover, "https://uploads.mangadex.org/covers/", 36) && strstr(b[k].cover, ".256.jpg"), "a book: %s | %s", b[k].title, b[k].cover);
  if (got > 0) printf("mangadex  %d books, first: %s (%.60s)\n", got, b[0].title, b[0].tags);
  free(t);
  t = slurp("tests/manga/mangadex_feed.json", &n);
  LvChapter c[100];
  got = t ? lv_md_parse_feed(t, n, c, 100, &total) : -1;
  CHECK(got >= 1 && c[0].pages > 0 && !strncmp(c[0].label, "Chapter", 7), "feed: %d, %s", got, got > 0 ? c[0].label : "");
  free(t);
  t = slurp("tests/manga/mangadex_athome.json", &n);
  char **u = NULL;
  got = t ? lv_md_parse_pages(t, n, &u) : -1;
  CHECK(got > 5 && strstr(u[0], "/data-saver/") && !strncmp(u[0], "https://", 8), "pages: %d %s", got, got > 0 ? u[0] : "");
  if (got > 0) printf("  a page  %s\n", u[0]);
  lv_strings_free(u, got);
  free(t);
  char url[1500];
  lv_md_list_url("frieren", 1, 0, 30, url, sizeof url);
  CHECK(strstr(url, "title=frieren") && strstr(url, "contentRating[]=pornographic") && strstr(url, "offset=30"), "list url: %s", url);
  lv_md_list_url("", 0, 1, 0, url, sizeof url);
  CHECK(!strstr(url, "erotica") && strstr(url, "latestUploadedChapter"), "all-ages, latest: %s", url);

  // E-Hentai
  t = slurp("tests/manga/ehentai_list.html", &n);
  long long gids[60];
  char toks[60][16];
  got = t ? lv_eh_parse_list(t, gids, toks, 60) : -1;
  CHECK(got >= 10, "e-hentai list: %d galleries", got);
  free(t);
  lv_eh_gdata_body(gids, toks, 2, url, sizeof url);
  CHECK(strstr(url, "\"method\":\"gdata\"") && strstr(url, "\"gidlist\":[["), "gdata body: %s", url);
  t = slurp("tests/manga/ehentai_gdata.json", &n);
  got = t ? lv_eh_parse_gdata(t, n, b, 40) : -1;
  CHECK(got == 1 && b[0].pages > 0 && b[0].token[0] && !strncmp(b[0].cover, "https://", 8), "gdata: %d", got);
  if (got > 0) printf("e-hentai  %s, %d pages, %s\n", b[0].title, b[0].pages, b[0].cover);
  free(t);
  t = slurp("tests/manga/ehentai_gallery.html", &n);
  got = t ? lv_eh_parse_gallery(t, b[0].id, &u) : -1;
  CHECK(got >= 10 && strstr(u[0], "-1") && strstr(u[1], "-2"), "gallery pages: %d %s", got, got > 0 ? u[0] : "");
  lv_strings_free(u, got);
  free(t);
  t = slurp("tests/manga/ehentai_page.html", &n);
  char img[600];
  CHECK(t && lv_eh_parse_page(t, img, sizeof img) == 0 && !strncmp(img, "http", 4), "the picture: %s", img);
  char nl[64];
  CHECK(t && lv_eh_parse_reload(t, nl, sizeof nl) == 0 && nl[0], "the reload key: %s", nl);
  free(t);
  lv_eh_list_url("english", 2, url, sizeof url);
  CHECK(strstr(url, "f_cats=993") && strstr(url, "page=2") && strstr(url, "lolicon"), "e-hentai url: %s", url);

  printf(fails ? "%d FAILED\n" : "manga: all passed\n", fails);
  // other sources: a gallery's own tags are checked, a "related" block's aren't
  {
    char **u = NULL;
    const char *ok = "<div class=\"info\"><a href=\"/tag/milf/\">milf</a></div><div class=\"gthumb\"><img data-src=\"https://i.example/1/1t.jpg\"></div>"
                     "<div class=\"related_galleries\"><a href=\"/tag/shotacon/\">shotacon</a></div>";
    int got = lv_ms_parse_gallery(MS_HENTAIFOX, ok, &u);
    CHECK(got != -2, "a related gallery's tag blocked this one");
    lv_strings_free(u, got);
    const char *bad = "<div class=\"info\"><a href=\"/tag/lolicon/\">lolicon</a></div><div class=\"related\"></div>";
    CHECK(lv_ms_parse_gallery(MS_HENTAIFOX, bad, &u) == -2, "its own tag must block it");
    // Manhwa18: pages under /chapters/ or /uploads/
    const char *ch = "<img class=\"logo\" src=\"/images/logo.png\"><img data-src=\"https://img01.manhwa18.cc/uploads/1/1/1-5.jpg\"><img src=\"https://img22.manhwa18.cc/chapters/2/0/2-5.jpg\">";
    got = lv_ms_parse_pages(MS_MANHWA18, ch, &u);
    CHECK(got == 2, "manhwa18 pages: %d", got);
    lv_strings_free(u, got);
  }
  // a webtoon's strip with its size: bands about a page's shape; a plain page stays one
  {
    char **u = NULL;
    const char *st = "<img src=\"/assets/images/chapters/x/chapter-1/0001.webp\" width=\"700\" height=\"900\">"
                     "<img src=\"/assets/images/chapters/x/chapter-1/reader-strip-0002.jpg\" width=\"700\" height=\"23450\">";
    int got = lv_ms_parse_pages(MS_HWEBTOON, st, &u);
    CHECK(got == 1 + 23, "strip bands: %d", got);
    char base[300]; int k = -1, rows = 0, total = 0;
    CHECK(got > 2 && !strstr(u[0], "#lvband") && lv_band_of(u[2], base, sizeof base, &k, &rows, &total) && k == 1 && rows == 1020 && total == 23450
          && !strcmp(base, "https://hentaiwebtoon.com/assets/images/chapters/x/chapter-1/reader-strip-0002.jpg"), "band: %s", got > 2 ? u[2] : "");
    CHECK(!lv_band_of("https://a.example/1.jpg", base, sizeof base, &k, &rows, &total), "a plain page");
    lv_strings_free(u, got);
  }
  return fails != 0;
}
