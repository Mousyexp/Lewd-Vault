// Manga: MangaDex (its JSON API) and E-Hentai (its gallery API, and its pages for the pictures' addresses).
// The addresses and the readers of their answers; portable C (tests/manga_test.c runs it on a PC
// against saved answers). As everywhere in the app, anything tagged as sexual content involving minors
// is left out (the tags are checked word by word: "female:lolicon", MangaDex's "Loli", ...).
#pragma once
#include <stddef.h>

enum { MS_MANGADEX, MS_EHENTAI, MS_HENTAIFOX, MS_ASMHENTAI, MS_HENTAIERA, MS_NHXXX, MS_HENTAIZAP, MS_HENTAIENVY, MS_3HENTAI,
       MS_HENTAIHAND, MS_MANHWA18, MS_HWEBTOON, MS_MDISTRICT, MS_MULTPORN, MS_FAC, MS_N };
// How a source is read: its own code (MangaDex, E-Hentai), or one of three kinds of site
enum { MF_MANGADEX, MF_EHENTAI, MF_GALLERY, MF_HHAND, MF_SERIES };
// What it has, shown on its tile: manga (all kinds, 18+ included), hentai manga, hentai webtoons, short comics
enum { MK_MANGA, MK_HMANGA, MK_HWEBTOON, MK_SHORT, MK_N };
extern const char *const LV_MK_NAME[MK_N];
typedef struct {
  const char *name, *icon, *about;
  int family, kind;
  const char *base;    // https://site
  const char *list;    // a page of its list: %d the page, from 1 (NULL: one page only)
  const char *search;  // a search: %s the words, %d the page
  const char *item;    // the start of a book's address ("/gallery/", "/webtoon/"); "*" any one-part path
  const char *item2;   // (another, or NULL)
  const char *img;     // series: a piece every page picture's address has
  int single;          // series: a book is its own pages (no chapters)
  int ajax;            // series: its chapters from <book>ajax/chapters/ (POST: WordPress "Madara" sites)
  int page0;           // the list's pages counted from 0
} LvMSrc;
extern const LvMSrc LV_MSRC[MS_N];
typedef struct {
  char id[200], token[16]; // MangaDex: the manga's id; E-Hentai: gid and token; others: the book's path
  char title[200], cover[400], tags[1200];
  int pages;               // (E-Hentai: the gallery's; MangaDex: 0)
} LvBook;
typedef struct {
  char id[200], label[120]; // (others: the chapter's path)
  int pages;
} LvChapter;

// ---- the other sites (LV_MSRC) ----
// A page of the list (page from 1), or a search when query has words: the full address
void lv_ms_list_url(int src, const char *query, int page, char *out, size_t cap);
// The books a list page links to (gallery and series sites): id the book's path, a title, a cover
int lv_ms_parse_list(int src, const char *html, LvBook *out, int cap);
// A gallery's pages, from its page (gallery sites): -2 when its tags carry a never-shown word
int lv_ms_parse_gallery(int src, const char *html, char ***urls);
// A series' chapters, first to last, from its page (or its ajax list): -2 when its genres carry one
int lv_ms_parse_chapters(int src, const char *html, const char *book, LvChapter *out, int cap);
// A chapter's pictures (or a single gallery's, series sites)
int lv_ms_parse_pages(int src, const char *html, char ***urls);
// HentaiHand's API: a list, and a book's pictures
int lv_hh_parse_list(const char *json, size_t n, LvBook *out, int cap);
int lv_hh_parse_images(const char *json, size_t n, char ***urls);
// The full address of a path on the source (absolute and protocol-relative ones kept)
void lv_ms_url(int src, const char *path, char *out, size_t cap);

// 1 when the tags (any separator) carry one of the never-shown words.
int lv_manga_never(const char *tags);

// MangaDex. order: 0 most followed, 1 latest updated, 2 newest. adult: erotica and pornographic too.
void lv_md_list_url(const char *query, int adult, int order, int offset, char *out, size_t cap);
int lv_md_parse_list(const char *json, size_t n, LvBook *out, int cap, int *total);
void lv_md_feed_url(const char *manga_id, int adult, int offset, char *out, size_t cap);
void lv_md_feed_url_lang(const char *manga_id, int adult, int offset, int any_lang, char *out, size_t cap);
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

// A webtoon's tall strip is read in bands (a page each): "<address>#lvband=<k>,<rows>,<total>" is
// band k, of <rows> rows of the <total> the page said it has. Returns 1 and fills them (base: the
// address without it), 0 for a plain address.
int lv_band_of(const char *url, char *base, size_t cap, int *k, int *rows, int *total);
