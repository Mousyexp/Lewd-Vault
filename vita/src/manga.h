// Manga: MangaDex (its JSON API) and E-Hentai (its gallery API, and its pages for the pictures' addresses).
// The addresses and the readers of their answers; portable C (tests/manga_test.c runs it on a PC
// against saved answers). As everywhere in the app, anything tagged as sexual content involving minors
// is left out (the tags are checked word by word: "female:lolicon", MangaDex's "Loli", ...).
#pragma once
#include <stddef.h>

enum { MS_MANGADEX, MS_EHENTAI, MS_N };
typedef struct {
  char id[64], token[16];  // MangaDex: the manga's id; E-Hentai: gid and token
  char title[200], cover[400], tags[1200];
  int pages;               // (E-Hentai: the gallery's; MangaDex: 0)
} LvBook;
typedef struct {
  char id[64], label[120];
  int pages;
} LvChapter;

// 1 when the tags (any separator) carry one of the never-shown words.
int lv_manga_never(const char *tags);

// MangaDex. order: 0 most followed, 1 latest updated, 2 newest. adult: erotica and pornographic too.
void lv_md_list_url(const char *query, int adult, int order, int offset, char *out, size_t cap);
int lv_md_parse_list(const char *json, size_t n, LvBook *out, int cap, int *total);
void lv_md_feed_url(const char *manga_id, int adult, int offset, char *out, size_t cap);
// (chapters hosted elsewhere, or with no pages, are left out)
int lv_md_parse_feed(const char *json, size_t n, LvChapter *out, int cap, int *total);
void lv_md_pages_url(const char *chapter_id, char *out, size_t cap);
// The chapter's pages (the smaller "data saver" ones): a malloc'd array of malloc'd addresses.
int lv_md_parse_pages(const char *json, size_t n, char ***urls);

// E-Hentai (doujinshi, manga, artist and game CG only). page: 0, 1, ...
void lv_eh_list_url(const char *query, int page, char *out, size_t cap);
// The galleries a list page links to: up to cap of them.
int lv_eh_parse_list(const char *html, long long *gids, char (*tokens)[16], int cap);
// The gallery API's question for those (POST to LV_EH_API).
#define LV_EH_API "https://api.e-hentai.org/api.php"
void lv_eh_gdata_body(const long long *gids, char (*tokens)[16], int n, char *out, size_t cap);
int lv_eh_parse_gdata(const char *json, size_t n, LvBook *out, int cap);
// A gallery's page of thumbnails (p: 0, 1, ...), and the picture pages it links to, in order.
void lv_eh_gallery_url(const LvBook *b, int p, char *out, size_t cap);
int lv_eh_parse_gallery(const char *html, const char *gid, char ***urls);
// A picture page's picture.
int lv_eh_parse_page(const char *html, char *out, size_t cap);
// Its "reload from another server" key (nl('...')): 0 and the key, or -1.
int lv_eh_parse_reload(const char *html, char *out, size_t cap);
void lv_strings_free(char **s, int n);
