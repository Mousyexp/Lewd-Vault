// The Browser: boorus (booru.c) in the same grid as the Library, a full view, and saving into the
// vault. Compiled as part of main.c (#include "web.c": one translation unit, so it shares the
// screens' helpers and the compiler can inline across both).
//
// Light on the card and the network: the grid loads the sites' small previews (150-360 px), the full
// view their samples (~850 px, made for screens), never the originals. A save keeps a JPEG sample
// as it is when it fits 1920 x 1088 (the most a Vita shows, zoomed), and otherwise makes a JPEG of
// it at that size; its thumbnail is a card-sized JPEG; its row is appended to index.csv.
//
// Threads: one reads result pages, two fetch previews, one fetches the full view's sample (and
// prepares saves). Everything they hand back carries the search's generation: a new search makes
// whatever is still on its way from the old one go to waste instead of into the new grid.
#include "booru.h"
#include <psp2/io/devctl.h>
#include "net.h"

#define WSLOTS 120 // (atlas cells 120-239: main.c)
#define MAX_POSTS 2000
#define BOX_LONG 1920
#define BOX_SHORT 1088

static const int RATING_NSFW[4] = {1, 4, 7, 9}; // general, sensitive, questionable, explicit (as Discover)
static const char *RATING_NAMES[4] = {"General", "Sensitive", "Questionable", "Explicit"};

static LvPost *posts;
static int nposts;
static signed char *wstate; // per post: 0 nothing, 1 asked, 2 in a slot, 3 failed
static short *wslot_of;
typedef struct { int post; vita2d_texture *tex; unsigned used; } WSlot;
static WSlot wslots[WSLOTS];
static pthread_mutex_t wmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wcv = PTHREAD_COND_INITIALIZER;
static volatile int wgen = 1;
static int wpage, wend, wloading, list_want;
static char werr[160];
static int wwant[48], nwwant;
// a page of results from the list thread, waiting to be added to the grid
static struct { int ready, page, count; LvPost *posts; char err[160]; } page_ready;

enum { WD_PREVIEW, WD_SAMPLE, WD_SAVE };
typedef struct {
  int kind, post, gen, vgen;
  LvPixels px;
  LvAdded added; // (a save: what was written, for the loaded vault)
  LvNew nw;
  char name[96], cat[64], when[40];
  uint8_t *a, *b; // sample: its raw bytes; save: the file and its thumbnail
  size_t an, bn;
  int w, h, tw, th;
  char ext[8];
  char err[96];
} WDone;
static WDone wdone[24];
static int nwdone;

// the full view's job: a sample to fetch, or a save to prepare
static struct {
  int post, gen, save, vgen;
  uint8_t *raw; // (a save: the sample's bytes, handed over; a video's are in its file)
  size_t rawn;
  char cat[64];
} big_job = {-1, 0, 0, 0, NULL, 0, ""};
// the search a page is for, copied when asked (the settings may change while it loads)
static struct { int site, ratings, sort; char q[128]; } job_q;
static int saving;
static const char *save_category(void);
static volatile float big_progress;
static volatile int big_cancel;
static int web_started;

static unsigned long long fnv64(const char *s) {
  unsigned long long h = 1469598103934665603ULL;
  while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
  return h;
}
// the vault's sources, to show what's saved already (and not save it twice)
static unsigned long long *saved_src;
static int nsaved, capsaved;
static int cmp_u64(const void *a, const void *b) {
  unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
  return x < y ? -1 : x > y;
}
static void saved_build(void) {
  free(saved_src);
  capsaved = vault.n + 64;
  saved_src = malloc(capsaved * sizeof *saved_src);
  nsaved = 0;
  for (int i = 0; i < vault.n; i++) if (vault.items[i].source[0]) saved_src[nsaved++] = fnv64(vault.items[i].source);
  qsort(saved_src, nsaved, sizeof *saved_src, cmp_u64);
}
static int is_saved(const LvPost *p) {
  if (!saved_src) return 0;
  unsigned long long h = fnv64(p->page);
  return bsearch(&h, saved_src, nsaved, sizeof h, cmp_u64) != NULL;
}
static void saved_add(const char *src) {
  if (nsaved == capsaved) { capsaved = capsaved * 2 + 16; saved_src = realloc(saved_src, capsaved * sizeof *saved_src); }
  saved_src[nsaved++] = fnv64(src);
  qsort(saved_src, nsaved, sizeof *saved_src, cmp_u64);
}

// ---------- the threads ----------

static void *list_main(void *arg) {
  LvHttp *h = lv_http_new();
  while (worker_run) {
    pthread_mutex_lock(&wmx);
    while (worker_run && !list_want) pthread_cond_wait(&wcv, &wmx);
    int page = list_want, gen = wgen, ratings = job_q.ratings;
    list_want = 0;
    const LvSite *s = &LV_SITES[job_q.site];
    char url[2048];
    lv_booru_url(s, job_q.q, ratings, job_q.sort, page, url, sizeof url);
    pthread_mutex_unlock(&wmx);
    if (!worker_run) break;
    uint8_t *b = NULL;
    size_t n = 0;
    const char *err = NULL;
    int st = lv_http_get(h, url, &b, &n, 8 * 1024 * 1024, NULL, NULL, &err);
    int count = 0;
    LvPost *got = st == 200 ? lv_booru_parse(s, (const char *)b, n, ratings, &count) : NULL;
    // handed over: only the drawing thread changes the list it draws (see web_take_results)
    pthread_mutex_lock(&wmx);
    if (gen == wgen && !page_ready.ready) {
      page_ready.ready = 1;
      page_ready.page = page;
      page_ready.posts = got;
      page_ready.count = count;
      page_ready.err[0] = 0;
      if (st != 200 || !got) {
        snprintf(page_ready.err, sizeof page_ready.err, "%s", st == 200 ? "The site's answer couldn't be read" : err && err[0] ? err : "No answer from the site");
        // (Cloudflare's "checking your browser" page: say so)
        if (b && strstr((const char *)b, "Just a moment")) snprintf(page_ready.err, sizeof page_ready.err, "The site is asking for a browser check: try again later");
      }
      got = NULL;
    }
    pthread_mutex_unlock(&wmx);
    if (got) lv_posts_free(got, count);
    free(b);
  }
  lv_http_free(h);
  return NULL;
}

static void *preview_main(void *arg) {
  LvHttp *h = lv_http_new();
  while (worker_run) {
    pthread_mutex_lock(&wmx);
    while (worker_run && nwwant == 0) pthread_cond_wait(&wcv, &wmx);
    if (!worker_run) { pthread_mutex_unlock(&wmx); break; }
    int p = wwant[0], gen = wgen;
    memmove(wwant, wwant + 1, --nwwant * sizeof *wwant);
    char url[1024];
    snprintf(url, sizeof url, "%s", p < nposts ? posts[p].preview : "");
    int full = nwdone >= 20;
    pthread_mutex_unlock(&wmx);
    if (full || !url[0]) {
      pthread_mutex_lock(&wmx);
      if (gen == wgen && p < nposts) wstate[p] = full ? 0 : 3;
      pthread_mutex_unlock(&wmx);
      if (full) sceKernelDelayThread(8000);
      continue;
    }
    uint8_t *b = NULL;
    size_t n = 0;
    int st = lv_http_get(h, url, &b, &n, 4 * 1024 * 1024, NULL, NULL, NULL);
    LvPixels px = {0};
    int cw = CARD_W, ch = CARD_H * 2;
    if (st == 200) cover_size(b, n, &cw, &ch);
    int ok = st == 200 && lv_decode(b, n, cw, ch, &px) == 0;
    free(b);
    pthread_mutex_lock(&wmx);
    if (gen == wgen && p < nposts) {
      if (ok && nwdone < 24) { WDone d; memset(&d, 0, sizeof d); d.kind = WD_PREVIEW; d.post = p; d.gen = gen; d.px = px; wdone[nwdone++] = d; px.rgba = NULL; }
      else wstate[p] = ok ? 0 : 3;
    }
    pthread_mutex_unlock(&wmx);
    free(px.rgba);
  }
  lv_http_free(h);
  return NULL;
}

static void post_copy(const LvPost *s, LvPost *d) {
  *d = *s;
  d->preview = strdup(s->preview); d->sample = strdup(s->sample); d->file = strdup(s->file);
  d->page = strdup(s->page); d->source = strdup(s->source); d->tags = strdup(s->tags);
}

// A save: the file as the Vita wants it, and a card-sized thumbnail.
static void prepare_save(LvHttp *h, const LvPost *p, uint8_t *raw, size_t rawn, WDone *d) {
  int video = lv_post_is_video(p), gif = !strcmp(p->ext, "gif");
  int w = 0, hh = 0;
  if (video || gif) {
    // as it is (the Vita can't make these smaller; a video straight from its file); the thumbnail
    // from the preview
    d->a = raw; d->an = rawn; raw = NULL;
    snprintf(d->ext, sizeof d->ext, "%s", p->ext);
    d->w = p->width; d->h = p->height;
    uint8_t *b = NULL;
    size_t n = 0;
    if (lv_http_get(h, p->preview, &b, &n, 4 * 1024 * 1024, NULL, NULL, NULL) == 200) {
      LvPixels px;
      int cw, ch;
      cover_size(b, n, &cw, &ch);
      if (lv_decode(b, n, cw, ch, &px) == 0) { d->b = lv_encode_jpeg(&px, 85, &d->bn); d->tw = px.w; d->th = px.h; free(px.rgba); }
    }
    free(b);
    return;
  }
  int landscape = lv_image_size(raw, rawn, &w, &hh) && w >= hh;
  int mw = landscape ? BOX_LONG : BOX_SHORT, mh = landscape ? BOX_SHORT : BOX_LONG;
  snprintf(d->ext, sizeof d->ext, "jpg");
  if (lv_jpeg_size(raw, rawn, &w, &hh) && w <= mw && hh <= mh) {
    // a JPEG that fits: kept byte for byte (no second compression)
    d->a = raw; d->an = rawn; d->w = w; d->h = hh; raw = NULL;
  } else {
    LvPixels px;
    if (lv_decode(raw, rawn, mw, mh, &px) != 0) { snprintf(d->err, sizeof d->err, "This picture can't be read"); free(raw); return; }
    d->a = lv_encode_jpeg(&px, 90, &d->an);
    d->w = px.w; d->h = px.h;
    free(px.rgba);
    if (!d->a) { snprintf(d->err, sizeof d->err, "This picture couldn't be made into a JPEG"); free(raw); return; }
  }
  // the thumbnail: the picture again, decoded straight to card size
  const uint8_t *src = d->a;
  size_t srcn = d->an;
  LvPixels tp;
  int cw, ch;
  cover_size(src, srcn, &cw, &ch);
  if (lv_decode(src, srcn, cw, ch, &tp) == 0) { d->b = lv_encode_jpeg(&tp, 85, &d->bn); d->tw = tp.w; d->th = tp.h; free(tp.rgba); }
  free(raw);
}

static char video_path[256];

// Free room where the vault is (its device: ux0:, uma0:...), -1 when unknown.
static long long free_room(const char *root) {
  char dev[16];
  const char *c = strchr(root, ':');
  if (!c || c - root > 10) return -1;
  snprintf(dev, sizeof dev, "%.*s", (int)(c - root + 1), root);
  SceIoDevInfo info;
  memset(&info, 0, sizeof info);
  if (sceIoDevctl(dev, 0x3001, NULL, 0, &info, sizeof info) < 0) return -1;
  return (long long)info.free_size;
}

// The save's files, written here on the save thread: the vault is locked only while it's touched.
static void write_save(const LvPost *post, WDone *d, int vgen_asked, const char *cat) {
  int video = lv_post_is_video(post), gif = !strcmp(post->ext, "gif");
  snprintf(d->name, sizeof d->name, "%s %lld.%s", LV_SITES[job_q.site].name, post->id, d->ext);
  snprintf(d->cat, sizeof d->cat, "%s", cat);
  now_iso(d->when, sizeof d->when);
  int ri = post->rating == LV_R_EXPLICIT ? 3 : post->rating == LV_R_QUESTIONABLE ? 2 : post->rating == LV_R_SENSITIVE ? 1 : 0;
  LvNew nw = {.name = d->name, .category = d->cat, .kind = video ? "video" : gif ? "gif" : "image", .tags = post->tags,
              .source = post->page, .added = d->when, .nsfw = RATING_NSFW[ri], .quality = 5, .width = d->w, .height = d->h,
              .from = d->a ? NULL : video_path};
  size_t need = d->a ? d->an : 0;
  if (!d->a) { struct stat st; if (stat(video_path, &st) == 0) need = (size_t)st.st_size; d->an = need; }
  pthread_mutex_lock(&vmx);
  if (vgen_asked != vgen || !loaded_vault) {
    pthread_mutex_unlock(&vmx);
    snprintf(d->err, sizeof d->err, "Not saved: another vault was opened");
    return;
  }
  long long room = free_room(vault.root);
  if (room >= 0 && room < (long long)need + 4 * 1024 * 1024) {
    pthread_mutex_unlock(&vmx);
    snprintf(d->err, sizeof d->err, "Not enough room on the memory card");
    return;
  }
  const char *why = NULL;
  int r = lv_vault_add_files(&vault, &nw, d->a, d->an, d->ext, d->b, d->bn, d->tw, d->th, &d->added, &why);
  pthread_mutex_unlock(&vmx);
  if (r != 0) { snprintf(d->err, sizeof d->err, "%s", why ? why : "It couldn't be saved"); return; }
  // what the loaded vault needs, kept with the result (the post's strings go with it)
  d->nw = nw;
  d->nw.name = d->name; d->nw.category = d->cat; d->nw.added = d->when;
  d->nw.tags = strdup(post->tags); d->nw.source = strdup(post->page);
  d->vgen = vgen_asked;
}

static void *sample_main(void *arg) {
  LvHttp *h = lv_http_new();
  while (worker_run) {
    pthread_mutex_lock(&wmx);
    while (worker_run && big_job.post < 0) pthread_cond_wait(&wcv, &wmx);
    if (!worker_run) { pthread_mutex_unlock(&wmx); break; }
    int p = big_job.post, gen = big_job.gen, save = big_job.save, vgen_asked = big_job.vgen;
    char cat[64];
    snprintf(cat, sizeof cat, "%s", big_job.cat);
    uint8_t *raw = big_job.raw;
    size_t rawn = big_job.rawn;
    big_job.post = -1;
    big_job.raw = NULL;
    LvPost post;
    int have = p < nposts;
    if (have) post_copy(&posts[p], &post);
    big_cancel = 0;
    big_progress = 0;
    pthread_mutex_unlock(&wmx);
    if (!have || (save && gen != wgen)) { free(raw); if (save) { pthread_mutex_lock(&wmx); saving = 0; pthread_mutex_unlock(&wmx); } if (have) { free(post.preview); free(post.sample); free(post.file); free(post.page); free(post.source); free(post.tags); } continue; }
    WDone d;
    memset(&d, 0, sizeof d);
    d.post = p;
    d.gen = gen;
    if (save) {
      d.kind = WD_SAVE;
      prepare_save(h, &post, raw, rawn, &d);
      if (!d.err[0]) write_save(&post, &d, vgen_asked, cat);
      // (the file is on the card now: its bytes needn't wait in memory)
      free(d.a); d.a = NULL;
      free(d.b); d.b = NULL;
    } else {
      d.kind = WD_SAMPLE;
      int video = lv_post_is_video(&post);
      const char *err = NULL;
      uint8_t *b = NULL;
      size_t n = 0;
      int st;
      if (video) {
        // a video: the whole file, straight onto the card (it never has to fit in memory), played
        // from there and copied into the vault from there by a save
        snprintf(video_path, sizeof video_path, DATA "/play.%s", post.ext);
        st = lv_http_get_file(h, post.file, video_path, &n, 200u * 1024 * 1024, &big_cancel, &big_progress, &err);
      } else {
        st = lv_http_get(h, post.sample, &b, &n, 32u * 1024 * 1024, &big_cancel, &big_progress, &err);
      }
      if (st != 200) {
        snprintf(d.err, sizeof d.err, "%s", err && err[0] ? err : "It didn't download");
        free(b);
      } else if (video) {
        snprintf(d.ext, sizeof d.ext, "%s", post.ext);
      } else {
        d.a = b; d.an = n;
        if (lv_decode(b, n, 1920, 1920, &d.px) != 0) {
          // (a GIF, or something else the Vita can't decode: the preview stands in)
          snprintf(d.err, sizeof d.err, "%s", !strcmp(post.ext, "gif") ? "GIF: the preview is shown" : "This picture can't be shown");
        }
      }
    }
    free(post.preview); free(post.sample); free(post.file); free(post.page); free(post.source); free(post.tags);
    pthread_mutex_lock(&wmx);
    if (nwdone < 24) wdone[nwdone++] = d;
    else { free(d.px.rgba); free(d.a); free(d.b); }
    pthread_mutex_unlock(&wmx);
  }
  lv_http_free(h);
  return NULL;
}

static void web_start(void) {
  if (web_started) return;
  web_started = 1;
  lv_net_init();
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 256 * 1024);
  pthread_create(&t, &at, list_main, NULL);
  pthread_create(&t, &at, preview_main, NULL);
  pthread_create(&t, &at, preview_main, NULL);
  pthread_create(&t, &at, sample_main, NULL);
}

// ---------- searches ----------

static Grid wgrid;
static float post_aspect(int k) { return posts[k].width > 0 && posts[k].height > 0 ? (float)posts[k].width / posts[k].height : 0.75f; }
static float wscroll, wscroll_v;
// picking up where the Browser was: the pages to load again and the post to go back to (-1: none)
static int resume_pages, resume_sel = -1, resume_checked;
static int wsel;
static void web_clear_textures(void) {
  // (the cells stay: only who they belong to is forgotten)
  for (int k = 0; k < WSLOTS; k++) { wslots[k].tex = NULL; wslots[k].post = -1; wslots[k].used = 0; }
}
static void web_search(void) {
  pthread_mutex_lock(&wmx);
  wgen++;
  for (int k = 0; k < nwdone; k++) { free(wdone[k].px.rgba); free(wdone[k].a); free(wdone[k].b); }
  nwdone = 0;
  nwwant = 0;
  if (page_ready.ready) { lv_posts_free(page_ready.posts, page_ready.count); page_ready.ready = 0; page_ready.posts = NULL; }
  lv_posts_free(posts, nposts);
  posts = NULL;
  nposts = 0;
  wgrid.n = 0;
  wgrid.height = 0;
  wpage = 0;
  wend = 0;
  werr[0] = 0;
  wloading = 1;
  list_want = 1;
  big_cancel = 1;
  job_q.site = cfg.site;
  job_q.ratings = cfg.ratings;
  job_q.sort = cfg.wsort;
  snprintf(job_q.q, sizeof job_q.q, "%s", cfg.wquery);
  pthread_cond_broadcast(&wcv);
  pthread_mutex_unlock(&wmx);
  web_clear_textures();
  wscroll = wscroll_v = 0;
  wsel = 0;
  cfg_save();
}
static void web_more(void) {
  pthread_mutex_lock(&wmx);
  if (!wloading && !wend && nposts) { wloading = 1; list_want = wpage + 1; pthread_cond_broadcast(&wcv); }
  pthread_mutex_unlock(&wmx);
}

// ---------- the full view ----------

static int wv;               // the post shown
static vita2d_texture *wv_tex;
static int wv_tex_post = -1, wv_loading, wv_failed;
static char wv_err[96];
static uint8_t *wv_raw;      // the sample's bytes (for a save)
static size_t wv_rawn;
static int wv_raw_post = -1;
static char wv_ext[8];
static LvVideo *wv_vid;
static float wzoom = 1, wpan_x, wpan_y, wswipe;
static int wdetails;
static float wdetails_y;
static unsigned wchrome_until;

static void wv_drop(void) {
  if (wv_vid) { video_close_later(wv_vid); wv_vid = NULL; }
  free_later(wv_tex);
  wv_tex = NULL;
  wv_tex_post = -1;
  free(wv_raw);
  wv_raw = NULL;
  wv_rawn = 0;
  wv_raw_post = -1;
}
static void wv_show(int k) {
  if (!nposts) return;
  wv = (k % nposts + nposts) % nposts;
  wv_drop();
  wzoom = 1; wpan_x = wpan_y = 0;
  wv_loading = 1;
  wv_failed = 0;
  wv_err[0] = 0;
  pthread_mutex_lock(&wmx);
  big_cancel = 1; // (the one before stops)
  big_job.post = wv;
  big_job.gen = wgen;
  big_job.save = 0;
  pthread_cond_broadcast(&wcv);
  pthread_mutex_unlock(&wmx);
  // (and the next ones: their previews ahead of time)
}
static void wv_save(void) {
  if (saving || wv >= nposts) return;
  if (is_saved(&posts[wv])) { sfx(SFX_ERROR); say("Already in your vault"); return; }
  if (!loaded_vault) { say("Open a vault first"); return; }
  int video = lv_post_is_video(&posts[wv]);
  if (wv_raw_post != wv || (!wv_raw && !video)) { say("Still loading…"); return; }
  saving = 1;
  pthread_mutex_lock(&wmx);
  big_job.post = wv;
  big_job.gen = wgen;
  big_job.save = 1;
  big_job.vgen = vgen;
  snprintf(big_job.cat, sizeof big_job.cat, "%s", save_category());
  big_job.raw = wv_raw; // (handed over: the full view doesn't need them once shown)
  big_job.rawn = wv_rawn;
  wv_raw = NULL;
  wv_raw_post = -1;
  pthread_cond_broadcast(&wcv);
  pthread_mutex_unlock(&wmx);
  say("Saving…");
}

static void library_grew(int before); // (main.c: the Library's per-item arrays follow the vault)
static void draw_bar(void);

static const char *save_category(void) {
  if (cfg.save_cat[0]) return cfg.save_cat;
  return LV_SITES[cfg.site].name;
}

static void web_take_results(void) {
  pthread_mutex_lock(&wmx);
  if (page_ready.ready) {
    LvPost *got = page_ready.posts;
    int count = page_ready.count;
    if (page_ready.err[0] || !got) { snprintf(werr, sizeof werr, "%s", page_ready.err); wend = 1; }
    else {
      if (count == 0) wend = 1;
      if (count > MAX_POSTS - nposts) count = MAX_POSTS - nposts;
      LvPost *np = realloc(posts, (nposts + count + 1) * sizeof *np);
      signed char *ns = realloc(wstate, nposts + count + 1);
      short *nso = realloc(wslot_of, (nposts + count + 1) * sizeof *nso);
      if (np) posts = np;
      if (ns) wstate = ns;
      if (nso) wslot_of = nso;
      if (np && ns && nso) {
        for (int k = 0; k < count; k++) { posts[nposts + k] = got[k]; wstate[nposts + k] = 0; wslot_of[nposts + k] = -1; }
        // (the ones not taken: freed with the array below)
        if (count < page_ready.count) { for (int k = count; k < page_ready.count; k++) { LvPost *q = &got[k]; free(q->preview); free(q->sample); free(q->file); free(q->page); free(q->source); free(q->tags); } }
        nposts += count;
        grid_build(&wgrid, nposts, post_aspect);

        if (nposts > 0 && page_ready.page > 1) sfx(SFX_MORE);
        free(got);
        got = NULL;
        wpage = page_ready.page;
        if (nposts >= MAX_POSTS) wend = 1;
      } else wend = 1;
    }
    if (got) lv_posts_free(got, page_ready.count);
    page_ready.ready = 0;
    page_ready.posts = NULL;
    wloading = 0;
    // picking up where it was: more pages until the post it was on, then back to it
    if (resume_sel >= 0) {
      if (resume_sel < nposts && resume_sel < wgrid.n) {
        wsel = resume_sel;
        Rect r = wgrid.r[wsel];
        wscroll = r.y - 40 > 0 ? r.y - 40 : 0;
        resume_sel = -1;
      } else if (!wend && wpage < resume_pages) { wloading = 1; list_want = wpage + 1; pthread_cond_broadcast(&wcv); }
      else resume_sel = -1;
    }
  }
  int n = nwdone;
  WDone got[24];
  memcpy(got, wdone, n * sizeof *got);
  nwdone = 0;
  pthread_mutex_unlock(&wmx);
  for (int k = 0; k < n; k++) {
    WDone *d = &got[k];
    // (a save is on the card whatever the search: only its vault matters, below)
    if (d->gen != wgen && d->kind != WD_SAVE) { free(d->px.rgba); free(d->a); free(d->b); continue; }
    if (d->kind == WD_PREVIEW) {
      int best = 0;
      for (int s = 1; s < WSLOTS; s++) if (wslots[s].used < wslots[best].used) best = s;
      if (wslots[best].tex && wslots[best].post >= 0 && wslots[best].post < nposts) { wstate[wslots[best].post] = 0; wslot_of[wslots[best].post] = -1; }
      wslots[best].tex = put_cell(SLOTS + best, &d->px);
      wslots[best].post = d->post;
      wslots[best].used = frame_no;
      if (d->post < nposts) { wstate[d->post] = wslots[best].tex ? 2 : 3; wslot_of[d->post] = (short)best; }
    } else if (d->kind == WD_SAMPLE) {
      if (d->post == wv) {
        wv_loading = 0;
        if (d->err[0]) { snprintf(wv_err, sizeof wv_err, "%s", d->err); wv_failed = !(d->px.rgba); }
        if (d->px.rgba) { free_later(wv_tex); wv_tex = upload(&d->px); wv_tex_post = d->post; }
        free(wv_raw);
        wv_raw = d->a; d->a = NULL;
        wv_rawn = d->an;
        wv_raw_post = d->post;
        snprintf(wv_ext, sizeof wv_ext, "%s", d->ext);
        if (wv < nposts && lv_post_is_video(&posts[wv]) && !d->err[0]) {
          wv_vid = lv_video_open(video_path, NULL, 1);
          if (wv_vid) lv_video_set_volume(wv_vid, cfg.volume * 32768 / 100);
          else snprintf(wv_err, sizeof wv_err, "This video can't be played on the Vita (it can still be saved)");
        }
      }
    } else if (d->kind == WD_SAVE) {
      saving = 0;
      if (d->err[0] || !d->added.uid[0]) { sfx(SFX_ERROR); say(d->err[0] ? d->err : "It couldn't be saved"); }
      else if (d->vgen == vgen && loaded_vault) {
        // on the card already (the save thread wrote it): now into the loaded vault
        int before = vault.n;
        pthread_mutex_lock(&vmx);
        int ok = lv_vault_add_memory(&vault, &d->nw, &d->added) == 0;
        pthread_mutex_unlock(&vmx);
        if (ok) library_grew(before);
        saved_add(d->nw.source);
        char m[160];
        snprintf(m, sizeof m, "Saved to %s (%zu KB)%s", d->cat, d->an / 1024, ok ? "" : ": it shows when the vault is opened again");
        sfx(SFX_SAVED);
        say(m);
      }
      free((char *)d->nw.tags);
      free((char *)d->nw.source);
    }
    free(d->px.rgba);
    free(d->a);
    free(d->b);
  }
}

static vita2d_texture *preview_of(int p) {
  int k = p < nposts ? wslot_of[p] : -1;
  if (p < nposts && wstate[p] == 2 && k >= 0 && wslots[k].post == p && wslots[k].tex) { wslots[k].used = frame_no; return wslots[k].tex; }
  return NULL;
}
static void ask_previews(const int *list, int n) {
  pthread_mutex_lock(&wmx);
  for (int k = 0; k < nwwant; k++) if (wwant[k] < nposts && wstate[wwant[k]] == 1) wstate[wwant[k]] = 0;
  nwwant = 0;
  for (int k = 0; k < n && nwwant < 48; k++)
    if (list[k] < nposts && wstate[list[k]] == 0) { wstate[list[k]] = 1; wwant[nwwant++] = list[k]; }
  if (nwwant) pthread_cond_broadcast(&wcv);
  pthread_mutex_unlock(&wmx);
}

// ---------- sheets: the sites, and the filter ----------

enum { SH_NONE, SH_SITES, SH_FILTER };
static int sheet, sheet_sel;

static void draw_sites_sheet(void) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 150));
  int w = 300, x = W - w - 12, y = 46, rh = 40;
  rrect(x, y, w, LV_NSITES * rh + 16, 16, SURF);
  for (int k = 0; k < LV_NSITES; k++) {
    int ry = y + 8 + k * rh;
    int on = k == sheet_sel;
    if (on) rrect(x + 6, ry, w - 12, rh - 4, 10, SURF2);
    if (k == cfg.site) circle(x + 22, ry + rh / 2 - 2, 5, ACC);
    text(x + 36, ry + 25, k == cfg.site ? ACC : TEXT, 17, LV_SITES[k].name);
    const char *kinds[] = {"Danbooru", "Gelbooru", "Moebooru", "e621"};
    text(x + w - 18 - text_w(13, kinds[LV_SITES[k].kind]), ry + 24, FAINT, 13, kinds[LV_SITES[k].kind]);
    if (hit(x, ry, w, rh)) { sheet_sel = k; in.pressed |= SCE_CTRL_CROSS; }
  }
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) sheet_sel = (sheet_sel + 1) % LV_NSITES;
  if (in.pressed & SCE_CTRL_UP) sheet_sel = (sheet_sel + LV_NSITES - 1) % LV_NSITES;
  if (in.pressed & SCE_CTRL_CROSS) { sfx(SFX_SELECT); sheet = SH_NONE; if (sheet_sel != cfg.site) { cfg.site = sheet_sel; if (!LV_SITES[cfg.site].hot && cfg.wsort == LV_SORT_HOT) cfg.wsort = LV_SORT_LATEST; web_search(); } }
  else if ((in.pressed & SCE_CTRL_CIRCLE) || (in.tap && !(in.tap_x >= x && in.tap_y < y + LV_NSITES * rh + 16))) { sfx(SFX_CLOSE); sheet = SH_NONE; }
  in.pressed = 0;
  in.tap = 0;
}

static void draw_filter_sheet(void) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 150));
  int w = 420, x = W - w - 12, y = 46, rh = 44;
  enum { R_SEARCH, R_G, R_S, R_Q, R_E, R_CAT, R_N };
  rrect(x, y, w, R_N * rh + 18, 16, SURF);
  for (int k = 0; k < R_N; k++) {
    int ry = y + 9 + k * rh;
    if (k == sheet_sel) rrect(x + 6, ry, w - 12, rh - 4, 10, SURF2);
    char v[160];
    if (k == R_SEARCH) {
      text(x + 20, ry + 26, TEXT, 17, "Search");
      text_fit(x + 120, ry + 26, cfg.wquery[0] ? ACC : FAINT, 16, cfg.wquery[0] ? cfg.wquery : "tags, like blue_hair smile", w - 140);
    } else if (k <= R_E) {
      int bit = 1 << (k - R_G);
      text(x + 20, ry + 26, TEXT, 17, RATING_NAMES[k - R_G]);
      int on = (cfg.ratings & bit) != 0;
      rrect(x + w - 66, ry + 8, 44, 24, 12, on ? ACC : SURF2);
      circle(on ? x + w - 34 : x + w - 54, ry + 20, 9, TEXT);
    } else {
      text(x + 20, ry + 26, TEXT, 17, "Save into");
      snprintf(v, sizeof v, "‹ %s ›", save_category());
      text(x + w - 22 - text_w(16, v), ry + 26, ACC, 16, v);
    }
    if (hit(x, ry, w, rh)) { sheet_sel = k; in.pressed |= SCE_CTRL_CROSS; }
  }
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) sheet_sel = (sheet_sel + 1) % R_N;
  if (in.pressed & SCE_CTRL_UP) sheet_sel = (sheet_sel + R_N - 1) % R_N;
  int d = (in.pressed & SCE_CTRL_RIGHT) ? 1 : (in.pressed & SCE_CTRL_LEFT) ? -1 : (in.pressed & SCE_CTRL_CROSS) ? 1 : 0;
  if (d) {
    if (sheet_sel == R_SEARCH && (in.pressed & SCE_CTRL_CROSS) && !ime_open) ime_start(3, "Search tags", cfg.wquery, 0);
    else if (sheet_sel >= R_G && sheet_sel <= R_E && (in.pressed & SCE_CTRL_CROSS)) {
      int bit = 1 << (sheet_sel - R_G);
      if ((cfg.ratings & ~bit) & LV_R_ALL || !(cfg.ratings & bit)) { sfx(SFX_TOGGLE); cfg.ratings ^= bit; web_search(); }
      else { sfx(SFX_ERROR); say("At least one rating stays on"); }
    } else if (sheet_sel == R_CAT) {
      // the site's name, then the vault's categories
      int cur = -1;
      for (int c = 0; c < vault.ncats; c++) if (!strcmp(vault.cats[c], cfg.save_cat)) cur = c;
      int nx = cfg.save_cat[0] ? cur + d : (d > 0 ? 0 : vault.ncats - 1);
      if (nx < 0 || nx >= vault.ncats) cfg.save_cat[0] = 0;
      else snprintf(cfg.save_cat, sizeof cfg.save_cat, "%s", vault.cats[nx]);
      sfx(SFX_TOGGLE);
      cfg_save();
    }
  }
  if ((in.pressed & SCE_CTRL_CIRCLE) || (in.pressed & SCE_CTRL_SQUARE) || (in.tap && !(in.tap_x >= x && in.tap_y < y + R_N * rh + 18))) { sfx(SFX_CLOSE); sheet = SH_NONE; }
  in.pressed = 0;
  in.tap = 0;
}

// ---------- the grid ----------

static void draw_web(void) {
  web_start();
  if (!saved_src && loaded_vault) saved_build();
  if (!nposts && !wloading && !wend && !werr[0]) {
    web_search();
    if (!resume_checked) { resume_checked = 1; resume_pages = cfg.wpages; resume_sel = cfg.wpages > 0 ? cfg.wsel : -1; }
  }
  // (remembered as it goes, for next time)
  cfg.wpages = wpage;
  cfg.wsel = wsel;
  float content = wgrid.height + 60;
  float view_h = H - TOP - BAR;
  float max_scroll = fmaxf(0, content - view_h);
  int sheet_open = sheet != SH_NONE;

  if (!sheet_open) {
    if (in.touching && in.ty > TOP && in.ty < H - BAR) { wscroll -= in.drag_dy; wscroll_v = 0; }
    if (in.flicked && in.down_y > TOP) wscroll_v = -in.fvy * 1.6f;
    wscroll += wscroll_v + in.ly * 14;
    wscroll_v *= 0.94f;
    if (fabsf(wscroll_v) < 0.2f) wscroll_v = 0;
    int before = wsel;
    if (in.pressed & SCE_CTRL_RIGHT) wsel++;
    if (in.pressed & SCE_CTRL_LEFT) wsel--;
    if (wsel >= nposts) wsel = nposts - 1;
    if (wsel < 0) wsel = 0;
    if (in.pressed & SCE_CTRL_DOWN) wsel = grid_move(&wgrid, wsel, 1);
    if (in.pressed & SCE_CTRL_UP) wsel = grid_move(&wgrid, wsel, -1);
    if (wsel != before && wsel < wgrid.n) {
      sfx(SFX_TICK);
      Rect r = wgrid.r[wsel];
      if (r.y < wscroll) wscroll = r.y;
      else if (r.y + r.h > wscroll + view_h) wscroll = r.y + r.h - view_h + 10;
    }
    if (wscroll < 0 && !in.touching) { wscroll *= 0.7f; wscroll_v = 0; }
    if (wscroll > max_scroll && !in.touching) { wscroll = max_scroll + (wscroll - max_scroll) * 0.7f; wscroll_v = 0; }
  }

  int first = grid_first(&wgrid, wscroll - 8);
  static int wlast_top_row = -1;
  if (first < wgrid.n && wgrid.row[first] != wlast_top_row) { if (wlast_top_row >= 0 && (in.touching || wscroll_v != 0 || in.ly)) sfx(SFX_TICK); wlast_top_row = wgrid.row[first]; }
  int wanted[48], nw = 0, last = first;
  for (int k = first; k < nposts && k < wgrid.n; k++) {
    Rect rc = wgrid.r[k];
    float x = rc.x, y = TOP + 6 + rc.y - wscroll, cw = rc.w, ch = rc.h;
    if (y > H) break;
    last = k + 1;
    if (y + ch < TOP - 4) continue;
    LvPost *p = &posts[k];
    vita2d_texture *t = preview_of(k);
    int on = k == wsel && !in.touching && !sheet_open;
    float g = on ? 4 : 0;
    if (on) rrect(x - g - 3, y - g - 3, cw + 2 * g + 6, ch + 2 * g + 6, 12, ACC);
    if (t) draw_cover(t, x - g, y - g, cw + 2 * g, ch + 2 * g, RGBA(255, 255, 255, 255));
    else {
      rrect(x - g, y - g, cw + 2 * g, ch + 2 * g, 8, SURF);
      if (wstate[k] == 3) text_center(x + cw / 2, y + ch / 2, FAINT, 14, "No preview");
      else if (nw < 48) wanted[nw++] = k;
    }
    const char *badge = lv_post_is_video(p) ? "VIDEO" : !strcmp(p->ext, "gif") ? "GIF" : NULL;
    if (badge) {
      int tw = text_w(12, badge);
      rrect(x + cw - tw - 16, y + ch - 26, tw + 12, 20, 10, RGBA(0, 0, 0, 150));
      text(x + cw - tw - 10, y + ch - 11, TEXT, 12, badge);
    }
    if (is_saved(p)) { circle(x + 17, y + 17, 11, ACC); text_center(x + 17, y + 23, RGBA(20, 12, 16, 255), 14, "\u2713"); }
    if (!sheet_open && hit((int)x, (int)y, (int)cw, (int)ch) && in.tap_y > TOP && in.tap_y < H - BAR) { wsel = k; sfx(SFX_SELECT); wdetails = 0; wdetails_y = 0; wchrome_until = frame_no + 180; screen = S_WEBVIEW; wv_show(k); return; }
  }
  for (int k = last; k < last + 18 && k < nposts && nw < 48; k++) if (!preview_of(k) && wstate[k] != 3) wanted[nw++] = k;
  ask_previews(wanted, nw);
  // the next page, before the end is reached
  if (last + 12 >= nposts) web_more();
  if (!nposts) {
    if (!lv_net_online()) text_center(W / 2, TOP + 150, DIM, 20, "Not connected: turn on Wi-Fi in the Vita's settings");
    else if (wloading) {
      for (int k = 0; k < 8; k++) { float a = frame_no * 0.12f + k * 0.785f; circle(W / 2 + cosf(a) * 22, TOP + 150 + sinf(a) * 22, 3 + k * 0.5f, with_alpha(ACC, 80 + k * 20)); }
    } else text_center(W / 2, TOP + 150, DIM, 18, werr[0] ? werr : "Nothing found: try other tags or ratings");
  } else if (wloading && last >= nposts) text_center(W / 2, H - BAR - 14, FAINT, 14, "Loading more…");
  else if (wend && werr[0] && last >= nposts) text_center(W / 2, H - BAR - 14, FAINT, 14, werr);
  if (max_scroll > 0) {
    float bh = fmaxf(30, view_h * view_h / content);
    float by = TOP + (view_h - bh) * fminf(1, fmaxf(0, wscroll / max_scroll));
    rrect(W - 6, by, 4, bh, 2, with_alpha(TEXT, 70));
  }

  // --- the top: the site (the tab button), the search, the filter and the sort ---
  vita2d_draw_rectangle(0, 0, W, TOP, with_alpha(BG, 250));
  text(16, 28, TEXT, 22, "Browser");
  char site[80];
  snprintf(site, sizeof site, "%s  ▾", LV_SITES[cfg.site].name);
  int sx = 16 + text_w(22, "Browser") + 14;
  int ws = text_w(16, site) + 28;
  rrect(sx, 8, ws, 28, 14, ACC);
  text(sx + 14, 28, RGBA(20, 12, 16, 255), 16, site);
  if (!sheet_open && hit(sx, 0, ws, 40)) { sfx(SFX_OPEN); sheet = SH_SITES; sheet_sel = cfg.site; }
  // filter and sort on the right
  char sortl[40], filt[40];
  snprintf(sortl, sizeof sortl, "⇅ %s", LV_SORT_NAMES[cfg.wsort]);
  int nr = 0;
  for (int k = 0; k < 4; k++) nr += (cfg.ratings >> k) & 1;
  if (nr == 4) snprintf(filt, sizeof filt, "Filter: all");
  else { snprintf(filt, sizeof filt, "Filter:"); for (int k = 0; k < 4; k++) if (cfg.ratings & (1 << k)) { char c[3] = {' ', RATING_NAMES[k][0], 0}; strcat(filt, c); } }
  int wsr = text_w(15, sortl) + 26, wf = text_w(15, filt) + 26;
  int rx = W - 16 - wsr;
  rrect(rx, 8, wsr, 26, 13, SURF2);
  text(rx + 13, 26, TEXT, 15, sortl);
  if (!sheet_open && (hit(rx, 0, wsr, 36) || (in.pressed & SCE_CTRL_TRIANGLE))) {
    sfx(SFX_TOGGLE);
    do cfg.wsort = (cfg.wsort + 1) % LV_NSORTS; while (cfg.wsort == LV_SORT_HOT && !LV_SITES[cfg.site].hot);
    web_search();
  }
  rx -= wf + 8;
  rrect(rx, 8, wf, 26, 13, nr == 4 ? SURF2 : with_alpha(ACC, 90));
  text(rx + 13, 26, TEXT, 15, filt);
  if (!sheet_open && (hit(rx, 0, wf, 36) || (in.pressed & SCE_CTRL_SQUARE))) { sfx(SFX_OPEN); sheet = SH_FILTER; sheet_sel = 0; in.pressed &= ~SCE_CTRL_SQUARE; }
  // the search, under the title
  char q[180];
  snprintf(q, sizeof q, "%s", cfg.wquery[0] ? cfg.wquery : "Search tags…");
  int wq = text_w(15, q) + 28;
  if (wq > W - 32) wq = W - 32;
  rrect(16, 38, wq, 24, 12, SURF2);
  text_fit(30, 55, cfg.wquery[0] ? TEXT : FAINT, 15, q, wq - 28);
  if (!sheet_open && hit(16, 36, wq, 30) && !ime_open) ime_start(3, "Search tags", cfg.wquery, 0);
  if (cfg.wquery[0]) {
    int cx = 16 + wq + 6;
    rrect(cx, 38, 26, 24, 12, SURF2);
    text_center(cx + 13, 55, DIM, 14, "✕");
    if (!sheet_open && hit(cx, 36, 30, 30)) { cfg.wquery[0] = 0; web_search(); }
  }

  if (!sheet_open) {
    // the sites are tabs: L / R go through them
    if (in.pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
      sfx(SFX_TAB);
      cfg.site = (cfg.site + ((in.pressed & SCE_CTRL_RTRIGGER) ? 1 : LV_NSITES - 1)) % LV_NSITES;
      if (!LV_SITES[cfg.site].hot && cfg.wsort == LV_SORT_HOT) cfg.wsort = LV_SORT_LATEST;
      web_search();
      char m[80];
      snprintf(m, sizeof m, "%s", LV_SITES[cfg.site].name);
      say(m);
    }
    if ((in.pressed & SCE_CTRL_CROSS) && nposts) { sfx(SFX_SELECT); wdetails = 0; wdetails_y = 0; wchrome_until = frame_no + 180; screen = S_WEBVIEW; wv_show(wsel); return; }
    if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_TAB); screen = S_SHOW; plan_sel = 0; }
    if (in.pressed & SCE_CTRL_START) { sfx(SFX_OPEN); sheet = SH_SITES; sheet_sel = cfg.site; }
  }
  draw_bar();
  if (sheet == SH_SITES) draw_sites_sheet();
  else if (sheet == SH_FILTER) draw_filter_sheet();
}

// ---------- the full view's drawing ----------

static void draw_webview(void) {
  if (!nposts) { screen = S_WEB; return; }
  LvPost *p = &posts[wv];
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 255));
  vita2d_texture *t = NULL;
  int is_frame = 0;
  if (wv_vid) { t = lv_video_frame(wv_vid); is_frame = t != NULL; }
  if (!t && wv_tex_post == wv) t = wv_tex;
  if (!t) t = preview_of(wv);
  float avail_h = H - wdetails_y * 300;
  if (t) {
    float tw = is_frame ? lv_video_width(wv_vid) : vita2d_texture_get_width(t);
    float th = is_frame ? lv_video_height(wv_vid) : vita2d_texture_get_height(t);
    float s = fminf(W / tw, avail_h / th) * wzoom;
    float dw = tw * s, dh = th * s;
    float mx = fmaxf(0, (dw - W) / 2), my = fmaxf(0, (dh - avail_h) / 2);
    wpan_x = fminf(mx, fmaxf(-mx, wpan_x));
    wpan_y = fminf(my, fmaxf(-my, wpan_y));
    vita2d_draw_texture_scale(t, (W - dw) / 2 + wpan_x + wswipe, (avail_h - dh) / 2 + wpan_y, s, s);
  }
  if (wv_loading) {
    float f = big_progress;
    rrect(W / 2 - 120, H - 30, 240, 6, 3, with_alpha(TEXT, 50));
    if (f > 0) rrect(W / 2 - 120, H - 30, 240 * fminf(1, f), 6, 3, ACC);
    if (lv_post_is_video(p)) text_center(W / 2, H - 40, DIM, 14, "Downloading the video…");
  }
  if (wv_err[0]) text_center(W / 2, (int)avail_h - 50, ACC2, 15, wv_err);

  // --- touch: swipe, pinch, drag when zoomed, tap for the controls ---
  static float pd0, z0;
  if (in.touching && wdetails_y < 0.5f) {
    if (in.tcount >= 2) {
      float d = hypotf(in.t2x - in.tx, in.t2y - in.ty);
      if (pd0 == 0) { pd0 = d; z0 = wzoom; } else wzoom = fminf(6, fmaxf(1, z0 * d / pd0));
    } else {
      pd0 = 0;
      if (wzoom > 1.01f) { wpan_x += in.drag_dx; wpan_y += in.drag_dy; }
      else if (fabsf((float)(in.tx - in.down_x)) > fabsf((float)(in.ty - in.down_y))) wswipe += in.drag_dx;
      else if (in.ty - in.down_y < -60) wdetails = 1;
    }
  } else {
    pd0 = 0;
    if (wswipe > 120) { wswipe = 0; sfx(SFX_TICK); wv_show(wv - 1); return; }
    if (wswipe < -120) { wswipe = 0; sfx(SFX_TICK); wv_show(wv + 1); return; }
    wswipe *= 0.7f;
    if (wzoom < 1.01f) wzoom = 1;
  }
  if (in.dbl) { wzoom = wzoom > 1.5f ? 1 : 2.5f; wpan_x = (W / 2.0f - in.tap_x) * (wzoom - 1); wpan_y = (H / 2.0f - in.tap_y) * (wzoom - 1); }
  else if (in.tap && wdetails_y < 0.5f) {
    if (frame_no < wchrome_until && in.tap_x < 70 && in.tap_y < 60) { sfx(SFX_BACK); wv_drop(); screen = S_WEB; return; }
    else if (frame_no < wchrome_until && in.tap_x > W - 70 && in.tap_y < 60) wdetails = 1;
    else if (frame_no < wchrome_until && in.tap_x > W - 170 && in.tap_x < W - 70 && in.tap_y < 60) wv_save();
    else wchrome_until = frame_no < wchrome_until ? frame_no : frame_no + 240;
  }

  // --- buttons ---
  if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER)) { sfx(SFX_TICK); wv_show(wv + 1); return; }
  if (in.pressed & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER)) { sfx(SFX_TICK); wv_show(wv - 1); return; }
  if (in.pressed & SCE_CTRL_CIRCLE) { if (wdetails) { sfx(SFX_CLOSE); wdetails = 0; } else { sfx(SFX_BACK); wv_drop(); wsel = wv; screen = S_WEB; return; } }
  if (in.pressed & SCE_CTRL_TRIANGLE) { sfx(wdetails ? SFX_CLOSE : SFX_OPEN); wdetails = !wdetails; }
  if (in.pressed & SCE_CTRL_SQUARE) { wv_save(); wchrome_until = frame_no + 120; }
  if (in.pressed & SCE_CTRL_CROSS) {
    if (wv_vid) lv_video_pause(wv_vid, !lv_video_paused(wv_vid));
    else { wzoom = wzoom > 1.5f ? 1 : 2.5f; wpan_x = wpan_y = 0; }
  }
  if (wzoom > 1.01f) { wpan_x -= in.lx * 12; wpan_y -= in.ly * 12; }

  // --- the controls on top ---
  if (frame_no < wchrome_until || wdetails) {
    int a = frame_no + 30 > wchrome_until && !wdetails ? (int)((wchrome_until - frame_no) * 8) : 240;
    if (a > 240 || a < 0) a = 240;
    for (int k = 0; k < 64; k++) vita2d_draw_rectangle(0, k, W, 1, RGBA(0, 0, 0, (64 - k) * a / 400));
    vita2d_draw_line(30, 30, 42, 18, with_alpha(TEXT, a));
    vita2d_draw_line(30, 30, 42, 42, with_alpha(TEXT, a));
    vita2d_draw_line(31, 30, 43, 18, with_alpha(TEXT, a));
    vita2d_draw_line(31, 30, 43, 42, with_alpha(TEXT, a));
    char pos[80];
    snprintf(pos, sizeof pos, "%s  ·  %d of %d%s", LV_SITES[cfg.site].name, wv + 1, nposts, wend ? "" : "+");
    text_center(W / 2, 36, with_alpha(TEXT, a), 16, pos);
    int saved = is_saved(p);
    const char *sv = saved ? "✓ Saved" : saving ? "Saving…" : "↓ Save";
    int sw = text_w(15, sv) + 24;
    rrect(W - 72 - sw, 16, sw, 28, 14, saved ? with_alpha(SURF2, a) : with_alpha(ACC, a));
    text(W - 72 - sw + 12, 35, saved ? with_alpha(TEXT, a) : RGBA(20, 12, 16, a), 15, sv);
    circle(W - 38, 30, 13, with_alpha(TEXT, a));
    circle(W - 38, 30, 11, RGBA(0, 0, 0, a));
    text_center(W - 38, 36, with_alpha(TEXT, a), 15, "i");
    if (wv_vid) {
      uint64_t d = lv_video_duration(wv_vid), q = lv_video_pos(wv_vid);
      float f = d ? fminf(1, q / (float)d) : 0;
      rrect(40, H - 34, W - 80, 6, 3, with_alpha(TEXT, a / 3));
      rrect(40, H - 34, (W - 80) * f, 6, 3, with_alpha(ACC, a));
    }
  }

  // --- the details, pulled up ---
  wdetails_y += ((wdetails ? 1 : 0) - wdetails_y) * 0.25f;
  if (wdetails_y > 0.01f) {
    float top = H - wdetails_y * 300;
    rrect(0, top, W, 330, 20, with_alpha(SURF, 250));
    rrect(W / 2 - 24, top + 8, 48, 5, 3, with_alpha(TEXT, 60));
    char line[300];
    snprintf(line, sizeof line, "%s #%lld", LV_SITES[cfg.site].name, p->id);
    text(30, top + 46, TEXT, 22, line);
    int ri = p->rating == LV_R_EXPLICIT ? 3 : p->rating == LV_R_QUESTIONABLE ? 2 : p->rating == LV_R_SENSITIVE ? 1 : 0;
    snprintf(line, sizeof line, "%s  ·  %s  ·  score %d  ·  %d × %d", RATING_NAMES[ri], p->ext, p->score, p->width, p->height);
    text(30, top + 76, DIM, 15, line);
    text_fit(30, top + 100, FAINT, 13, p->page, W - 60);
    float tx = 30, ty = top + 116;
    char tags[4096];
    snprintf(tags, sizeof tags, "%s", p->tags);
    for (char *s = strtok(tags, " "); s; s = strtok(NULL, " ")) {
      int w = text_w(14, s) + 22;
      if (tx + w > W - 30) { tx = 30; ty += 32; }
      if (ty > H - 30) break;
      rrect(tx, ty, w, 26, 13, SURF2);
      text(tx + 11, ty + 18, TEXT, 14, s);
      if (hit((int)tx, (int)ty, w, 26)) {
        // a tag: a new search for it
        snprintf(cfg.wquery, sizeof cfg.wquery, "%s", s);
        wv_drop();
        screen = S_WEB;
        web_search();
        return;
      }
      tx += w + 6;
    }
    if (in.touching && in.down_y > top && in.ty - in.down_y > 50) wdetails = 0;
  }
}
