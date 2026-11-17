// Manga: the covers of a source (MangaDex or E-Hentai), a book's page (its chapters), and the reader.
// Opened from the Manga tab of the sites window. Everything that goes over the network happens on one
// worker thread (lists, covers, chapters, pages), newest wish first; the screen shows a spinner
// meanwhile. Covers are decoded to exactly a card's size, pages to exactly the screen's (twice that
// only when zoomed in). Compiled inside main.c, after vbrowse.c.
#include "manga.h"

#define MG_CW 148
#define MG_CH 210
#define MG_MAX 300  // (books in a list)
#define MG_TEX 48   // (covers kept as textures: those on screen and about to be)

static const char *MSRC_NAME[MS_N] = {"MangaDex", "E-Hentai"};
static const char *MSRC_ICON[MS_N] = {"mangadex", "ehentai"};
static const char *MSRC_ABOUT[MS_N] = {"Comics and hentai comics", "Doujinshi, comics, CG"};
static const char *MD_ORDER[3] = {"Popular", "Latest", "New"};

static struct {
  int src, order, adult;
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
} mg = {.adult = 1, .book = -1, .page = 0, .zoom = 1, .tall_top = 1};
static int manga_back = S_WEB;
// (per book of the list: 1 when its English chapters turned out to be only on the publisher's site)
static unsigned char md_official[MG_MAX];
static int nmsources(void) { return MS_N; }
static const char *msource_name(int k) { return MSRC_NAME[k]; }
static const char *msource_icon(int k) { return MSRC_ICON[k]; }
static const char *msource_about(int k) { return MSRC_ABOUT[k]; }
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
static void mg_done(MDone *d) {
  pthread_mutex_lock(&mgmx);
  while (nmdone == 16 && worker_run) { pthread_mutex_unlock(&mgmx); sceKernelDelayThread(5000); pthread_mutex_lock(&mgmx); }
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
static int mg_fetch_page(LvHttp *h, int src, const char *url, volatile int *genp, int gen, uint8_t **b, size_t *n, const char **err) {
  *b = NULL; *n = 0;
  if (src != MS_EHENTAI) return mg_get(h, url, b, n, 16u << 20, err) == 200;
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
    while (worker_run && !nmjobs) pthread_cond_wait(&mgcv, &mgmx);
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
        st = mg_get(h, j.url, &b, &n, 4u << 20, &err);
        d.ch = calloc(100, sizeof *d.ch);
        d.n = st == 200 && d.ch ? lv_md_parse_feed((char *)b, n, d.ch, 100, &d.total) : -1;
        if (d.n < 0) snprintf(d.err, sizeof d.err, "%s", err && err[0] ? err : "The chapters couldn't be read");
        break;
      case MJ_PAGES:
        st = mg_get(h, j.url, &b, &n, 8u << 20, &err);
        if (st == 200 && j.src == MS_MANGADEX) d.n = lv_md_parse_pages((char *)b, n, &d.urls);
        else if (st == 200) {
          // (the gallery's number, from its address: https://e-hentai.org/g/<gid>/<token>/)
          char gid[24] = "";
          const char *g = strstr(j.url, "/g/");
          if (g) snprintf(gid, sizeof gid, "%.*s", (int)strcspn(g + 3, "/"), g + 3);
          d.n = lv_eh_parse_gallery((char *)b, gid, &d.urls);
        }
        else d.n = -1;
        if (d.n < 0) snprintf(d.err, sizeof d.err, "%s", err && err[0] ? err : "The pages couldn't be read");
        break;
      case MJ_PAGE: {
        int got_img = mg_fetch_page(h, j.src, j.url, &mg_gen, j.gen, &b, &n, &err);
        if (got_img) {
          int hi = j.b; // (zoomed in: twice the screen)
          int iw = 0, ih = 0;
          // (a tall page, a webtoon's: the screen's width, as tall as a texture can be)
          int tall = lv_image_size(b, n, &iw, &ih) && iw > 0 && ih > iw * 2.2f;
          if ((tall || hi) && !heap_room((size_t)W * 4096 * 8)) { tall = 0; hi = 0; } // (keeping the heap under 95%)
          if (lv_decode(b, n, tall ? W : hi ? W * 2 : W, tall ? 4096 : hi ? H * 2 : H, &d.px) != 0) d.px.rgba = NULL;
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
typedef struct {
  int src, vgen, total;
  char **urls; int n;
  LvBook book;
  char coll[200], tags[1400];
} MSave;
typedef struct { LvNew nw; LvAdded added; char name[240], when[40], kind[8]; int vgen; } MSaved;
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
      break;
    }
    d.nw = nw; d.vgen = j->vgen;
    pthread_mutex_lock(&msmx);
    if (nmsaved == capmsaved) { MSaved *nm = realloc(msaved, (capmsaved + 16) * sizeof *nm); if (nm) { msaved = nm; capmsaved += 16; } }
    if (nmsaved < capmsaved) msaved[nmsaved++] = d;
    pthread_mutex_unlock(&msmx);
    msave_done++;
  }
  if (!h) msave_failed = msave_total;
  lv_http_free(h);
  lv_strings_free(j->urls, j->n);
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
    d->nw.tags = msave_tags; d->nw.source = ""; d->nw.collection = msave_coll;
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
    if (okn <= 0) { sfx(SFX_ERROR); snprintf(m, sizeof m, "Not saved: %s", msave_title[0] ? msave_title : "the pages didn't come"); }
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
  if (mg.src == MS_MANGADEX && mg.ch && mg.chapter < mg.nch) snprintf(j->coll, sizeof j->coll, "%.150s \u2013 %.40s", j->book.title, mg.ch[mg.chapter].label);
  else snprintf(j->coll, sizeof j->coll, "%.190s", j->book.title);
  // (no characters a file name can't have, no doubled or trailing spaces)
  {
    char *o = j->coll;
    for (const char *c = j->coll; *c; c++) {
      char ch = strchr("/\\:\"*?<>|", *c) ? ' ' : *c;
      if (ch == ' ' && (o == j->coll || o[-1] == ' ')) continue;
      *o++ = ch;
    }
    while (o > j->coll && o[-1] == ' ') o--;
    *o = 0;
  }
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
static void mg_list_more(void) {
  if (mg.loading || !mg.more) return;
  char url[1600];
  if (mg.src == MS_MANGADEX) lv_md_list_url(mg.query, mg.adult, mg.order, mg.nbooks, url, sizeof url);
  else lv_eh_list_url(mg.query, mg.total, url, sizeof url); // (E-Hentai: total counts the pages read)
  mg.loading = 1;
  mg_push(MJ_LIST, 0, 0, url);
}
static int mg_book_back = S_MANGA;   // (where ○ on a book goes: the list, or the Favourites)
static int mg_offline_pending;       // (opened to be made available offline: its pages saved once here)
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
  memset(md_official, 0, sizeof md_official);
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
  memset(md_official, 0, sizeof md_official);
  mg.nbooks = 0; mg.total = 0; mg.more = 1; mg.loading = 0; mg.err[0] = 0; mg.sel = 0; mg.scroll = 0;
  mg_list_more();
}
static void manga_open(int src) {
  lv_net_init();
  mg_start_thread();
  if (src != mg.src || !mg.books) { mg.src = src; mg_search(); }
  manga_back = screen == S_MANGA ? manga_back : S_WEB;
  screen = S_MANGA;
}
static void mg_free_pages(void) {
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
    lv_md_feed_url(mg.books[k].id, mg.adult, 0, url, sizeof url);
    mg.ch_loading = 1;
    mg_push(MJ_CHAPTERS, k, 0, url);
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
  else { lv_eh_gallery_url(&mg.books[mg.book], 0, url, sizeof url); mg.pages_total = mg.books[mg.book].pages; }
  mg_push(MJ_PAGES, mg.book, chapter, url);
  screen = S_READER;
}
// the page wanted at slot k (0 shown, 1-2 next), asked for if it isn't here
static void mg_want_page(int page, int hi) {
  if (page < 0 || page >= mg.npages) return;
  for (int k = 0; k < 3; k++) if (mg.ptex_page[k] == page && !hi) return;
  mg_push(MJ_PAGE, page, hi, mg.pages[page]);
}
static void mg_take(void) {
  pthread_mutex_lock(&mgmx);
  int n = nmdone < 6 ? nmdone : 6;
  MDone got[16];
  memcpy(got, mdone, n * sizeof *got);
  memmove(mdone, mdone + n, (nmdone - n) * sizeof *mdone);
  nmdone -= n;
  pthread_mutex_unlock(&mgmx);
  for (int k = 0; k < n; k++) {
    MDone *d = &got[k];
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
            for (int i = 0; i < d->n && mg.nbooks < MG_MAX; i++) { mg.books[mg.nbooks] = d->books[i]; mg.cstate[mg.nbooks++] = 0; }
            if (mg.src == MS_MANGADEX) mg.more = d->n > 0 && mg.nbooks < d->total && mg.nbooks < MG_MAX;
            else { mg.total++; mg.more = d->n > 0 && mg.nbooks < MG_MAX; }
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
        mg.tex[best] = upload(&d->px);
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
          else if (d->n == 0 && d->a >= 0 && d->a < MG_MAX) md_official[d->a] = 1;
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
        if (mg_offline_pending && mg.npages) { mg_offline_pending = 0; msave_start(); }
        break;
      case MJ_PAGE:
        if (d->px.rgba && d->a >= mg.page && d->a <= mg.page + 2) {
          int slot = d->a - mg.page;
          // (the slots follow the page shown: 0 it, 1-2 the next ones)
          release_tex(mg.ptex[slot]);
          mg.ptex[slot] = slot == 0 && !d->b ? screen_upload(0, &d->px) : upload(&d->px);
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
static void draw_manga(void) {
  mg_take();
  vita2d_draw_rectangle(0, 0, W, H, BG);
  const int cols = 6, gap = 12, x0 = (W - cols * MG_CW - (cols - 1) * gap) / 2;
  int rows = (mg.nbooks + cols - 1) / cols;
  float content = rows * (MG_CH + 52) + 20, view_h = H - TOP - BAR + 40;
  float max_scroll = fmaxf(0, content - view_h);
  // moving: touch, the stick, the D-pad
  if (in.touching && in.ty > TOP) mg.scroll -= in.drag_dy;
  mg.scroll += in.ly * 14 - in.bdy * 1.5f;
  int before = mg.sel;
  if (in.pressed & SCE_CTRL_RIGHT) mg.sel++;
  if (in.pressed & SCE_CTRL_LEFT) mg.sel--;
  if (in.pressed & SCE_CTRL_DOWN) mg.sel += cols;
  if (in.pressed & SCE_CTRL_UP) mg.sel -= cols;
  if (mg.sel >= mg.nbooks) mg.sel = mg.nbooks - 1;
  if (mg.sel < 0) mg.sel = 0;
  if (mg.sel != before) {
    sfx(SFX_TICK);
    float y = (mg.sel / cols) * (MG_CH + 52);
    if (y < mg.scroll) mg.scroll = y;
    if (y + MG_CH + 52 > mg.scroll + view_h) mg.scroll = y + MG_CH + 52 - view_h;
  }
  if (mg.scroll > max_scroll) mg.scroll = max_scroll;
  if (mg.scroll < 0) mg.scroll = 0;
  int last = 0;
  for (int i = 0; i < mg.nbooks; i++) {
    float x = x0 + (i % cols) * (MG_CW + gap), y = TOP + 10 + (i / cols) * (MG_CH + 52) - mg.scroll;
    if (y > H) break;
    if (y + MG_CH + 52 < TOP) continue;
    last = i;
    int on = i == mg.sel && !in.touching;
    if (on) rrect(x - 4, y - 4, MG_CW + 8, MG_CH + 8, 14, ACC);
    vita2d_texture *t = mg_cover(i);
    if (t) vita2d_draw_texture_scale(t, x, y, MG_CW / (float)vita2d_texture_get_width(t), MG_CH / (float)vita2d_texture_get_height(t));
    else {
      rrect(x, y, MG_CW, MG_CH, 10, SURF);
      if (mg.cstate && mg.cstate[i] == 3) text_center(x + MG_CW / 2, y + MG_CH / 2, FAINT, 13, "No cover");
      else {
        spinner(x + MG_CW / 2, y + MG_CH / 2, 12);
        // (asked for once the list isn't moving fast: covers flown past aren't fetched for nothing)
        if (mg.cstate && mg.cstate[i] == 0 && mg.books[i].cover[0] && !(in.touching && abs(in.drag_dy) > 25)) { mg.cstate[i] = 1; mg_push(MJ_COVER, i, 0, mg.books[i].cover); }
      }
    }
    if (i < MG_MAX && md_official[i]) {
      vita2d_draw_rectangle(x, y, MG_CW, MG_CH, RGBA(10, 8, 12, 150));
      rrect(x + 6, y + MG_CH - 26, MG_CW - 12, 20, 10, RGBA(0, 0, 0, 190));
      text_center(x + MG_CW / 2, y + MG_CH - 11, DIM, 11, "Official release only");
    }
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
  if (last + cols * 2 >= mg.nbooks) mg_list_more();
  if (!mg.nbooks) {
    if (mg.loading) spinner(W / 2, TOP + 150, 22);
    else text_center(W / 2, TOP + 150, DIM, 17, mg.err[0] ? mg.err : "Nothing found");
  } else if (mg.loading) spinner(W / 2, H - BAR - 20, 12);

  // the top: back, the source (its tab of the sites window), the search, the order, 18+
  vita2d_draw_rectangle(0, 0, W, TOP, with_alpha(BG, 250));
  rrect(16, 8, 44, 36, 18, SURF2);
  icon("back", 38, 26, TEXT);
  if (hit(16, 8, 44, 36)) { sfx(SFX_BACK); screen = manga_back; return; }
  char sl[64];
  snprintf(sl, sizeof sl, "%s  ▾", MSRC_NAME[mg.src]);
  int sw = text_w(17, sl) + 36;
  rrect(70, 8, sw, 36, 18, ACC);
  text(88, 32, RGBA(20, 12, 16, 255), 17, sl);
  if (hit(70, 8, sw, 36) || (in.pressed & SCE_CTRL_START)) { sfx(SFX_OPEN); screen = S_WEB; sheet = SH_SITES; sites_tab = 2; sheet_sel = mg.src; sites_scroll = 0; return; }
  int rx = W - 16, hb;
  if (mg.src == MS_MANGADEX) {
    rx -= head_button(rx, NULL, mg.adult ? "18+ on" : "18+ off", mg.adult ? with_alpha(ACC, 110) : SURF2, &hb) + 10;
    if (hb || (in.pressed & SCE_CTRL_SQUARE)) { sfx(SFX_TOGGLE); mg.adult = !mg.adult; mg_search(); }
    rx -= head_button(rx, "sort", MD_ORDER[mg.order], SURF2, &hb) + 10;
    if (hb || (in.pressed & SCE_CTRL_TRIANGLE)) { sfx(SFX_TOGGLE); mg.order = (mg.order + 1) % 3; mg_search(); }
  }
  rrect(16, 50, W - 32, 36, 18, SURF2);
  icon("search", 38, 68, mg.query[0] ? TEXT : FAINT);
  text_fit(56, 74, mg.query[0] ? TEXT : FAINT, 16, mg.query[0] ? mg.query : "Search titles", W - 100);
  if (hit(16, 50, W - 32, 36) && !ime_open) ime_start(15, "Search", mg.query, 0);
  if ((in.pressed & SCE_CTRL_CROSS) && mg.nbooks) { sfx(SFX_SELECT); mg_open_book(mg.sel); return; }
  if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_BACK); screen = manga_back; return; }
  draw_bar();
}

// ---------- a book ----------
static void draw_book(void) {
  mg_take();
  vita2d_draw_rectangle(0, 0, W, H, BG);
  if (mg.book < 0 || mg.book >= mg.nbooks) { screen = S_MANGA; return; }
  LvBook *b = &mg.books[mg.book];
  // the cover, big, on the left
  if (mg.big_cover) vita2d_draw_texture_scale(mg.big_cover, 30, 70, 220.0f / vita2d_texture_get_width(mg.big_cover), 312.0f / vita2d_texture_get_height(mg.big_cover));
  else { rrect(30, 70, 220, 312, 14, SURF); spinner(140, 226, 16); }
  text_fit(276, 96, TEXT, 22, b->title, W - 300);
  char tags[400];
  snprintf(tags, sizeof tags, "%.390s", b->tags);
  for (char *p = tags; *p; p++) if (*p == '|') *p = ',';
  text_fit(276, 122, FAINT, 13, tags, W - 300);
  int listy = 146;
  if (mg.src == MS_EHENTAI) {
    char m[64];
    snprintf(m, sizeof m, "%d pages", b->pages);
    text(276, listy + 20, DIM, 16, m);
    rrect(276, listy + 40, 180, 46, 23, ACC);
    text_center(366, listy + 70, RGBA(20, 12, 16, 255), 18, "▶  Read");
    if (hit(276, listy + 40, 180, 46) || (in.pressed & SCE_CTRL_CROSS)) { sfx(SFX_SELECT); mg_read(0); return; }
  } else if (mg.ch_loading) spinner(560, 260, 20);
  else if (!mg.nch && mg.err[0]) text(276, listy + 30, ACC2, 16, mg.err);
  else if (!mg.nch) {
    // its English chapters are the publisher's: said plainly, with the way on
    rrect(276, listy + 6, W - 306, 210, 18, SURF);
    icon("book", 310, listy + 44, ACC);
    text(334, listy + 51, TEXT, 20, "Published officially");
    text_fit(300, listy + 86, DIM, 15, "Its English chapters are licensed: they're read on the publisher's", W - 350);
    text_fit(300, listy + 108, DIM, 15, "own site or app, so MangaDex has no pages of them to give the Vita.", W - 350);
    // the next title in the list that isn't known to be the same
    int next = -1;
    for (int k = mg.book + 1; k < mg.nbooks && next < 0; k++) if (k >= MG_MAX || !md_official[k]) next = k;
    int bx = 300, by = listy + 140;
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
      char p[16];
      snprintf(p, sizeof p, "%d pages", mg.ch[k].pages);
      text(W - 46 - text_w(13, p), y + 28, FAINT, 13, p);
      if (hit(276, (int)y, W - 306, rh - 6) && in.tap_y > listy) { mg.ch_sel = k; sfx(SFX_SELECT); mg_read(k); return; }
    }
    if (in.pressed & SCE_CTRL_DOWN) { mg.ch_sel = mg.ch_sel + 1 < mg.nch ? mg.ch_sel + 1 : mg.ch_sel; sfx(SFX_TICK); }
    if (in.pressed & SCE_CTRL_UP) { mg.ch_sel = mg.ch_sel > 0 ? mg.ch_sel - 1 : 0; sfx(SFX_TICK); }
    if (in.pressed & SCE_CTRL_CROSS) { sfx(SFX_SELECT); mg_read(mg.ch_sel); return; }
  }
  // ♥ Favorite (its link kept) and ↓ Offline (its pages into the vault), at the top right
  {
    int fav = fav_find(1, b->id) >= 0;
    const char *fl = fav ? "\u2665 Favorite" : "\u2661 Favorite", *ol = msave_on ? "Saving\u2026" : "\u2193 Offline";
    int fw = text_w(16, fl) + 36, ow = text_w(16, ol) + 36, x2 = W - 16 - ow, x1 = x2 - 10 - fw;
    rrect(x1, 12, fw, 40, 20, fav ? ACC : SURF2);
    text(x1 + 18, 38, fav ? RGBA(20, 12, 16, 255) : TEXT, 16, fl);
    rrect(x2, 12, ow, 40, 20, SURF2);
    text(x2 + 18, 38, TEXT, 16, ol);
    if (hit(x1, 8, fw, 48) || (in.pressed & SCE_CTRL_SQUARE)) fav_toggle_comic(b, mg.src);
    if ((hit(x2, 8, ow, 48) || (in.pressed & SCE_CTRL_TRIANGLE)) && !msave_on) {
      if (mg.src == MS_EHENTAI || (mg.nch && !mg.ch_loading)) { mg_offline_pending = 1; sfx(SFX_OPEN); mg_read(mg.src == MS_EHENTAI ? 0 : mg.ch_sel); return; }
      else { sfx(SFX_ERROR); say(mg.ch_loading ? "The chapters are still coming" : "Nothing here to save"); }
    }
    // (opened from Favourites to be made available offline: as soon as there's something to save)
    if (mg_offline_pending && screen == S_BOOK && (mg.src == MS_EHENTAI || (mg.nch && !mg.ch_loading))) { mg_read(mg.src == MS_EHENTAI ? 0 : mg.ch_sel); return; }
    if (mg_offline_pending && mg.src == MS_MANGADEX && !mg.ch_loading && !mg.nch) mg_offline_pending = 0;
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
    if (mg.src == MS_MANGADEX && np >= mg.npages && mg.chapter + 1 < mg.nch) { sfx(SFX_MORE); mg_read(mg.chapter + 1); }
    else if (mg.src == MS_MANGADEX && np < 0 && mg.chapter > 0) { sfx(SFX_MORE); mg_read(mg.chapter - 1); }
    return;
  }
  if (np >= mg.npages) return; // (still coming)
  sfx(SFX_TICK);
  // the slots slide along: what was next is now shown
  if (d == 1) { release_tex(mg.ptex[0]); mg.ptex[0] = mg.ptex[1]; mg.ptex[1] = mg.ptex[2]; mg.ptex[2] = NULL; mg.ptex_page[0] = mg.ptex_page[1]; mg.ptex_page[1] = mg.ptex_page[2]; mg.ptex_page[2] = -1; }
  else { for (int k = 0; k < 3; k++) { release_tex(mg.ptex[k]); mg.ptex[k] = NULL; mg.ptex_page[k] = -1; } }
  mg.page = np;
  mg.zoom = 1; mg.pan_x = mg.pan_y = 0;
  mg.tall_top = 1;
}
static void draw_reader(void) {
  mg_take();
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 255));
  // (E-Hentai: the next gallery page of picture pages, before they run out)
  if (mg.src == MS_EHENTAI && !mg.pages_loading && mg.npages && mg.npages < mg.pages_total && mg.page + 6 >= mg.npages) {
    char url[600];
    lv_eh_gallery_url(&mg.books[mg.book], mg.gallery_p, url, sizeof url);
    mg.pages_loading = 1;
    mg_push(MJ_PAGES, mg.book, mg.chapter, url);
  }
  // the page shown and the next two, asked for
  if (mg.npages) {
    pthread_mutex_lock(&mgmx);
    int queued = 0;
    for (int k = 0; k < nmjobs; k++) if (mjobs[k].kind == MJ_PAGE) queued++;
    pthread_mutex_unlock(&mgmx);
    for (int k = 2; k >= 0 && queued < 3; k--) {
      int p = mg.page + k;
      static int asked[3] = {-1, -1, -1};
      if (p < mg.npages && mg.ptex_page[k] != p && asked[k] != p + mg.chapter * 100000) { asked[k] = p + mg.chapter * 100000; mg_want_page(p, 0); }
    }
  }
  vita2d_texture *t = mg.ptex_page[0] == mg.page ? mg.ptex[0] : NULL;
  int tall = 0;
  static float tall_my, tall_v;
  if (t) {
    float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
    // (a tall page, a webtoon's: the screen's width, from its top, scrolled down)
    tall = th > tw * 2.2f;
    float s = (tall ? W / tw : fminf(W / tw, H / th)) * mg.zoom;
    float dw = tw * s, dh = th * s;
    float mx = fmaxf(0, (dw - W) / 2), my = fmaxf(0, (dh - H) / 2);
    if (tall && mg.tall_top) { mg.pan_y = my; mg.tall_top = 0; tall_v = 0; }
    if (tall) { mg.pan_y += tall_v; tall_v *= 0.93f; if (fabsf(tall_v) < 0.3f) tall_v = 0; tall_my = my; }
    mg.pan_x = fminf(mx, fmaxf(-mx, mg.pan_x));
    mg.pan_y = fminf(my, fmaxf(-my, mg.pan_y));
    vita2d_draw_texture_scale(t, (W - dw) / 2 + mg.pan_x + mg.swipe, (H - dh) / 2 + mg.pan_y, s, s);
  } else if (mg.err[0] && !mg.npages) text_center(W / 2, H / 2, ACC2, 16, mg.err);
  else spinner(W / 2, H / 2, 24);
  // the page number, faint along the bottom
  char pn[64];
  snprintf(pn, sizeof pn, "%d / %d", mg.page + 1, mg.pages_total ? mg.pages_total : mg.npages);
  int pw = text_w(14, pn) + 24;
  rrect(W / 2 - pw / 2, H - 34, pw, 26, 13, RGBA(0, 0, 0, 150));
  text_center(W / 2, H - 16, DIM, 14, pn);
  // touch: swipe to turn, pinch to zoom, drag when zoomed
  static float pd0, z0;
  if (in.touching) {
    if (in.tcount >= 2) { float d = hypotf(in.t2x - in.tx, in.t2y - in.ty); if (pd0 == 0) { pd0 = d; z0 = mg.zoom; } else mg.zoom = fminf(4, fmaxf(1, z0 * d / pd0)); }
    else if (mg.zoom > 1.01f) { pd0 = 0; mg.pan_x += in.drag_dx; mg.pan_y += in.drag_dy; }
    else if (tall && fabsf((float)(in.ty - in.down_y)) >= fabsf((float)(in.tx - in.down_x))) { pd0 = 0; mg.pan_y += in.drag_dy; tall_v = 0; }
    else { pd0 = 0; mg.swipe += in.drag_dx; }
  } else {
    pd0 = 0;
    if (in.flicked && tall && mg.zoom < 1.01f && fabsf(in.fvy) > fabsf(in.fvx)) tall_v = in.fvy * 1.4f;
    if (mg.swipe < -120) { mg.swipe = 0; mg_turn(1); }
    else if (mg.swipe > 120) { mg.swipe = 0; mg_turn(-1); }
    mg.swipe *= 0.6f;
  }
  // buttons: ←/→ (or a tap on either side) turn, L/R the chapters, ✕ zoom, ○ back
  // the save button, top right (or △)
  {
    char sl[64];
    if (msave_on) snprintf(sl, sizeof sl, "Saving %d / %d", msave_done, msave_total);
    else snprintf(sl, sizeof sl, "Save %s", mg.src == MS_MANGADEX ? "chapter" : "gallery");
    int bw = text_w(15, sl) + 48;
    rrect(W - bw - 12, 10, bw, 36, 18, RGBA(0, 0, 0, 170));
    if (msave_on) spinner(W - bw + 10, 28, 8); else icon("plus", W - bw + 10, 28, TEXT);
    text(W - bw + 26, 34, TEXT, 15, sl);
    if (hit(W - bw - 12, 6, bw, 44) || (in.pressed & SCE_CTRL_TRIANGLE)) { in.tap = 0; if (!msave_on) msave_start(); }
  }
  if (in.tap && mg.zoom < 1.01f) { if (in.tap_x > W * 2 / 3) mg_turn(1); else if (in.tap_x < W / 3) mg_turn(-1); }
  if (in.pressed & SCE_CTRL_RIGHT) mg_turn(1);
  if (in.pressed & SCE_CTRL_LEFT) mg_turn(-1);
  if (mg.src == MS_MANGADEX && (in.pressed & SCE_CTRL_RTRIGGER) && mg.chapter + 1 < mg.nch) { sfx(SFX_MORE); mg_read(mg.chapter + 1); return; }
  if (mg.src == MS_MANGADEX && (in.pressed & SCE_CTRL_LTRIGGER) && mg.chapter > 0) { sfx(SFX_MORE); mg_read(mg.chapter - 1); return; }
  if (in.pressed & SCE_CTRL_CROSS) { mg.zoom = mg.zoom > 1.5f ? 1 : 2; mg.pan_x = mg.pan_y = 0; }
  if (mg.zoom > 1.01f) { mg.pan_x -= in.lx * 12; mg.pan_y -= in.ly * 12; }
  else if (tall) {
    // a tall page: the stick scrolls, ↑↓ a screenful (↓ at its foot: the next page)
    mg.pan_y -= in.ly * 16;
    if (in.pressed & SCE_CTRL_UP) tall_v = 22;
    if (in.pressed & SCE_CTRL_DOWN) { if (mg.pan_y <= -tall_my + 2) mg_turn(1); else tall_v = -22; }
  }
  // (zoomed in: the page again at twice the screen, once)
  static int hi_page = -1;
  if (mg.zoom > 1.25f && hi_page != mg.page && t && vita2d_texture_get_width(t) <= W) { hi_page = mg.page; mg_want_page(mg.page, 1); }
  if (mg.zoom < 1.01f) hi_page = -1;
  if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_BACK); mg_free_pages(); screen = S_BOOK; }
}
