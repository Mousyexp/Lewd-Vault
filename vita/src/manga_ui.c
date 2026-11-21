// Manga: the covers of a source (MangaDex or E-Hentai), a book's page (its chapters), and the reader.
// Opened from the Manga tab of the sites window. Everything that goes over the network happens on one
// worker thread (lists, covers, chapters, pages), newest wish first; the screen shows a spinner
// meanwhile. Covers are decoded to exactly a card's size, pages to exactly the screen's (twice that
// only when zoomed in). Compiled inside main.c, after vbrowse.c.
#include "manga.h"

#define MG_CW 148
#define MG_CH 210
#define MG_MAX 900  // (books in a list)
#define MG_TEX 48   // (covers kept as textures: those on screen and about to be)

static const char *MD_ORDER[3] = {"Popular", "Latest", "New"};

static struct {
  int src, order;
  char query[128];
  LvBook *books;
  int nbooks, total, more, loading;
  char err[120];
  int sel;
  float scroll;
  // covers
  vita2d_texture *tex[MG_TEX];
  int tex_book[MG_TEX];
  unsigned tex_used[MG_TEX];
  signed char *cstate; // per book: 0, 1 asked, 2 here, 3 failed
  // the book open
  int book;
  LvChapter *ch;
  int nch, ch_loading, ch_sel;
  float ch_scroll;
  vita2d_texture *big_cover;
  // the reader
  int chapter;          // (MangaDex: which; E-Hentai: 0)
  char **pages;         // their addresses (E-Hentai: the picture pages', looked at as they're read)
  int npages, pages_total, pages_loading, gallery_p;
  int page;
  vita2d_texture *ptex[3]; // the page shown, and the next two
  int ptex_page[3];
  float zoom, pan_x, pan_y, swipe;
  int tall_top; // (a tall page just turned to: it starts at its top)
} mg = {.book = -1, .page = 0, .zoom = 1, .tall_top = 1};
static int manga_back = S_WEB;
static int nmsources(void) { return MS_N; }
static const char *msource_name(int k) { return k >= 0 && k < MS_N ? LV_MSRC[k].name : ""; }
static const char *msource_icon(int k) { return k >= 0 && k < MS_N ? LV_MSRC[k].icon : NULL; }
static const char *msource_about(int k) { return k >= 0 && k < MS_N ? LV_MSRC[k].about : ""; }
static int msource_kind(int k) { return k >= 0 && k < MS_N ? LV_MSRC[k].kind : MK_MANGA; } // (its label on its tile)
// (a source whose books have chapters to choose from: MangaDex, the webtoon sites; the others are
//  read straight through, one gallery each)
static int mg_chapters(int src) { return src == MS_MANGADEX || (LV_MSRC[src].family == MF_SERIES && !LV_MSRC[src].single); }
static int mg_src_now(void) { return mg.src; }

// ---------- the worker ----------
enum { MJ_NONE, MJ_LIST, MJ_COVER, MJ_BIGCOVER, MJ_CHAPTERS, MJ_PAGES, MJ_PAGE };
typedef struct { int kind, a, b, gen, src; char url[600]; } MJob;
static MJob mjobs[32];
static int nmjobs;
static volatile int mg_gen = 1;
static pthread_mutex_t mgmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mgcv = PTHREAD_COND_INITIALIZER;
static int mg_thread_on;
typedef struct { int kind, a, b, gen; LvPixels px; LvBook *books; int n, total; LvChapter *ch; char **urls; char err[120]; } MDone;
static MDone mdone[16];
static int nmdone;

static void mg_push(int kind, int a, int b, const char *url) {
  pthread_mutex_lock(&mgmx);
  // (the newest wish goes first; the oldest covers go if it's full)
  if (nmjobs == 32) { for (int k = 0; k < nmjobs; k++) if (mjobs[k].kind == MJ_COVER) { memmove(&mjobs[k], &mjobs[k + 1], (--nmjobs - k) * sizeof *mjobs); break; } }
  if (nmjobs < 32) {
    MJob *j = &mjobs[nmjobs++];
    j->kind = kind; j->a = a; j->b = b; j->gen = mg_gen; j->src = mg.src;
    snprintf(j->url, sizeof j->url, "%s", url ? url : "");
  }
  pthread_cond_signal(&mgcv);
  pthread_mutex_unlock(&mgmx);
}
// (the Comics tab left: the covers waiting to be fetched dropped, asked again when it's back)
static void mg_drop_covers(void) {
  pthread_mutex_lock(&mgmx);
  int kept = 0;
  for (int k = 0; k < nmjobs; k++) {
    if (mjobs[k].kind == MJ_COVER) { if (mg.cstate && mjobs[k].gen == mg_gen && mjobs[k].a >= 0 && mjobs[k].a < mg.nbooks) mg.cstate[mjobs[k].a] = 0; }
    else mjobs[kept++] = mjobs[k];
  }
  nmjobs = kept;
  pthread_mutex_unlock(&mgmx);
}
static void mg_done(MDone *d) {
  pthread_mutex_lock(&mgmx);
  while (nmdone == 16 && worker_run) pthread_cond_wait(&mgcv, &mgmx); // (woken when some are taken, not looking every 5 ms)
  mdone[nmdone++] = *d;
  pthread_mutex_unlock(&mgmx);
}
static const char *const MG_UA[] = {"User-Agent: LewdVaultVita/1.0 (PS Vita)", NULL};
static int mg_get(LvHttp *h, const char *url, uint8_t **b, size_t *n, size_t max, const char **err) {
  LvReq rq = {.headers = MG_UA};
  return lv_http_req(h, url, &rq, b, n, max, NULL, err);
}
// A page's picture. E-Hentai: the picture page first, for the picture's address; a picture server
// that doesn't answer: the page again, asking for another server (its "reload"), twice at most.
// Stops early when *gen moves on from gen.
// ----- a webtoon's strip (700 x 23000, a chapter in three or four): cut into page-sized bands once,
//       top to bottom, each kept as a small JPEG; the reader and Save ask for one band at a time -----
typedef struct { char url[640]; int nb; uint8_t **jpg; size_t *len; } Strip;
static Strip strips[2];   // (the last two strips cut: a chapter's pages go strip by strip)
static int strip_next;
static pthread_mutex_t strip_mx = PTHREAD_MUTEX_INITIALIZER;     // (the two above)
static pthread_mutex_t strip_cut_mx = PTHREAD_MUTEX_INITIALIZER; // (one strip cut at a time: the most memory it takes)
// (how the cutting has gone: the test kit's report)
static struct { int cuts; float ms, ms_max; unsigned store_kb; } strip_stats;
static void strip_stats_reset(void) { memset(&strip_stats, 0, sizeof strip_stats); }
static void strip_free(Strip *s) {
  for (int k = 0; k < s->nb && s->jpg; k++) free(s->jpg[k]);
  free(s->jpg); free(s->len);
  memset(s, 0, sizeof *s);
}
// band k of a strip cut already: 1 copied, -1 the strip's here without it, 0 not cut
static int strip_copy(const char *url, int k, uint8_t **b, size_t *n) {
  int r = 0;
  pthread_mutex_lock(&strip_mx);
  for (int i = 0; i < 2 && !r; i++) {
    Strip *s = &strips[i];
    if (!s->jpg || strcmp(s->url, url)) continue;
    r = -1;
    if (k < s->nb && s->jpg[k] && (*b = malloc(s->len[k]))) { memcpy(*b, s->jpg[k], s->len[k]); *n = s->len[k]; r = 1; }
  }
  pthread_mutex_unlock(&strip_mx);
  return r;
}
static int strip_band(void *ctx, int k, LvPixels *px) {
  Strip *s = ctx;
  if (k < s->nb) s->jpg[k] = lv_encode_jpeg(px, 90, &s->len[k]);
  free(px->rgba);
  return k + 1 >= s->nb;
}
static int mg_fetch_band(LvHttp *h, const char *url, int k, int rows, int total, uint8_t **b, size_t *n, const char **err) {
  int r = strip_copy(url, k, b, n);
  if (r) return r > 0;
  pthread_mutex_lock(&strip_cut_mx);
  r = strip_copy(url, k, b, n); // (cut while this waited: the other thread asked for the same strip)
  if (r) { pthread_mutex_unlock(&strip_cut_mx); return r > 0; }
  uint8_t *raw = NULL; size_t rn = 0;
  int iw = 0, ih = 0;
  if (mg_get(h, url, &raw, &rn, 24u << 20, err) != 200 || !lv_image_size(raw, rn, &iw, &ih) || iw < 1 || ih < 1) {
    free(raw); pthread_mutex_unlock(&strip_cut_mx); return 0;
  }
  // (cut a few bands at a time: never the whole strip's pixels; a progressive JPEG's coefficients
  //  kept compressed, ~a quarter of a byte a pixel, image.c)
  if (!heap_room((size_t)iw * ih / 2 + (12u << 20))) {
    free(raw); pthread_mutex_unlock(&strip_cut_mx); *err = "Not enough memory for this strip"; return 0;
  }
  // (the bands as the page measured them, in the file's own rows)
  int br = total > 0 ? (int)(((long long)rows * ih + total - 1) / total) : rows;
  if (br < 16) br = 16;
  Strip s;
  memset(&s, 0, sizeof s);
  snprintf(s.url, sizeof s.url, "%s", url);
  s.nb = (ih + br - 1) / br;
  s.jpg = calloc(s.nb, sizeof *s.jpg);
  s.len = calloc(s.nb, sizeof *s.len);
  uint64_t cut0 = sceKernelGetProcessTimeWide();
  int got = s.jpg && s.len ? lv_decode_bands(raw, rn, br, W, strip_band, &s) : -1;
  free(raw);
  {
    float ms = (sceKernelGetProcessTimeWide() - cut0) / 1000.0f;
    unsigned kb = (unsigned)(lv_jpeg_store_peak() >> 10);
    strip_stats.cuts++; strip_stats.ms += ms; if (ms > strip_stats.ms_max) strip_stats.ms_max = ms;
    if (kb > strip_stats.store_kb) strip_stats.store_kb = kb;
  }
  LOG("strip %s: %dx%d, %d bands of %d rows -> %d (coefficients held %u KB)\n", url, iw, ih, s.nb, br, got, strip_stats.store_kb);
  if (got <= 0) { strip_free(&s); pthread_mutex_unlock(&strip_cut_mx); *err = "This strip couldn't be read"; return 0; }
  pthread_mutex_lock(&strip_mx);
  strip_free(&strips[strip_next]);
  strips[strip_next] = s;
  strip_next ^= 1;
  pthread_mutex_unlock(&strip_mx);
  pthread_mutex_unlock(&strip_cut_mx);
  return strip_copy(url, k, b, n) > 0;
}

static int mg_fetch_page(LvHttp *h, int src, const char *url, volatile int *genp, int gen, uint8_t **b, size_t *n, const char **err) {
  *b = NULL; *n = 0;
  {
    char base[700];
    int bk, brows, btot;
    if (lv_band_of(url, base, sizeof base, &bk, &brows, &btot)) return mg_fetch_band(h, base, bk, brows, btot, b, n, err);
  }
  if (src != MS_EHENTAI) {
    if (mg_get(h, url, b, n, 16u << 20, err) == 200) return 1;
    // (a gallery site's page that isn't the kind its thumbnail was: the others tried)
    if (LV_MSRC[src].family != MF_GALLERY) return 0;
    static const char *const EXT[] = {"webp", "jpg", "png", "gif"};
    char alt[700];
    const char *dot = strrchr(url, '.');
    if (!dot) return 0;
    for (int k = 0; k < 4; k++) {
      if (!strcmp(dot + 1, EXT[k])) continue;
      free(*b); *b = NULL;
      snprintf(alt, sizeof alt, "%.*s.%s", (int)(dot - url), url, EXT[k]);
      if (mg_get(h, alt, b, n, 16u << 20, err) == 200) return 1;
    }
    return 0;
  }
  char img[600], page_url[700];
  snprintf(page_url, sizeof page_url, "%s", url);
  for (int attempt = 0; attempt < 3 && *genp == gen; attempt++) {
    img[0] = 0;
    char nl[64] = "";
    if (mg_get(h, page_url, b, n, 1u << 20, err) == 200) { lv_eh_parse_page((char *)*b, img, sizeof img); lv_eh_parse_reload((char *)*b, nl, sizeof nl); }
    free(*b); *b = NULL;
    if (img[0] && mg_get(h, img, b, n, 16u << 20, err) == 200) return 1;
    free(*b); *b = NULL;
    if (!nl[0]) break;
    snprintf(page_url, sizeof page_url, "%s%snl=%s", url, strchr(url, '?') ? "&" : "?", nl);
  }
  return 0;
}

static void *mg_main(void *arg) {
  (void)arg;
  pin_to_core(2);
  LvHttp *h = lv_http_new();
  while (worker_run) {
    pthread_mutex_lock(&mgmx);
    while (worker_run && !nmjobs) wait_work(&mgcv, &mgmx, h);
    if (!worker_run) { pthread_mutex_unlock(&mgmx); break; }
    // newest first (the reader's page before the covers asked for earlier)
    MJob j = mjobs[--nmjobs];
    for (int k = nmjobs - 1; k >= 0; k--) if (mjobs[k].kind == MJ_PAGE && j.kind != MJ_PAGE) { MJob t = mjobs[k]; mjobs[k] = j; j = t; break; }
    pthread_mutex_unlock(&mgmx);
    if (j.gen != mg_gen) continue;
    MDone d;
    memset(&d, 0, sizeof d);
    d.kind = j.kind; d.a = j.a; d.b = j.b; d.gen = j.gen;
    uint8_t *b = NULL;
    size_t n = 0;
    const char *err = NULL;
    int st;
    switch (j.kind) {
      case MJ_LIST:
        if (j.src == MS_MANGADEX) {
          st = mg_get(h, j.url, &b, &n, 4u << 20, &err);
          d.books = calloc(40, sizeof *d.books);
          d.n = st == 200 && d.books ? lv_md_parse_list((char *)b, n, d.books, 40, &d.total) : -1;
        } else if (j.src != MS_EHENTAI) {
          // the other sites: a page of their list, as HTML (or HentaiHand's JSON)
          st = mg_get(h, j.url, &b, &n, 6u << 20, &err);
          d.books = calloc(60, sizeof *d.books);
          if (st == 200 && d.books) d.n = LV_MSRC[j.src].family == MF_HHAND ? lv_hh_parse_list((char *)b, n, d.books, 60) : lv_ms_parse_list(j.src, (char *)b, d.books, 60);
          else d.n = -1;
          if (st == 404) d.n = 0; // (past the last page)
          if (b && st != 200 && strstr((char *)b, "Just a moment")) err = "The site is asking for a browser check";
          // (anything AI-made by its name: not shown, as everywhere in the app)
          int kept = 0;
          for (int k = 0; d.books && k < d.n; k++) if (!lv_ai_made(d.books[k].title)) d.books[kept++] = d.books[k];
          if (d.n > 0) d.n = kept;
          d.total = 1 << 30;
        } else {
          // E-Hentai: the list's galleries, then their details from its API
          st = mg_get(h, j.url, &b, &n, 4u << 20, &err);
          long long gids[25];
          char toks[25][16];
          int ng = st == 200 ? lv_eh_parse_list((char *)b, gids, toks, 25) : 0;
          free(b); b = NULL;
          d.n = -1;
          if (ng > 0) {
            char body[2000];
            lv_eh_gdata_body(gids, toks, ng, body, sizeof body);
            static const char *const hs[] = {"Content-Type: application/json", NULL};
            LvReq rq = {.method = "POST", .headers = hs, .body = body, .body_len = strlen(body)};
            st = lv_http_req(h, LV_EH_API, &rq, &b, &n, 2u << 20, NULL, &err);
            d.books = calloc(25, sizeof *d.books);
            d.n = st == 200 && d.books ? lv_eh_parse_gdata((char *)b, n, d.books, 25) : -1;
            d.total = 1 << 30; // (more pages until one is empty)
          } else if (st == 200) d.n = 0;
        }
        if (d.n < 0) snprintf(d.err, sizeof d.err, "%s", err && err[0] ? err : "The site's answer couldn't be read");
        break;
      case MJ_COVER: case MJ_BIGCOVER:
        if (mg_get(h, j.url, &b, &n, 4u << 20, &err) == 200)
          decode_exact(b, n, j.kind == MJ_COVER ? MG_CW : 220, j.kind == MJ_COVER ? MG_CH : 312, &d.px);
        break;
      case MJ_CHAPTERS:
        if (j.src == MS_MANGADEX) {
          st = mg_get(h, j.url, &b, &n, 4u << 20, &err);
          d.ch = calloc(100, sizeof *d.ch);
          d.n = st == 200 && d.ch ? lv_md_parse_feed((char *)b, n, d.ch, 100, &d.total) : -1;
        } else {
          // a series' page (its chapters on it), or its WordPress "ajax/chapters/" list (POST)
          char book[200], url[700];
          snprintf(book, sizeof book, "%s", j.url);
          lv_ms_url(j.src, book, url, sizeof url);
          if (LV_MSRC[j.src].ajax) {
            char ajax[760];
            snprintf(ajax, sizeof ajax, "%s%sajax/chapters/", url, url[strlen(url) - 1] == '/' ? "" : "/");
            LvReq rq = {.method = "POST", .headers = MG_UA, .body = "", .body_len = 0};
            st = lv_http_req(h, ajax, &rq, &b, &n, 4u << 20, NULL, &err);
          } else st = mg_get(h, url, &b, &n, 6u << 20, &err);
          d.ch = calloc(300, sizeof *d.ch);
          d.n = st == 200 && d.ch ? lv_ms_parse_chapters(j.src, (char *)b, book, d.ch, 300) : -1;
          if (d.n == -2) { d.n = -1; err = "Not shown here: tagged as something never shown"; }
        }
        if (d.n < 0) snprintf(d.err, sizeof d.err, "%s", err && err[0] ? err : "The chapters couldn't be read");
        break;
      case MJ_PAGES:
        st = mg_get(h, j.url, &b, &n, 8u << 20, &err);
        if (st == 200 && j.src == MS_MANGADEX) d.n = lv_md_parse_pages((char *)b, n, &d.urls);
        else if (st == 200 && LV_MSRC[j.src].family == MF_GALLERY) d.n = lv_ms_parse_gallery(j.src, (char *)b, &d.urls);
        else if (st == 200 && LV_MSRC[j.src].family == MF_HHAND) d.n = lv_hh_parse_images((char *)b, n, &d.urls);
        else if (st == 200 && LV_MSRC[j.src].family == MF_SERIES) d.n = lv_ms_parse_pages(j.src, (char *)b, &d.urls);
        else if (st == 200) {
          // (the gallery's number, from its address: https://e-hentai.org/g/<gid>/<token>/)
          char gid[24] = "";
          const char *g = strstr(j.url, "/g/");
          if (g) snprintf(gid, sizeof gid, "%.*s", (int)strcspn(g + 3, "/"), g + 3);
          d.n = lv_eh_parse_gallery((char *)b, gid, &d.urls);
        }
        else d.n = -1;
        if (d.n == -2) { d.n = -1; err = "Not shown here: tagged as something never shown"; }
        if (d.n < 0) snprintf(d.err, sizeof d.err, "%s", err && err[0] ? err : "The pages couldn't be read");
        break;
      case MJ_PAGE: {
        // (the last eight pages' files kept in memory: turning back, or forward again, needs no download)
        static struct { char url[300]; uint8_t *b; size_t n; } pc[8]; static int pc_next;
        int got_img = 0;
        for (int c = 0; c < 8 && !got_img; c++) if (pc[c].b && !strncmp(pc[c].url, j.url, sizeof pc[c].url - 1)) { b = malloc(pc[c].n); if (b) { memcpy(b, pc[c].b, pc[c].n); n = pc[c].n; got_img = 1; } }
        if (!got_img) {
          got_img = mg_fetch_page(h, j.src, j.url, &mg_gen, j.gen, &b, &n, &err);
          if (got_img && n < (3u << 20) && heap_room(n * 4)) { int c = pc_next; pc_next = (pc_next + 1) % 8; free(pc[c].b); pc[c].b = malloc(n); if (pc[c].b) { memcpy(pc[c].b, b, n); pc[c].n = n; snprintf(pc[c].url, sizeof pc[c].url, "%s", j.url); } }
        }
        if (got_img) {
          int hi = j.b; // (zoomed in: twice the screen)
          int iw = 0, ih = 0;
          // (a tall page, a webtoon's: the screen's width, as tall as a texture can be)
          int tall = lv_image_size(b, n, &iw, &ih) && iw > 0 && ih > iw * 2.2f;
          if ((tall || hi) && !heap_room((size_t)W * 4096 * 8)) { tall = 0; hi = 0; } // (keeping the heap under 95%)
          if (lv_decode_lowmem(b, n, tall ? W : hi ? W * 2 : W, tall ? 4096 : hi ? H * 2 : H, &d.px) != 0) d.px.rgba = NULL; // (a big page: in bands)
        }
        if (!d.px.rgba) snprintf(d.err, sizeof d.err, "%s", err && err[0] ? err : "This page couldn't be loaded");
        break;
      }
    }
    free(b);
    if (j.gen == mg_gen) mg_done(&d);
    else { free(d.px.rgba); free(d.books); free(d.ch); lv_strings_free(d.urls, d.n); }
  }
  lv_http_free(h);
  return NULL;
}
// ---------- a chapter (or a gallery) into the vault, as a comic ----------
// Its pages as pictures, as they come (not made smaller: they're pages to read), in the category
// Comics, tagged comic and with the book's tags, one collection for the chapter: the Library's Comics
// mode shows it as a book. On a thread of its own, so reading goes on meanwhile.
typedef struct { char id[200], label[80]; } SChap; // (a whole series: its chapters, each saved in turn)
typedef struct {
  int src, vgen, total;
  char **urls; int n;
  LvBook book;
  char coll[200], tags[1400];
  SChap *chaps; int nchaps; // (a series: chapter after chapter; else the pages above, one chapter)
} MSave;
typedef struct { LvNew nw; LvAdded added; char name[240], when[40], kind[8], coll[200]; int vgen; } MSaved;
static volatile int msave_series_at, msave_series_n; // (a series: the chapter being saved, of how many)
// (no characters a file name can't have, no doubled or trailing spaces)
static void coll_clean(char *s) {
  char *o = s;
  for (const char *c = s; *c; c++) {
    char ch = strchr("/\\:\"*?<>|", *c) ? ' ' : *c;
    if (ch == ' ' && (o == s || o[-1] == ' ')) continue;
    *o++ = ch;
  }
  while (o > s && o[-1] == ' ') o--;
  *o = 0;
}
static volatile int msave_on, msave_done, msave_total, msave_failed, msave_gen = 1;
static char msave_title[200];
static MSaved *msaved; // (pages written, waiting for the drawing thread)
static int nmsaved, capmsaved;
static pthread_mutex_t msmx = PTHREAD_MUTEX_INITIALIZER;
static char msave_tags[1400], msave_coll[200]; // (the job's, for the loaded vault's copies)

static const char *pic_ext(const uint8_t *b, size_t n) {
  if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8) return "jpg";
  if (n >= 8 && !memcmp(b, "\x89PNG", 4)) return "png";
  if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) return "webp";
  if (n >= 6 && !memcmp(b, "GIF8", 4)) return "gif";
  return NULL;
}
static void *msave_main(void *arg) {
  MSave *j = arg;
  pin_to_core(1);
  LvHttp *h = lv_http_new();
  const char *err = NULL;
  int gen = msave_gen;
  // E-Hentai: every picture page of the gallery (the reader may have seen only the first few)
  if (j->src == MS_EHENTAI && h) {
    lv_strings_free(j->urls, j->n);
    j->urls = NULL; j->n = 0;
    char gid[24];
    snprintf(gid, sizeof gid, "%s", j->book.id);
    for (int p = 0; p < 60 && msave_gen == gen && (j->n < j->total || !j->total); p++) {
      char url[600];
      lv_eh_gallery_url(&j->book, p, url, sizeof url);
      uint8_t *b = NULL; size_t n = 0;
      char **got = NULL;
      int m = mg_get(h, url, &b, &n, 8u << 20, &err) == 200 ? lv_eh_parse_gallery((char *)b, gid, &got) : -1;
      free(b);
      if (m <= 0) { lv_strings_free(got, m); break; }
      char **nu = realloc(j->urls, (j->n + m) * sizeof *nu);
      if (!nu) { lv_strings_free(got, m); break; }
      j->urls = nu;
      memcpy(j->urls + j->n, got, m * sizeof *got);
      j->n += m;
      free(got);
    }
    msave_total = j->n;
  }
  int nch = j->nchaps ? j->nchaps : 1, stop = 0;
  for (int c = 0; c < nch && h && msave_gen == gen && !stop; c++) {
  if (j->nchaps) {
    // (a series: this chapter's pages, then they're saved as one comic of their own)
    lv_strings_free(j->urls, j->n);
    j->urls = NULL; j->n = 0;
    char url[600];
    if (j->src == MS_MANGADEX) lv_md_pages_url(j->chaps[c].id, url, sizeof url); else lv_ms_url(j->src, j->chaps[c].id, url, sizeof url);
    uint8_t *b = NULL; size_t n = 0;
    if (mg_get(h, url, &b, &n, 8u << 20, &err) == 200)
      j->n = j->src == MS_MANGADEX ? lv_md_parse_pages((char *)b, n, &j->urls) : LV_MSRC[j->src].family == MF_HHAND ? lv_hh_parse_images((char *)b, n, &j->urls) : lv_ms_parse_pages(j->src, (char *)b, &j->urls);
    free(b);
    msave_series_at = c + 1;
    if (j->n <= 0) { j->n = 0; j->urls = NULL; continue; }
    snprintf(j->coll, sizeof j->coll, "%.150s \u2013 %.40s", j->book.title, j->chaps[c].label);
    coll_clean(j->coll);
    msave_done = msave_failed = 0; msave_total = j->n;
  }
  for (int k = 0; k < j->n && h && msave_gen == gen; k++) {
    uint8_t *b = NULL; size_t n = 0;
    int ok = mg_fetch_page(h, j->src, j->urls[k], &msave_gen, gen, &b, &n, &err);
    if (!ok && msave_gen == gen) { free(b); ok = mg_fetch_page(h, j->src, j->urls[k], &msave_gen, gen, &b, &n, &err); } // (once more)
    const char *ext = ok ? pic_ext(b, n) : NULL;
    int w = 0, hh = 0;
    if (!ext || !lv_image_size(b, n, &w, &hh)) { free(b); msave_failed++; msave_done++; continue; }
    // the thumbnail: the page decoded straight to card size
    uint8_t *th = NULL; size_t tn = 0; int tw = 0, tht = 0;
    {
      LvPixels px;
      int cw, ch;
      cover_size(b, n, &cw, &ch);
      // (a tall page, a webtoon's: its top, in a page's shape, not a sliver of the whole strip)
      int ok = hh > w * 1.6f ? decode_exact(b, n, (int)(ch * 0.7f), ch, &px) : lv_decode(b, n, cw, ch, &px);
      if (ok == 0) { th = lv_encode_jpeg(&px, 85, &tn); tw = px.w; tht = px.h; free(px.rgba); }
    }
    MSaved d;
    memset(&d, 0, sizeof d);
    snprintf(d.name, sizeof d.name, "%.200s %03d.%s", j->coll, k + 1, ext);
    snprintf(d.kind, sizeof d.kind, "%s", strcmp(ext, "gif") ? "image" : "gif");
    now_iso(d.when, sizeof d.when);
    LvNew nw = {.name = d.name, .category = "Comics", .kind = d.kind, .tags = j->tags, .source = j->urls[k], .added = d.when,
                .nsfw = 9, .quality = 0, .width = w, .height = hh, .collection = j->coll};
    saver_flush(); // (a rewrite of index.csv waiting to be written goes first: this appends to it)
    pthread_mutex_lock(&vmx);
    const char *why = NULL;
    int r = -1;
    if (loaded_vault && vgen == j->vgen) {
      long long room = free_room(vault.root);
      if (room >= 0 && room < (long long)n + 4 * 1024 * 1024) why = "Not enough room on the memory card";
      else r = lv_vault_add_files(&vault, &nw, b, n, ext, th, tn, tw, tht, &d.added, &why);
    } else why = "Another vault was opened";
    pthread_mutex_unlock(&vmx);
    free(b); free(th);
    if (r != 0) {
      msave_failed += j->n - k; msave_done = j->n;
      snprintf(msave_title, sizeof msave_title, "%s", why ? why : "It couldn't be saved");
      stop = 1; // (no room, another vault: the rest of a series too)
      break;
    }
    d.nw = nw; d.vgen = j->vgen;
    snprintf(d.coll, sizeof d.coll, "%s", j->coll);
    pthread_mutex_lock(&msmx);
    if (nmsaved == capmsaved) { MSaved *nm = realloc(msaved, (capmsaved + 16) * sizeof *nm); if (nm) { msaved = nm; capmsaved += 16; } }
    if (nmsaved < capmsaved) msaved[nmsaved++] = d;
    pthread_mutex_unlock(&msmx);
    msave_done++;
  }
  } // (the chapters)
  if (!h) msave_failed = msave_total;
  lv_http_free(h);
  lv_strings_free(j->urls, j->n);
  free(j->chaps);
  free(j);
  msave_on = 0;
  return NULL;
}
// (the drawing thread: the pages written, into the loaded vault; then a word when it's all done)
static void msave_take(void) {
  static int was_on;
  if (!was_on && !msave_on) return;
  pthread_mutex_lock(&msmx);
  int n = nmsaved;
  MSaved *got = n ? malloc(n * sizeof *got) : NULL;
  if (got) { memcpy(got, msaved, n * sizeof *got); nmsaved = 0; } else n = 0;
  pthread_mutex_unlock(&msmx);
  for (int k = 0; k < n; k++) {
    MSaved *d = &got[k];
    if (d->vgen != vgen || !loaded_vault) continue;
    d->nw.name = d->name; d->nw.kind = d->kind; d->nw.added = d->when; d->nw.category = "Comics";
    d->nw.tags = msave_tags; d->nw.source = ""; d->nw.collection = d->coll;
    int before = vault.n;
    pthread_mutex_lock(&vmx);
    int ok = lv_vault_add_memory(&vault, &d->nw, &d->added) == 0;
    pthread_mutex_unlock(&vmx);
    if (ok) library_grew(before);
  }
  free(got);
  if (was_on && !msave_on) {
    char m[300];
    int okn = msave_done - msave_failed;
    if (msave_series_n) { sfx(SFX_SAVED); snprintf(m, sizeof m, "%d chapters saved: Library \u203a Comics%s", msave_series_at, msave_title[0] ? " (it stopped: no room)" : ""); msave_series_n = 0; }
    else if (okn <= 0) { sfx(SFX_ERROR); snprintf(m, sizeof m, "Not saved: %s", msave_title[0] ? msave_title : "the pages didn't come"); }
    else { sfx(SFX_SAVED); snprintf(m, sizeof m, "%d pages saved: Library \u203a Comics%s", okn, msave_failed ? " (some pages didn't come)" : ""); }
    say(m);
  }
  was_on = msave_on;
}
// The vault's tags from the book's ("Big Breasts|female:sole female" → big_breasts female:sole_female)
static void book_tags(const char *in, char *out, size_t cap) {
  size_t o = 0;
  for (const char *p = in; *p && o + 2 < cap; p++) {
    char c = *p == '|' ? ' ' : *p == ' ' ? '_' : (char)tolower((unsigned char)*p);
    if (c == ',' || c == '"') continue;
    out[o++] = c;
  }
  out[o] = 0;
  snprintf(out + o, cap - o, "%scomic", o ? " " : "");
}
static void msave_start(void) {
  if (msave_on) { say("A chapter is being saved already"); return; }
  if (!loaded_vault) { sfx(SFX_ERROR); say("Open a vault first: the pages go into it"); return; }
  if (!mg.npages || mg.book < 0) { sfx(SFX_ERROR); say("The pages are still coming"); return; }
  MSave *j = calloc(1, sizeof *j);
  if (!j) return;
  j->src = mg.src; j->vgen = vgen; j->book = mg.books[mg.book];
  j->total = mg.pages_total;
  j->urls = calloc(mg.npages, sizeof *j->urls);
  if (!j->urls) { free(j); return; }
  for (int k = 0; k < mg.npages; k++) { j->urls[k] = strdup(mg.pages[k]); if (j->urls[k]) j->n++; else break; }
  if (mg_chapters(mg.src) && mg.ch && mg.chapter < mg.nch) snprintf(j->coll, sizeof j->coll, "%.150s \u2013 %.40s", j->book.title, mg.ch[mg.chapter].label);
  else snprintf(j->coll, sizeof j->coll, "%.190s", j->book.title);
  coll_clean(j->coll);
  for (int i = 0; i < vault.n; i++)
    if (vault.items[i].collection && !strcmp(vault.items[i].collection, j->coll)) {
      lv_strings_free(j->urls, j->n); free(j);
      sfx(SFX_ERROR); say("It's in the vault already (Library \u203a Comics)");
      return;
    }
  book_tags(j->book.tags, j->tags, sizeof j->tags);
  snprintf(msave_tags, sizeof msave_tags, "%s", j->tags);
  snprintf(msave_coll, sizeof msave_coll, "%s", j->coll);
  msave_done = msave_failed = 0; msave_total = j->src == MS_EHENTAI && j->total ? j->total : j->n;
  msave_title[0] = 0;
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024); // (it decodes the thumbnails)
  msave_on = 1;
  if (pthread_create(&t, &at, msave_main, j) != 0) { msave_on = 0; lv_strings_free(j->urls, j->n); free(j); sfx(SFX_ERROR); say("It couldn't start"); return; }
  pthread_detach(t);
  sfx(SFX_OPEN);
  char m[260];
  snprintf(m, sizeof m, "Saving \u201c%.200s\u201d into the vault", j->coll);
  say(m);
}

// A whole series: every chapter of the book open that isn't in the vault yet, one after another, each a
// comic of its own ("Title – Chapter 3"), on the same thread as a chapter's save
static void msave_series_start(void) {
  if (msave_on) { say("Something's being saved already"); return; }
  if (!loaded_vault) { sfx(SFX_ERROR); say("Open a vault first: the pages go into it"); return; }
  if (mg.book < 0 || !mg.nch || !mg.ch) { sfx(SFX_ERROR); say("The chapters are still coming"); return; }
  MSave *j = calloc(1, sizeof *j);
  if (!j || !(j->chaps = calloc(mg.nch, sizeof *j->chaps))) { if (j) free(j); return; }
  j->src = mg.src; j->vgen = vgen; j->book = mg.books[mg.book];
  for (int c = 0; c < mg.nch; c++) {
    char coll[200];
    snprintf(coll, sizeof coll, "%.150s \u2013 %.40s", j->book.title, mg.ch[c].label);
    coll_clean(coll);
    int have = 0;
    for (int i = 0; i < vault.n && !have; i++) have = vault.items[i].collection && !strcmp(vault.items[i].collection, coll);
    if (have) continue;
    snprintf(j->chaps[j->nchaps].id, sizeof j->chaps[0].id, "%s", mg.ch[c].id);
    snprintf(j->chaps[j->nchaps].label, sizeof j->chaps[0].label, "%s", mg.ch[c].label);
    j->nchaps++;
  }
  if (!j->nchaps) { free(j->chaps); free(j); sfx(SFX_ERROR); say("Every chapter is in the vault already"); return; }
  book_tags(j->book.tags, j->tags, sizeof j->tags);
  snprintf(msave_tags, sizeof msave_tags, "%s", j->tags);
  msave_done = msave_failed = 0; msave_total = 0; msave_title[0] = 0;
  msave_series_n = j->nchaps; msave_series_at = 0;
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024);
  msave_on = 1;
  if (pthread_create(&t, &at, msave_main, j) != 0) { msave_on = 0; msave_series_n = 0; free(j->chaps); free(j); sfx(SFX_ERROR); say("It couldn't start"); return; }
  pthread_detach(t);
  sfx(SFX_OPEN);
  char m[260];
  snprintf(m, sizeof m, "Saving %d chapters of \u201c%.150s\u201d into the vault", j->nchaps, j->book.title);
  say(m);
}
static void mg_start_thread(void) {
  if (mg_thread_on) return;
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024); // (it decodes pages)
  mg_thread_on = pthread_create(&t, &at, mg_main, NULL) == 0;
}

// ---------- the lists ----------
static void mg_drop_textures(void) {
  for (int k = 0; k < MG_TEX; k++) { release_tex(mg.tex[k]); mg.tex[k] = NULL; mg.tex_book[k] = -1; }
  for (int k = 0; k < 3; k++) { release_tex(mg.ptex[k]); mg.ptex[k] = NULL; mg.ptex_page[k] = -1; }
  release_tex(mg.big_cover);
  mg.big_cover = NULL;
}
static int mg_english = 1; // (the Filter: English only, the word added to what's searched; on to begin with)
static void mg_list_more(void) {
  if (mg.loading || !mg.more) return;
  char url[1600];
  char keep[128]; snprintf(keep, sizeof keep, "%s", mg.query);
  if (mg_english && mg.src != MS_MANGADEX && !strstr(mg.query, "english")) snprintf(mg.query, sizeof mg.query, "%s%s%s", keep, keep[0] ? " " : "", mg.src == MS_EHENTAI ? "language:english" : "english");
  if (mg.src == MS_MANGADEX) lv_md_list_url(mg.query, 1 /* (always 18+) */, mg.order, mg.nbooks, url, sizeof url);
  else if (mg.src == MS_EHENTAI) lv_eh_list_url(mg.query, mg.total, url, sizeof url); // (E-Hentai: total counts the pages read)
  else lv_ms_list_url(mg.src, mg.query, mg.total + 1, url, sizeof url); // (the others too: pages from 1)
  snprintf(mg.query, sizeof mg.query, "%s", keep);
  mg.loading = 1;
  mg_push(MJ_LIST, 0, 0, url);
}
static int mg_book_back = S_MANGA;   // (where ○ on a book goes: the list, or the Favourites)
static int mg_offline_pending;       // (opened to be made available offline: its pages saved once here)
static int msave_from_book;          // (that save: shown as its own screen over the reader, closed when done)
static int fav_toggle_comic(const LvBook *b, int src);
static int fav_find(int kind, const char *page);
// (the list let go, nothing asked for: a book from elsewhere is about to be put in it)
static void mg_search_cancel(void) {
  pthread_mutex_lock(&mgmx);
  mg_gen++;
  nmjobs = 0;
  for (int k = 0; k < nmdone; k++) { free(mdone[k].px.rgba); free(mdone[k].books); free(mdone[k].ch); lv_strings_free(mdone[k].urls, mdone[k].n); }
  nmdone = 0;
  pthread_mutex_unlock(&mgmx);
  mg_drop_textures();
  free(mg.books); mg.books = NULL;
  free(mg.cstate); mg.cstate = NULL;
  mg.nbooks = 0; mg.total = 0; mg.more = 0; mg.loading = 0; mg.err[0] = 0; mg.sel = 0; mg.scroll = 0;
}
static void mg_search(void) {
  pthread_mutex_lock(&mgmx);
  mg_gen++;
  nmjobs = 0;
  for (int k = 0; k < nmdone; k++) { free(mdone[k].px.rgba); free(mdone[k].books); free(mdone[k].ch); lv_strings_free(mdone[k].urls, mdone[k].n); }
  nmdone = 0;
  pthread_mutex_unlock(&mgmx);
  mg_drop_textures();
  free(mg.books); mg.books = NULL;
  free(mg.cstate); mg.cstate = NULL;
  mg.nbooks = 0; mg.total = 0; mg.more = 1; mg.loading = 0; mg.err[0] = 0; mg.sel = 0; mg.scroll = 0;
  mg_list_more();
}
static void manga_open(int src) {
  lv_net_warm(); // (on a thread of its own; the rest on the worker that fetches)
  mg_start_thread();
  if (src != mg.src || !mg.books) { mg.src = src; mg_search(); }
  manga_back = screen == S_MANGA ? manga_back : S_WEB;
  screen = S_MANGA;
}
static void mg_stage_drop(void);
static void mg_free_pages(void) {
  mg_stage_drop();
  lv_strings_free(mg.pages, mg.npages);
  mg.pages = NULL;
  mg.npages = 0;
  for (int k = 0; k < 3; k++) { release_tex(mg.ptex[k]); mg.ptex[k] = NULL; mg.ptex_page[k] = -1; }
}
static void mg_open_book(int k) {
  mg.book = k;
  free(mg.ch); mg.ch = NULL;
  mg.nch = 0; mg.ch_sel = 0; mg.ch_scroll = 0;
  release_tex(mg.big_cover); mg.big_cover = NULL;
  mg_push(MJ_BIGCOVER, k, 0, mg.books[k].cover);
  if (mg.src == MS_MANGADEX) {
    char url[600];
    lv_md_feed_url(mg.books[k].id, 1 /* (always 18+) */, 0, url, sizeof url);
    mg.ch_loading = 1;
    mg_push(MJ_CHAPTERS, k, 0, url);
  } else if (mg_chapters(mg.src)) {
    mg.ch_loading = 1;
    mg_push(MJ_CHAPTERS, k, 0, mg.books[k].id); // (the book's path: the worker makes the address)
  } else mg.ch_loading = 0;
  screen = S_BOOK;
}
static void mg_read(int chapter) {
  mg_free_pages();
  mg.chapter = chapter;
  mg.page = 0;
  mg.zoom = 1; mg.pan_x = mg.pan_y = 0;
  mg.tall_top = 1;
  mg.gallery_p = 0;
  mg.pages_loading = 1;
  char url[600];
  if (mg.src == MS_MANGADEX) { lv_md_pages_url(mg.ch[chapter].id, url, sizeof url); mg.pages_total = mg.ch[chapter].pages; }
  else if (mg.src == MS_EHENTAI) { lv_eh_gallery_url(&mg.books[mg.book], 0, url, sizeof url); mg.pages_total = mg.books[mg.book].pages; }
  else {
    // the others: a gallery's page, HentaiHand's API, a series' chapter or a single gallery's page
    const LvMSrc *ms = &LV_MSRC[mg.src];
    if (ms->family == MF_HHAND) snprintf(url, sizeof url, "%s/api/comics/%s/images", ms->base, mg.books[mg.book].id);
    else lv_ms_url(mg.src, mg_chapters(mg.src) && mg.ch && chapter < mg.nch ? mg.ch[chapter].id : mg.books[mg.book].id, url, sizeof url);
    mg.pages_total = 0; // (counted when they come)
  }
  mg_push(MJ_PAGES, mg.book, chapter, url);
  screen = S_READER;
}
// the page wanted at slot k (0 shown, 1-2 next), asked for if it isn't here
// The next strip, cut ahead: while the pages of this one are read, the next one (a few pages on) is
// downloaded and cut on a thread of its own, so turning onto it doesn't wait seconds for the cut
static char strip_pf_url[640];
static volatile int strip_pf_busy;
static void *strip_pf_main(void *arg) {
  char *url = arg;
  char base[700];
  int k, rows, total;
  if (lv_band_of(url, base, sizeof base, &k, &rows, &total)) {
    LvHttp *h = lv_http_new();
    uint8_t *b = NULL; size_t n = 0; const char *err = NULL;
    if (h && mg_fetch_band(h, base, k, rows, total, &b, &n, &err)) free(b);
    LOG("strip cut ahead: %s\n", base);
    lv_http_free(h);
  }
  free(url);
  strip_pf_busy = 0;
  return NULL;
}
static void strip_prefetch(int page) {
  if (strip_pf_busy || page < 0 || page >= mg.npages) return;
  char cur[700], next[700];
  int k, rows, total;
  if (!lv_band_of(mg.pages[page], cur, sizeof cur, &k, &rows, &total)) return;
  for (int q = page + 1; q < mg.npages && q <= page + 40; q++) { // (from the strip's first page on: the next one is ready long before it's reached)
    if (!lv_band_of(mg.pages[q], next, sizeof next, &k, &rows, &total) || !strcmp(next, cur)) continue;
    if (!strcmp(next, strip_pf_url)) return; // (asked already)
    uint8_t *b = NULL; size_t n = 0;
    if (strip_copy(next, 0, &b, &n)) { free(b); return; } // (cut already)
    snprintf(strip_pf_url, sizeof strip_pf_url, "%s", next);
    char *url = strdup(mg.pages[q]);
    if (!url) return;
    strip_pf_busy = 1;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    if (pthread_create(&t, &at, strip_pf_main, url) == 0) pthread_detach(t);
    else { free(url); strip_pf_busy = 0; }
    return;
  }
}
// Pages on their way into their textures, a slice of rows a frame (about 2 ms of copying in all);
// each goes into its slot (0 the page shown, 1-2 the next) once it's whole
static struct { LvPixels px; vita2d_texture *t; int row, page, on; } mg_stage[3];
static int mg_stage_add(LvPixels *px, int page) {
  for (int k = 0; k < 3; k++) {
    if (mg_stage[k].on) continue;
    vita2d_texture *t = tex_new(px->w, px->h);
    if (!t) return 0;
    mg_stage[k].px = *px; mg_stage[k].t = t; mg_stage[k].row = 0; mg_stage[k].page = page; mg_stage[k].on = 1;
    return 1;
  }
  return 0;
}
static void mg_stage_drop(void) {
  for (int k = 0; k < 3; k++) if (mg_stage[k].on) { free(mg_stage[k].px.rgba); release_tex(mg_stage[k].t); mg_stage[k].on = 0; }
}
static void mg_stage_step(void) {
  uint64_t t0 = sceKernelGetProcessTimeWide();
  for (int k = 0; k < 3; k++) {
    if (!mg_stage[k].on) continue;
    LvPixels *px = &mg_stage[k].px;
    uint8_t *d = vita2d_texture_get_datap(mg_stage[k].t);
    unsigned stride = vita2d_texture_get_stride(mg_stage[k].t);
    while (mg_stage[k].row < px->h) {
      int end = mg_stage[k].row + 32 < px->h ? mg_stage[k].row + 32 : px->h;
      for (int y = mg_stage[k].row; y < end; y++) memcpy(d + (size_t)y * stride, px->rgba + (size_t)y * px->w * 4, (size_t)px->w * 4);
      mg_stage[k].row = end;
      if (sceKernelGetProcessTimeWide() - t0 > 2000) break; // (this frame's share done)
    }
    if (mg_stage[k].row < px->h) return;
    // (whole: into its slot, if it's still one of the three wanted)
    vita2d_texture_set_filters(mg_stage[k].t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    int page = mg_stage[k].page;
    if (page >= mg.page && page <= mg.page + 2) {
      int slot = page - mg.page;
      release_tex(mg.ptex[slot]);
      mg.ptex[slot] = mg_stage[k].t;
      mg.ptex_page[slot] = page;
    } else release_tex(mg_stage[k].t);
    free(px->rgba);
    mg_stage[k].on = 0;
    if (sceKernelGetProcessTimeWide() - t0 > 2000) return;
  }
}
static void mg_want_page(int page, int hi) {
  if (page < 0 || page >= mg.npages) return;
  strip_prefetch(page);
  for (int k = 0; k < 3; k++) if (mg_stage[k].on && mg_stage[k].page == page && !hi) return; // (on its way in)
  for (int k = 0; k < 3; k++) if (mg.ptex_page[k] == page && !hi) return;
  mg_push(MJ_PAGE, page, hi, mg.pages[page]);
}
static void mg_take(void) {
  mg_stage_step(); // (pages on their way into their textures: this frame's slice)
  if (!nmdone) return; // (nothing come: no lock)
  pthread_mutex_lock(&mgmx);
  int n = nmdone < 6 ? nmdone : 6;
  MDone got[16];
  memcpy(got, mdone, n * sizeof *got);
  memmove(mdone, mdone + n, (nmdone - n) * sizeof *mdone);
  nmdone -= n;
  if (n) pthread_cond_broadcast(&mgcv); // (room again: a worker waiting to hand one over carries on)
  pthread_mutex_unlock(&mgmx);
  for (int k = 0; k < n; k++) {
    MDone *d = &got[k];
    // (this frame's done enough: the rest back at the front, for the next)
    if (k > 0 && frame_busy()) {
      pthread_mutex_lock(&mgmx);
      int rest = n - k;
      if (nmdone + rest <= 16) { memmove(mdone + rest, mdone, nmdone * sizeof *mdone); memcpy(mdone, got + k, rest * sizeof *got); nmdone += rest; pthread_mutex_unlock(&mgmx); break; }
      pthread_mutex_unlock(&mgmx); // (no room to put them back: done now after all)
    }
    if (d->gen != mg_gen) { free(d->px.rgba); free(d->books); free(d->ch); lv_strings_free(d->urls, d->n); continue; }
    switch (d->kind) {
      case MJ_LIST:
        mg.loading = 0;
        if (d->n < 0) { snprintf(mg.err, sizeof mg.err, "%s", d->err); mg.more = 0; }
        else {
          LvBook *nb = realloc(mg.books, (mg.nbooks + d->n + 1) * sizeof *nb);
          signed char *nc = realloc(mg.cstate, mg.nbooks + d->n + 1);
          if (nb) mg.books = nb;
          if (nc) mg.cstate = nc;
          if (nb && nc) {
            int fresh = 0;
            for (int i = 0; i < d->n && mg.nbooks < MG_MAX; i++) {
              int dup = 0; // (already in the list: a site that hands its last page out again past the end)
              for (int j = mg.nbooks - 1; j >= 0 && j >= mg.nbooks - 400 && !dup; j--) dup = !strcmp(mg.books[j].id, d->books[i].id);
              if (dup) continue;
              mg.books[mg.nbooks] = d->books[i]; mg.cstate[mg.nbooks++] = 0; fresh++;
            }
            if (!fresh) d->n = 0; // (nothing new: treated as the end below)
            if (mg.src == MS_MANGADEX) mg.more = d->n > 0 && mg.nbooks < d->total && mg.nbooks < MG_MAX;
            else {
              mg.total++;
              mg.more = d->n > 0 && mg.nbooks < MG_MAX;
              // (a source whose list or search is one page only)
              const LvMSrc *ms = &LV_MSRC[mg.src];
              if (mg.src != MS_EHENTAI && (mg.query[0] ? !strstr(ms->search, "%d") : !ms->list)) mg.more = 0;
              // (a page that brought nothing new: the site showing its first page again past its end)
              if (mg.src != MS_EHENTAI && d->n > 0 && mg.nbooks >= d->n) {
                int fresh = 0;
                for (int i = 0; i < d->n && !fresh; i++) { fresh = 1; for (int q = 0; q < mg.nbooks - d->n && fresh; q++) if (!strcmp(mg.books[q].id, d->books[i].id)) fresh = 0; }
                if (!fresh) mg.more = 0;
              }
            }
          }
        }
        free(d->books);
        break;
      case MJ_COVER: {
        if (!mg.cstate || d->a >= mg.nbooks) { free(d->px.rgba); break; }
        if (!d->px.rgba) { mg.cstate[d->a] = 3; break; }
        int best = 0;
        for (int s = 1; s < MG_TEX; s++) if (mg.tex_used[s] < mg.tex_used[best]) best = s;
        if (mg.tex[best] && mg.tex_book[best] >= 0 && mg.tex_book[best] < mg.nbooks) mg.cstate[mg.tex_book[best]] = 0;
        release_tex(mg.tex[best]);
        mg.tex[best] = pool_put(&pool_covers, &d->px);
        mg.tex_book[best] = d->a;
        mg.tex_used[best] = frame_no;
        mg.cstate[d->a] = mg.tex[best] ? 2 : 3;
        free(d->px.rgba);
        break;
      }
      case MJ_BIGCOVER:
        if (d->a == mg.book && d->px.rgba) { release_tex(mg.big_cover); mg.big_cover = upload(&d->px); }
        free(d->px.rgba);
        break;
      case MJ_CHAPTERS:
        mg.ch_loading = 0;
        if (d->a == mg.book) {
          free(mg.ch); mg.ch = d->ch; mg.nch = d->n < 0 ? 0 : d->n;
          if (d->n < 0) snprintf(mg.err, sizeof mg.err, "%s", d->err);
          else if (d->n == 0 && d->b == 0 && mg.src == MS_MANGADEX && d->a >= 0 && d->a < mg.nbooks) {
            // (no English chapter with its pages on MangaDex: every language's, then)
            char url[600];
            lv_md_feed_url_lang(mg.books[d->a].id, 1 /* (always 18+) */, 0, 1, url, sizeof url);
            mg.ch_loading = 1;
            mg_push(MJ_CHAPTERS, d->a, 1, url);
          }
        }
        else free(d->ch);
        break;
      case MJ_PAGES:
        if (d->a != mg.book || d->b != mg.chapter || screen != S_READER) { lv_strings_free(d->urls, d->n); break; }
        mg.pages_loading = 0;
        if (d->n <= 0) { snprintf(mg.err, sizeof mg.err, "%s", d->n < 0 ? d->err : "No pages"); lv_strings_free(d->urls, d->n); break; }
        // (E-Hentai: a gallery page adds its share of the picture pages)
        {
          char **np = realloc(mg.pages, (mg.npages + d->n) * sizeof *np);
          if (np) { mg.pages = np; memcpy(mg.pages + mg.npages, d->urls, d->n * sizeof *np); mg.npages += d->n; free(d->urls); }
          else lv_strings_free(d->urls, d->n);
        }
        mg.gallery_p++;
        // (opened to be made available offline: saved now there's a list of pages)
        if (mg_offline_pending && mg.npages) { mg_offline_pending = 0; msave_from_book = 1; msave_start(); }
        break;
      case MJ_PAGE:
        // (into its texture a slice a frame: a page is up to 15 MB, copied in one frame it was a stutter)
        if (d->px.rgba && d->a >= mg.page && d->a <= mg.page + 2 && mg_stage_add(&d->px, d->a)) d->px.rgba = NULL;
        else if (d->px.rgba && d->a >= mg.page && d->a <= mg.page + 2) {
          int slot = d->a - mg.page;
          release_tex(mg.ptex[slot]);
          mg.ptex[slot] = upload(&d->px);
          mg.ptex_page[slot] = d->a;
        }
        free(d->px.rgba);
        break;
    }
  }
}

// ---------- the covers ----------
static vita2d_texture *mg_cover(int i) {
  for (int s = 0; s < MG_TEX; s++) if (mg.tex[s] && mg.tex_book[s] == i) { mg.tex_used[s] = frame_no; return mg.tex[s]; }
  return NULL;
}
// ---------- where each book was left: picked up again (ux0:data/LWDV00001/comic_progress.tsv) ----------
// A line each, the most recent first: the source, the book (its address, title, cover), the chapter
// (its address and label) and the page. Kept as reading goes, written a second after the last change.
typedef struct { char src[32], id[200], title[160], cover[400], chid[200], chlabel[80]; int page; long long at; } Prog;
#define PROG_MAX 60
static Prog *prog;
static int nprog, prog_loaded;
static unsigned prog_dirty_at;
static int mg_resume_page = -1; // (a chapter opened to pick up from here: its page, once its pages are in)
static void prog_field(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src); for (char *p = dst; *p; p++) if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' '; }
static void prog_load(void) {
  if (prog_loaded) return;
  prog_loaded = 1;
  prog = calloc(PROG_MAX, sizeof *prog);
  FILE *f = fopen(DATA "/comic_progress.tsv", "r");
  if (!f || !prog) { if (f) fclose(f); return; }
  char line[1400];
  while (nprog < PROG_MAX && fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    char *c[8] = {0};
    int k = 0;
    for (char *p = line; k < 8; k++) { c[k] = p; p = strchr(p, '\t'); if (!p) { k++; break; } *p++ = 0; }
    if (k < 8) continue;
    Prog *g = &prog[nprog++];
    snprintf(g->src, sizeof g->src, "%s", c[0]); snprintf(g->id, sizeof g->id, "%s", c[1]); snprintf(g->title, sizeof g->title, "%s", c[2]);
    snprintf(g->cover, sizeof g->cover, "%s", c[3]); snprintf(g->chid, sizeof g->chid, "%s", c[4]); snprintf(g->chlabel, sizeof g->chlabel, "%s", c[5]);
    g->page = atoi(c[6]); g->at = atoll(c[7]);
  }
  fclose(f);
}
static void prog_write(void) {
  prog_dirty_at = 0;
  FILE *f = fopen(DATA "/comic_progress.tsv.part", "w");
  if (!f) return;
  for (int k = 0; k < nprog; k++) fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\t%d\t%lld\n", prog[k].src, prog[k].id, prog[k].title, prog[k].cover, prog[k].chid, prog[k].chlabel, prog[k].page, prog[k].at);
  fclose(f);
  sceIoRemove(DATA "/comic_progress.tsv");
  sceIoRename(DATA "/comic_progress.tsv.part", DATA "/comic_progress.tsv");
}
static int prog_find(const char *src, const char *id) {
  prog_load();
  for (int k = 0; k < nprog; k++) if (!strcmp(prog[k].src, src) && !strcmp(prog[k].id, id)) return k;
  return -1;
}
// (the reader's page: noted for its book, which goes to the front)
static void prog_note(void) {
  if (mg.book < 0 || mg.book >= mg.nbooks || !prog_loaded && (prog_load(), !prog)) return;
  if (!prog) return;
  LvBook *b = &mg.books[mg.book];
  int k = prog_find(LV_MSRC[mg.src].name, b->id);
  Prog g;
  memset(&g, 0, sizeof g);
  if (k >= 0) g = prog[k];
  prog_field(g.src, sizeof g.src, LV_MSRC[mg.src].name); prog_field(g.id, sizeof g.id, b->id);
  prog_field(g.title, sizeof g.title, b->title); prog_field(g.cover, sizeof g.cover, b->cover);
  if (mg_chapters(mg.src) && mg.ch && mg.chapter < mg.nch) { prog_field(g.chid, sizeof g.chid, mg.ch[mg.chapter].id); prog_field(g.chlabel, sizeof g.chlabel, mg.ch[mg.chapter].label); }
  else { g.chid[0] = 0; g.chlabel[0] = 0; }
  g.page = mg.page; g.at = (long long)time(NULL);
  if (k < 0) { k = nprog < PROG_MAX ? nprog++ : PROG_MAX - 1; }
  memmove(prog + 1, prog, k * sizeof *prog); // (to the front)
  prog[0] = g;
  prog_dirty_at = frame_no ? frame_no : 1;
}
// (a second after the last change, or at once when the app is put away: main.c's flush_settings)
static void prog_flush(int now) { if (prog_dirty_at && (now || frame_no - prog_dirty_at > 60)) prog_write(); }
// A book picked up from the Continue row: its source opened, the book put first in its list and opened
static void prog_open(int k) {
  if (k < 0 || k >= nprog) return;
  Prog g = prog[k];
  int src = -1;
  for (int i = 0; i < MS_N && src < 0; i++) if (!strcmp(LV_MSRC[i].name, g.src)) src = i;
  if (src < 0) { say("That source isn't here any more"); return; }
  if (src != mg.src || !mg.books) { mg.src = src; mg_search(); }
  if (!mg.books) { mg.books = calloc(MG_MAX, sizeof *mg.books); mg.cstate = calloc(MG_MAX, 1); }
  if (!mg.books || !mg.cstate || mg.nbooks >= MG_MAX) return;
  memmove(mg.books + 1, mg.books, mg.nbooks * sizeof *mg.books);
  memmove(mg.cstate + 1, mg.cstate, mg.nbooks);
  memset(&mg.books[0], 0, sizeof mg.books[0]);
  snprintf(mg.books[0].id, sizeof mg.books[0].id, "%s", g.id);
  snprintf(mg.books[0].title, sizeof mg.books[0].title, "%s", g.title);
  snprintf(mg.books[0].cover, sizeof mg.books[0].cover, "%s", g.cover);
  mg.cstate[0] = 0; mg.nbooks++;
  for (int t = 0; t < MG_TEX; t++) if (mg.tex[t] && mg.tex_book[t] >= 0) mg.tex_book[t]++; // (the covers follow their books)
  mg.sel = 0;
  sfx(SFX_SELECT);
  mg_book_back = S_MANGA;
  mg_open_book(0);
}
// From the book screen: its chapter (once the chapters are in), from its page
static int prog_continue(int k) {
  if (k < 0) return 0;
  if (mg_chapters(mg.src)) {
    if (!mg.nch) return 0;
    int c = -1;
    for (int i = 0; i < mg.nch && c < 0; i++) if (!strcmp(mg.ch[i].id, prog[k].chid)) c = i;
    if (c < 0) return 0;
    mg.ch_sel = c;
    mg_resume_page = prog[k].page;
    mg_read(c);
  } else { mg_resume_page = prog[k].page; mg_read(0); }
  return 1;
}

static void draw_manga(void) {
  mg_take();
  // (the background: the frame is cleared to it already)
  const int cols = 6, gap = 12, x0 = (W - cols * MG_CW - (cols - 1) * gap) / 2;
  int rows = (mg.nbooks + cols - 1) / cols;
  // the Continue row: the books left part way, from any source (not while searching)
  prog_load();
  int ncont = !mg.query[0] ? (nprog < 8 ? nprog : 8) : 0, cont_h = ncont ? 52 : 0;
  float content = rows * (MG_CH + 52) + 20 + cont_h, view_h = H - TOP - BAR + 40;
  float max_scroll = fmaxf(0, content - view_h);
  // moving: touch, the stick, the D-pad
  if (in.touching && in.ty > TOP) mg.scroll -= in.drag_dy;
  mg.scroll -= in.bdy * 1.5f;
  in.pressed |= stick_dirs(); // (the left stick moves the selection, as the D-pad)
  if (mg.sel >= 0 && mg.nbooks && (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_LEFT | SCE_CTRL_DOWN | SCE_CTRL_UP))) {
    // (scrolled away from it: the first cover in sight is where the D-pad starts)
    float y = (mg.sel / cols) * (MG_CH + 52) + cont_h;
    if (y + MG_CH < mg.scroll || y > mg.scroll + view_h - 40) {
      int row = (int)((mg.scroll - cont_h + MG_CH + 51) / (MG_CH + 52));
      if (row < 0) row = 0;
      int v = row * cols;
      if (v >= mg.nbooks) v = mg.nbooks - 1;
      mg.sel = v; in.pressed &= ~(SCE_CTRL_RIGHT | SCE_CTRL_LEFT | SCE_CTRL_DOWN | SCE_CTRL_UP); sfx(SFX_TICK);
    }
  }
  int before = mg.sel;
  if (mg.sel < 0) { // (in the Continue row: -1 its first)
    int c = -1 - mg.sel;
    if (in.pressed & SCE_CTRL_RIGHT) c = c + 1 < ncont ? c + 1 : c;
    if (in.pressed & SCE_CTRL_LEFT) c = c > 0 ? c - 1 : 0;
    mg.sel = -1 - c;
    if (in.pressed & SCE_CTRL_DOWN) mg.sel = 0;
    if (!ncont) mg.sel = 0;
  } else {
    if (in.pressed & SCE_CTRL_RIGHT) mg.sel++;
    if (in.pressed & SCE_CTRL_LEFT) mg.sel--;
    if (in.pressed & SCE_CTRL_DOWN) mg.sel += cols;
    if ((in.pressed & SCE_CTRL_UP) && mg.sel < cols && ncont) mg.sel = -1;
    else if (in.pressed & SCE_CTRL_UP) mg.sel -= cols;
    if (mg.sel >= mg.nbooks) mg.sel = mg.nbooks - 1;
    if (mg.sel < 0 && !(ncont && mg.sel == -1 && (in.pressed & SCE_CTRL_UP))) mg.sel = 0;
  }
  if (mg.sel != before && mg.sel < 0) { sfx(SFX_TICK); mg.scroll = 0; }
  else if (mg.sel != before) {
    sfx(SFX_TICK);
    float y = (mg.sel / cols) * (MG_CH + 52) + cont_h;
    if (y < mg.scroll) mg.scroll = y;
    if (y + MG_CH + 52 > mg.scroll + view_h) mg.scroll = y + MG_CH + 52 - view_h;
  }
  if (mg.scroll > max_scroll) mg.scroll = max_scroll;
  if (mg.scroll < 0) mg.scroll = 0;
  int last = 0;
  if (ncont) {
    float cy = TOP + 8 - mg.scroll, cx = x0;
    if (cy > TOP - 48) {
      text(x0, cy + 14, FAINT, 13, "Continue reading");
      cx = x0 + text_w(13, "Continue reading") + 14;
    }
    for (int k = 0; k < ncont && cy > TOP - 48; k++) {
      char l[200];
      if (prog[k].chlabel[0]) snprintf(l, sizeof l, "%.60s \u00b7 %.30s", prog[k].title, prog[k].chlabel);
      else snprintf(l, sizeof l, "%.60s \u00b7 page %d", prog[k].title, prog[k].page + 1);
      int cw = text_w(14, l) + 30;
      if (cw > 280) cw = 280;
      if (cx + cw > W - 10) break;
      int on = mg.sel == -1 - k;
      if (on) glass(cx, cy, cw, 34, 0); else rrect(cx, cy, cw, 34, 12, SURF2);
      text_fit(cx + 14, cy + 23, on ? TEXT : DIM, 14, l, cw - 24);
      if (on) focus_ring(cx, cy, cw, 34);
      if (hit((int)cx, (int)cy, cw, 34) && in.tap_y > TOP) { prog_open(k); return; }
      cx += cw + 8;
    }
  }
  for (int i = 0; i < mg.nbooks; i++) {
    float x = x0 + (i % cols) * (MG_CW + gap), y = TOP + 10 + cont_h + (i / cols) * (MG_CH + 52) - mg.scroll;
    if (y > H) break;
    if (y + MG_CH + 52 < TOP) continue;
    last = i;
    int on = i == mg.sel && !in.touching;
    vita2d_texture *t = mg_cover(i);
    if (t) vita2d_draw_texture_scale(t, x, y, MG_CW / (float)vita2d_texture_get_width(t), MG_CH / (float)vita2d_texture_get_height(t));
    else {
      if (mg.cstate && mg.cstate[i] == 3) { rrect(x, y, MG_CW, MG_CH, 10, SURF); text_center(x + MG_CW / 2, y + MG_CH / 2, FAINT, 13, "No cover"); }
      else {
        skeleton(x, y, MG_CW, MG_CH, 10);
        spinner(x + MG_CW / 2, y + MG_CH / 2, 10);
        // (asked for once the list isn't moving fast: covers flown past aren't fetched for nothing)
        if (mg.cstate && mg.cstate[i] == 0 && mg.books[i].cover[0] && !(in.touching && abs(in.drag_dy) > 25)) { mg.cstate[i] = 1; mg_push(MJ_COVER, i, 0, mg.books[i].cover); }
      }
    }
    if (on) focus_ring(x, y, MG_CW, MG_CH);
    if (mg.books[i].pages) {
      char p[16];
      snprintf(p, sizeof p, "%dp", mg.books[i].pages);
      rrect(x + MG_CW - text_w(11, p) - 16, y + 6, text_w(11, p) + 10, 18, 9, RGBA(0, 0, 0, 160));
      text(x + MG_CW - text_w(11, p) - 11, y + 19, TEXT, 11, p);
    }
    text_fit(x + 2, y + MG_CH + 20, on ? TEXT : DIM, 14, mg.books[i].title, MG_CW - 4);
    if (hit((int)x, (int)y, MG_CW, MG_CH + 30) && in.tap_y > TOP && in.tap_y < H - BAR) { mg.sel = i; sfx(SFX_SELECT); mg_open_book(i); return; }
  }
  // more, before the end is reached
  if (last + cols * 4 >= mg.nbooks) mg_list_more(); // (asked for four rows ahead: never a wait at the end)
  if (!mg.nbooks) {
    if (mg.loading) spinner(W / 2, TOP + 150, 22);
    else text_center(W / 2, TOP + 150, DIM, 17, mg.err[0] ? mg.err : "Nothing found");
  } else if (mg.loading) spinner(W / 2, H - BAR - 20, 12);

  // the top: back, the source (its tab of the sites window), the search, the order
  vita2d_draw_rectangle(0, 0, W, TOP, with_alpha(BG, 250));
  // (no back button: this is the Browser while a comics source is chosen; the source chip switches)
  char sl[64];
  snprintf(sl, sizeof sl, "%s  ▾", LV_MSRC[mg.src].name);
  int sw = text_w(17, sl) + 36;
  rrect(16, 8, sw, 36, 18, ACC);
  text(34, 32, RGBA(20, 12, 16, 255), 17, sl);
  if (hit(16, 8, sw, 36) || (in.pressed & SCE_CTRL_START)) { sfx(SFX_OPEN); screen = S_WEB; sheet = SH_SITES; sites_tab = 2; sheet_sel = mg.src; sites_scroll = 0; return; }
  int kbw = kind_buttons(16 + sw + 10, 7, 2);
  if (screen != S_MANGA) return;
  int rx = W - 16, hb;
  if (mg.src == MS_MANGADEX) { // (the order: MangaDex's own)
    rx -= head_button(rx, "sort", MD_ORDER[mg.order], SURF2, &hb) + 10;
    if (hb || (in.pressed & SCE_CTRL_TRIANGLE)) { sfx(SFX_TOGGLE); mg.order = (mg.order + 1) % 3; mg_search(); }
  } else { // (the filter: English only, a glass button lit when on)
    const char *fl = mg_english ? "English" : "All languages";
    int fw = text_w(14, fl) + 46, fx = rx - fw;
    if (mg_english) { rrect(fx, 8, fw, 36, 18, with_alpha(ACC, 150)); rrect(fx + 2, 9, fw - 4, 14, 7, with_alpha(TEXT, 30)); } else glass(fx, 8, fw, 36, 0);
    icon("sliders", fx + 20, 26, TEXT);
    text(fx + 34, 31, TEXT, 14, fl);
    if (hit(fx, 4, fw, 44) || (in.pressed & SCE_CTRL_TRIANGLE)) { sfx(SFX_TOGGLE); mg_english = !mg_english; mg_search(); }
    rx = fx - 10;
  }
  { // the search, in the same row: the room between
    int qx = 16 + sw + 10 + kbw + 10, qw = rx - qx;
    if (qw > 60) {
      rrect(qx, 8, qw, 36, 18, SURF2);
      icon("search", qx + 20, 26, mg.query[0] ? TEXT : FAINT);
      text_fit(qx + 38, 32, mg.query[0] ? TEXT : FAINT, 15, mg.query[0] ? mg.query : "Search titles", qw - 48);
      if (hit(qx, 4, qw, 44) && !ime_open) ime_start(15, "Search", mg.query, 0);
    }
  }
  if ((in.pressed & SCE_CTRL_CROSS) && mg.sel < 0) { prog_open(-1 - mg.sel); return; }
  if ((in.pressed & SCE_CTRL_CROSS) && mg.nbooks) { sfx(SFX_SELECT); mg_open_book(mg.sel); return; }
  if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_BACK); screen = S_LIBRARY; return; } // (nothing behind it: the Library)
  draw_bar();
}

// ---------- a book ----------
static void draw_book(void) {
  mg_take();
  // (the background: the frame is cleared to it already)
  if (mg.book < 0 || mg.book >= mg.nbooks) { screen = S_MANGA; return; }
  LvBook *b = &mg.books[mg.book];
  // (behind it all: its cover, the whole screen, dim, a wash of its colours)
  if (mg.big_cover) { draw_cover(mg.big_cover, 0, 0, W, H, RGBA(255, 255, 255, 60)); vita2d_draw_rectangle(0, 0, W, H, with_alpha(BG, 170)); grad(0, 0, W, 120, with_alpha(BG, 255), 200, 0, 0); }
  // the cover on the left: rounded, lifted on a shadow, a glass rim
  const float cvx = 34, cvy = 64, cvw = 196, cvh = 278;
  rrect(cvx - 3, cvy + 5, cvw + 6, cvh + 4, 16, RGBA(0, 0, 0, 120));
  rrect(cvx - 3, cvy - 3, cvw + 6, cvh + 6, 16, with_alpha(TEXT, 40));
  if (mg.big_cover) draw_cover_round(mg.big_cover, cvx, cvy, cvw, cvh, 13, RGBA(255, 255, 255, 255));
  else { rrect(cvx, cvy, cvw, cvh, 13, SURF); spinner(cvx + cvw / 2, cvy + cvh / 2, 16); }
  { // the source and its kind, as a small glass pill, over the title
    char sk[80]; snprintf(sk, sizeof sk, "%s \u00b7 %s", LV_MSRC[mg.src].name, LV_MK_NAME[LV_MSRC[mg.src].kind]);
    int pw = text_w(12, sk) + 22;
    glass(276, 62, pw, 24, 0);
    text(287, 79, DIM, 12, sk);
  }
  text_fit(277, 112, TEXT, 24, b->title, W - 300);
  text_fit(276, 112, TEXT, 24, b->title, W - 300);
  char tags[400];
  snprintf(tags, sizeof tags, "%.390s", b->tags);
  for (char *p = tags; *p; p++) if (*p == '|') *p = ',';
  text_fit(276, 134, FAINT, 13, tags, W - 300);
  int listy = 152;
  // (left part way: picked up again)
  int pk = prog_find(LV_MSRC[mg.src].name, b->id);
  if (pk >= 0 && (prog[pk].page > 0 || prog[pk].chid[0]) && !mg.ch_loading) {
    char cl[160];
    if (prog[pk].chid[0]) snprintf(cl, sizeof cl, "\u25b6  Continue \u00b7 %.60s, page %d", prog[pk].chlabel, prog[pk].page + 1);
    else snprintf(cl, sizeof cl, "\u25b6  Continue from page %d", prog[pk].page + 1);
    int cw = text_w(16, cl) + 40;
    if (cw > W - 306) cw = W - 306;
    glass(276, listy - 4, cw, 42, 0);
    text_fit(296, listy + 22, TEXT, 16, cl, cw - 30);
    if (hit(276, listy - 4, cw, 42) && prog_continue(pk)) { sfx(SFX_SELECT); return; }
    if ((in.pressed & SCE_CTRL_RTRIGGER) && prog_continue(pk)) { sfx(SFX_SELECT); return; }
    text(276 + cw + 12, listy + 22, FAINT, 13, "R");
    listy += 50;
  }
  if (!mg_chapters(mg.src)) {
    char m[64];
    if (b->pages) snprintf(m, sizeof m, "%d pages", b->pages); else snprintf(m, sizeof m, "%s", LV_MK_NAME[LV_MSRC[mg.src].kind]);
    text(276, listy + 20, DIM, 16, m);
    rrect(276, listy + 40, 180, 46, 23, ACC);
    text_center(366, listy + 70, RGBA(20, 12, 16, 255), 18, "▶  Read");
    if (hit(276, listy + 40, 180, 46) || (in.pressed & SCE_CTRL_CROSS)) { sfx(SFX_SELECT); mg_read(0); return; }
  } else if (mg.ch_loading) spinner(560, 260, 20);
  else if (!mg.nch && mg.err[0]) text(276, listy + 30, ACC2, 16, mg.err);
  else if (!mg.nch) {
    // (nothing with its pages here, in any language: said, with the way on)
    rrect(276, listy + 6, W - 306, 170, 18, SURF);
    icon("book", 310, listy + 44, ACC);
    text(334, listy + 51, TEXT, 20, "No chapters to read here yet");
    text_fit(300, listy + 86, DIM, 15, "None of its chapters has its pages on this site, in any language.", W - 350);
    int next = mg.book + 1 < mg.nbooks ? mg.book + 1 : -1;
    int bx = 300, by = listy + 106;
    if (next >= 0) {
      int bw = text_w(17, "Next title") + 64;
      rrect(bx, by, bw, 48, 24, ACC);
      text(bx + 22, by + 31, RGBA(20, 12, 16, 255), 17, "Next title");
      icon("chev_r", bx + bw - 24, by + 24, RGBA(20, 12, 16, 255));
      if (hit(bx, by, bw, 48) || (in.pressed & SCE_CTRL_CROSS)) { sfx(SFX_SELECT); mg.sel = next; mg_open_book(next); return; }
      bx += bw + 12;
    }
    int lw = text_w(17, "Latest uploads") + 44;
    rrect(bx, by, lw, 48, 24, SURF2);
    text(bx + 22, by + 31, TEXT, 17, "Latest uploads");
    if (hit(bx, by, lw, 48) || (in.pressed & SCE_CTRL_TRIANGLE)) { sfx(SFX_TAB); mg.order = 1; mg_search(); screen = S_MANGA; return; }
    text(300, by + 76, FAINT, 13, next >= 0 ? "\u2715 next title   \u25b3 the latest uploads (scanlations, readable)   \u25cb back" : "\u25b3 the latest uploads (scanlations, readable)   \u25cb back");
  }
  else {
    const int rh = 48;
    float view = H - listy - 20;
    float want = mg.ch_sel * rh;
    if (want - mg.ch_scroll > view - rh) mg.ch_scroll = want - view + rh;
    if (want < mg.ch_scroll) mg.ch_scroll = want;
    if (in.touching && in.down_x > 260) mg.ch_scroll -= in.drag_dy;
    if (mg.ch_scroll < 0) mg.ch_scroll = 0;
    for (int k = 0; k < mg.nch; k++) {
      float y = listy + k * rh - mg.ch_scroll;
      if (y < listy - rh || y > H) continue;
      rrect(276, y, W - 306, rh - 6, 12, k == mg.ch_sel ? SURF2 : SURF);
      if (k == mg.ch_sel) rrect(276, y, 5, rh - 6, 3, ACC);
      text_fit(296, y + 28, TEXT, 16, mg.ch[k].label, W - 420);
      if (mg.ch[k].pages > 0) { // (the webtoon sites don't say: nothing shown, rather than "0 pages")
        char p[16];
        snprintf(p, sizeof p, "%d pages", mg.ch[k].pages);
        text(W - 46 - text_w(13, p), y + 28, FAINT, 13, p);
      }
      if (hit(276, (int)y, W - 306, rh - 6) && in.tap_y > listy) { mg.ch_sel = k; sfx(SFX_SELECT); mg_read(k); return; }
    }
    if (in.pressed & SCE_CTRL_DOWN) { mg.ch_sel = mg.ch_sel + 1 < mg.nch ? mg.ch_sel + 1 : mg.ch_sel; sfx(SFX_TICK); }
    if (in.pressed & SCE_CTRL_UP) { mg.ch_sel = mg.ch_sel > 0 ? mg.ch_sel - 1 : 0; sfx(SFX_TICK); }
    if (in.pressed & SCE_CTRL_CROSS) { sfx(SFX_SELECT); mg_read(mg.ch_sel); return; }
  }
  // ♥ Favorite (its link kept) and ↓ Offline (its pages into the vault), at the top right
  {
    int fav = fav_find(1, b->id) >= 0;
    char sl[48];
    if (msave_on && msave_series_n) snprintf(sl, sizeof sl, "Chapter %d/%d\u2026", msave_series_at ? msave_series_at : 1, msave_series_n);
    const char *fl = fav ? "\u2665 Favorite" : "\u2661 Favorite", *ol = msave_on ? (msave_series_n ? sl : "Saving\u2026") : "\u2193 Offline";
    // (a book with chapters: all of them, one after another)
    const char *al = "\u2193 All";
    int series = mg_chapters(mg.src) && mg.nch > 1 && !mg.ch_loading && !msave_on;
    int aw = series ? text_w(16, al) + 36 : 0;
    int fw = text_w(16, fl) + 36, ow = text_w(16, ol) + 36, x3 = W - 16 - aw, x2 = (series ? x3 - 10 : W - 16) - ow, x1 = x2 - 10 - fw;
    if (series) {
      rrect(x3, 12, aw, 40, 20, SURF2);
      text(x3 + 18, 38, TEXT, 16, al);
      if (hit(x3, 8, aw, 48) || (in.pressed & SCE_CTRL_START)) { msave_series_start(); return; }
    }
    (void)fw; (void)ow; (void)x1; (void)x2;
    // Favourite and Offline: two big round glossy buttons under the cover, their words beneath
    const float by = cvy + cvh + 46, r = 30;
    float fx = cvx + cvw / 2 - 48, ox = cvx + cvw / 2 + 48;
    for (int k2 = 0; k2 < 2; k2++) {
      float cx = k2 ? ox : fx;
      int lit = k2 ? msave_on : fav, pressed = in.touching && hypotf(in.tx - cx, in.ty - by) < r + 6;
      float rr = r * (pressed ? 0.92f : 1);
      circle(cx, by + 3, rr + 1, RGBA(0, 0, 0, 110));
      circle(cx, by, rr, lit ? ACC : mix_col(SURF2, ACC, 0.18f));
      circle(cx, by - rr * 0.32f, rr * 0.66f, RGBA(255, 255, 255, lit ? 60 : 30)); // (gloss)
      unsigned ink = lit ? RGBA(20, 12, 16, 255) : TEXT;
      if (k2 == 0) heart(cx, by + 2, 10, ink);
      else if (msave_on) spinner((int)cx, (int)by, 11); else icon("save", cx, by, ink);
      text_center((int)cx, (int)(by + r + 20), DIM, 13, k2 ? ol : (fav ? "Favourite" : "Favourite"));
    }
    int x1h = (int)(fx - r), x2h = (int)(ox - r);
    if (hit(x1h, (int)(by - 34), 64, 70) || (in.pressed & SCE_CTRL_SQUARE)) fav_toggle_comic(b, mg.src);
    if ((hit(x2h, (int)(by - 34), 64, 70) || (in.pressed & SCE_CTRL_TRIANGLE)) && !msave_on) {
      if (!mg_chapters(mg.src) || (mg.nch && !mg.ch_loading)) { mg_offline_pending = 1; sfx(SFX_OPEN); mg_read(!mg_chapters(mg.src) ? 0 : mg.ch_sel); return; }
      else { sfx(SFX_ERROR); say(mg.ch_loading ? "The chapters are still coming" : "Nothing here to save"); }
    }
    // (opened from Favourites to be made available offline: as soon as there's something to save)
    if (mg_offline_pending && screen == S_BOOK && (!mg_chapters(mg.src) || (mg.nch && !mg.ch_loading))) { mg_read(!mg_chapters(mg.src) ? 0 : mg.ch_sel); return; }
    if (mg_offline_pending && mg_chapters(mg.src) && !mg.ch_loading && !mg.nch) mg_offline_pending = 0;
  }
  rrect(16, 12, 44, 36, 18, SURF2);
  icon("back", 38, 30, TEXT);
  if (hit(16, 12, 44, 36) || (in.pressed & SCE_CTRL_CIRCLE)) { sfx(SFX_BACK); screen = mg_book_back; mg_book_back = S_MANGA; mg_offline_pending = 0; }
}

// ---------- the reader ----------
static void mg_turn(int d) {
  int np = mg.page + d;
  if (np < 0 || (np >= mg.npages && np >= mg.pages_total)) {
    // past the ends: the chapter before or after (MangaDex)
    if (mg_chapters(mg.src) && np >= mg.npages && mg.chapter + 1 < mg.nch) { sfx(SFX_MORE); mg_read(mg.chapter + 1); }
    else if (mg_chapters(mg.src) && np < 0 && mg.chapter > 0) { sfx(SFX_MORE); mg_read(mg.chapter - 1); }
    return;
  }
  if (np >= mg.npages) return; // (still coming)
  sfx(SFX_TICK);
  // the slots slide along: what was next is now shown
  if (d == 1) { release_tex(mg.ptex[0]); mg.ptex[0] = mg.ptex[1]; mg.ptex[1] = mg.ptex[2]; mg.ptex[2] = NULL; mg.ptex_page[0] = mg.ptex_page[1]; mg.ptex_page[1] = mg.ptex_page[2]; mg.ptex_page[2] = -1; }
  else if (d == -1) { // (the one before: what was shown is now the next, nothing thrown away)
    release_tex(mg.ptex[2]); mg.ptex[2] = mg.ptex[1]; mg.ptex[1] = mg.ptex[0]; mg.ptex[0] = NULL;
    mg.ptex_page[2] = mg.ptex_page[1]; mg.ptex_page[1] = mg.ptex_page[0]; mg.ptex_page[0] = -1;
  }
  else { for (int k = 0; k < 3; k++) { release_tex(mg.ptex[k]); mg.ptex[k] = NULL; mg.ptex_page[k] = -1; } }
  mg.page = np;
  mg.zoom = 1; mg.pan_x = mg.pan_y = 0;
  mg.tall_top = 1;
}
// The Comics tab's chapter, as the reader's pages: the page shown and the next two are kept (ptex)
static vita2d_texture *mg_rd_tex(int k) { for (int i = 0; i < 3; i++) if (mg.ptex_page[i] == k && mg.ptex[i]) return mg.ptex[i]; return NULL; }
static float mg_rd_aspect(int k) { (void)k; return 1.45f; }
static void mg_rd_go(int k) { int d = k - mg.page; if (d) mg_turn(d); }
static int mg_rd_now(void) { return mg.page; }
static void mg_rd_close(void) { mg_free_pages(); screen = S_BOOK; }
static void draw_reader(void) {
  mg_take();
  // (picked up where it was left: its page, once the pages are in)
  if (mg_resume_page >= 0 && mg.npages > mg_resume_page) { if (mg_resume_page > 0) mg_rd_go(mg_resume_page); mg_resume_page = -1; }
  else if (mg_resume_page >= 0 && !mg.pages_loading && mg.npages && mg.npages >= mg.pages_total) mg_resume_page = -1; // (fewer pages now)
  // (where it is, noted for the book: on each turn of a page)
  {
    static int last_page = -1, last_ch = -1, last_book = -1;
    if (mg.npages && (mg.page != last_page || mg.chapter != last_ch || mg.book != last_book) && mg_resume_page < 0) {
      last_page = mg.page; last_ch = mg.chapter; last_book = mg.book;
      prog_note();
    }
    prog_flush(0);
  }
  // (E-Hentai: the next gallery page of picture pages, before they run out)
  if (mg.src == MS_EHENTAI && !mg.pages_loading && mg.npages && mg.npages < mg.pages_total && mg.page + 6 >= mg.npages) {
    char url[600];
    lv_eh_gallery_url(&mg.books[mg.book], mg.gallery_p, url, sizeof url);
    mg.pages_loading = 1;
    mg_push(MJ_PAGES, mg.book, mg.chapter, url);
  }
  // the page shown and the next two, asked for
  // (looked at when the page moves, and twice a second: not the queue under its lock every frame)
  static int last_page = -1;
  if (mg.npages && (mg.page != last_page || frame_no % 30 == 0)) {
    last_page = mg.page;
    pthread_mutex_lock(&mgmx);
    int queued = 0;
    for (int k = 0; k < nmjobs; k++) if (mjobs[k].kind == MJ_PAGE) queued++;
    pthread_mutex_unlock(&mgmx);
    for (int k = 2; k >= 0 && queued < 3; k--) {
      int p = mg.page + k;
      static int asked[3] = {-1, -1, -1};
      if (p < mg.npages && mg.ptex_page[k] != p && asked[k] != p + mg.chapter * 100000) { asked[k] = p + mg.chapter * 100000; mg_want_page(p, 0); }
    }
    // (and the three after those, their files fetched into memory ahead of time)
    static int warm_asked = -1;
    if (queued < 2 && warm_asked != mg.page + mg.chapter * 100000) {
      warm_asked = mg.page + mg.chapter * 100000;
      for (int k = 3; k <= 5; k++) if (mg.page + k < mg.npages) mg_push(MJ_PAGE, mg.page + k, 0, mg.pages[mg.page + k]);
    }
  }
  // the pages, read as the reading options say (reader.c): this chapter's, as they come
  static ComicSrc src;
  src.n = mg.pages_total > mg.npages ? mg.pages_total : mg.npages;
  if (src.n < 1) src.n = 1;
  src.cur = mg.page; src.tex = mg_rd_tex; src.aspect = mg_rd_aspect; src.go = mg_rd_go; src.now = mg_rd_now; src.close = mg_rd_close;
  src.title = mg.book >= 0 && mg.book < mg.nbooks ? mg.books[mg.book].title : "";
  src.strip = LV_MSRC[mg.src].kind == MK_HWEBTOON; // (webtoons: one long strip, as they're made)
  if (!mg.npages && mg.err[0]) { text_center(W / 2, H / 2, ACC2, 16, mg.err); if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_BACK); mg_rd_close(); } return; }
  if (comic_reader(&src)) return;
  // (zoomed in: the page again at twice the screen, once)
  static int hi_page = -1;
  vita2d_texture *t = mg.ptex_page[0] == mg.page ? mg.ptex[0] : NULL;
  if (rd.zoom > 1.25f && hi_page != mg.page && t && vita2d_texture_get_width(t) <= W) { hi_page = mg.page; mg_want_page(mg.page, 1); }
  if (rd.zoom < 1.01f) hi_page = -1;
  // (made available offline from the book: its own screen over the reader while it saves, a bar and a
  //  ring and the page it's on; done, back to the book by itself)
  if (msave_from_book) {
    if (!msave_on && frame_no > 2) { static unsigned done_at; if (!done_at) done_at = frame_no; if (frame_no - done_at > 40) { done_at = 0; msave_from_book = 0; mg_rd_close(); return; } }
    vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 200));
    const int ww = 460, wh = 250, wx = (W - ww) / 2, wy = (H - wh) / 2;
    rrect(wx, wy + 4, ww, wh, 24, RGBA(0, 0, 0, 110));
    rrect(wx, wy, ww, wh, 24, SURF);
    rrect(wx + 2, wy + 2, ww - 4, 50, 22, with_alpha(TEXT, 8));
    float f = msave_total > 0 ? fminf(1, msave_done / (float)msave_total) : 0;
    // the page just come, small, at the left
    vita2d_texture *pt = mg.ptex_page[0] >= 0 ? mg.ptex[0] : NULL;
    if (pt) draw_cover_round(pt, wx + 24, wy + 66, 110, 156, 10, RGBA(255, 255, 255, 255)); else skeleton(wx + 24, wy + 66, 110, 156, 10);
    text(wx + 24, wy + 36, TEXT, 18, msave_on ? "Saving for offline" : "Saved for offline");
    // the ring, and the count in it
    float rcx = wx + 210, rcy = wy + 128, rr = 40;
    for (int k = 0; k < 60; k++) { float an = -1.5708f + k * 6.2832f / 60; circle(rcx + cosf(an) * rr, rcy + sinf(an) * rr, 3, with_alpha(TEXT, 30)); }
    for (int k = 0; k < (int)(60 * f); k++) { float an = -1.5708f + k * 6.2832f / 60; circle(rcx + cosf(an) * rr, rcy + sinf(an) * rr, 3.4f, ACC); }
    char c[32]; snprintf(c, sizeof c, "%d%%", (int)(f * 100));
    text_center((int)rcx, (int)rcy + 7, TEXT, 18, c);
    char pg[48]; snprintf(pg, sizeof pg, "%d of %d pages", msave_done, msave_total);
    text(wx + 272, wy + 120, DIM, 15, pg);
    if (msave_series_n) { char sc[48]; snprintf(sc, sizeof sc, "Chapter %d of %d", msave_series_at ? msave_series_at : 1, msave_series_n); text(wx + 272, wy + 144, FAINT, 13, sc); }
    // the bar under it all
    pill_progress(wx + 24, wy + wh - 26, ww - 48, 12, msave_total > 0 ? f : -1, 255);
    if (in.pressed & SCE_CTRL_CIRCLE) { msave_from_book = 0; mg_rd_close(); } // (○: back; it carries on saving)
    in.pressed = 0; in.tap = 0;
    return;
  }
  // the save button, with the controls (or □)
  if (!ro_open && (frame_no < rd.chrome_until || msave_on)) {
    char sl[64];
    if (msave_on) snprintf(sl, sizeof sl, "Saving %d / %d", msave_done, msave_total);
    else snprintf(sl, sizeof sl, "Save %s  □", mg_chapters(mg.src) ? "chapter" : "gallery");
    int bw = text_w(15, sl) + 48, bx = W - 124 - bw;
    rrect(bx, 10, bw, 40, 20, RGBA(0, 0, 0, 170));
    if (msave_on) spinner(bx + 22, 30, 8); else icon("plus", bx + 22, 30, TEXT);
    text(bx + 38, 36, TEXT, 15, sl);
    if (hit(bx, 6, bw, 48)) { in.tap = 0; if (!msave_on) msave_start(); }
  }
  if ((in.pressed & SCE_CTRL_SQUARE) && !msave_on && !ro_open) msave_start();
}
