#include <ctype.h>
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
  int post, gen, save, vgen, full, hires;
  uint8_t *raw; // (a save: the sample's bytes, handed over; a video's are in its file; hires: a copy)
  size_t rawn;
  char cat[64];
} big_job = {-1, 0, 0, 0, 0, 0, NULL, 0, ""};
// the search a page is for, copied when asked (the settings may change while it loads)
static struct { int site, ratings, sort, span; char q[128], key[200], hidden[512]; } job_q;
static int saving;
static const char *save_category(void);

// ---------- what's kept: recent searches, bookmarks, hidden tags, account keys ----------
// (ux0:data/LWDV00001/web_recent.txt and web_marks.txt: "<site>\t<search>" a line; web_hidden.txt:
// the hidden tags on one line; web_keys.txt: "<site name>\t<user id> <api key>" a line)
typedef struct { int site; char q[128]; } WSearch;
static WSearch wrecent[10], wmarks[30];
static int nwrecent, nwmarks, wlists_loaded;
static char whidden[512];
static char wkeys[32][200]; // (per site)
static char wkey_user[80];
static int wkey_ask;
static void wlist_load(const char *path, WSearch *a, int cap, int *n) {
  *n = 0;
  FILE *f = fopen(path, "r");
  char ln[200];
  while (f && *n < cap && fgets(ln, sizeof ln, f)) {
    ln[strcspn(ln, "\r\n")] = 0;
    char *t = strchr(ln, '\t');
    if (!t) continue;
    *t = 0;
    int s = atoi(ln);
    if (s < 0 || s >= LV_NSITES) continue;
    a[*n].site = s;
    snprintf(a[*n].q, sizeof a[*n].q, "%s", t + 1);
    (*n)++;
  }
  if (f) fclose(f);
}
static void wlist_save(const char *path, const WSearch *a, int n) {
  char part[200];
  snprintf(part, sizeof part, "%s.part", path);
  FILE *f = fopen(part, "w");
  if (!f) return;
  for (int k = 0; k < n; k++) fprintf(f, "%d\t%s\n", a[k].site, a[k].q);
  fclose(f);
  sceIoRemove(path);
  sceIoRename(part, path);
}
static void hidden_save(void) {
  FILE *f = fopen(DATA "/web_hidden.txt", "w");
  if (f) { fprintf(f, "%s\n", whidden); fclose(f); }
}
static void wkeys_save(void) {
  FILE *f = fopen(DATA "/web_keys.txt", "w");
  if (!f) return;
  for (int k = 0; k < LV_NSITES && k < 32; k++) if (wkeys[k][0]) fprintf(f, "%s\t%s\n", LV_SITES[k].name, wkeys[k]);
  fclose(f);
}
static void wlists_load(void) {
  if (wlists_loaded) return;
  wlists_loaded = 1;
  wlist_load(DATA "/web_recent.txt", wrecent, 10, &nwrecent);
  wlist_load(DATA "/web_marks.txt", wmarks, 30, &nwmarks);
  FILE *f = fopen(DATA "/web_hidden.txt", "r");
  if (f) { if (fgets(whidden, sizeof whidden, f)) whidden[strcspn(whidden, "\r\n")] = 0; fclose(f); }
  f = fopen(DATA "/web_keys.txt", "r");
  char ln[300];
  while (f && fgets(ln, sizeof ln, f)) {
    ln[strcspn(ln, "\r\n")] = 0;
    char *t = strchr(ln, '\t');
    if (!t) continue;
    *t = 0;
    for (int k = 0; k < LV_NSITES && k < 32; k++) if (!strcmp(LV_SITES[k].name, ln)) snprintf(wkeys[k], sizeof wkeys[k], "%s", t + 1);
  }
  if (f) fclose(f);
}
static int wsearch_find(const WSearch *a, int n, int site, const char *q) {
  for (int k = 0; k < n; k++) if (a[k].site == site && !strcmp(a[k].q, q)) return k;
  return -1;
}
static void recent_add(int site, const char *q) {
  if (!q[0]) return;
  int k = wsearch_find(wrecent, nwrecent, site, q);
  if (k == 0) return;
  if (k < 0) k = nwrecent < 10 ? nwrecent++ : 9;
  memmove(&wrecent[1], &wrecent[0], k * sizeof *wrecent);
  wrecent[0].site = site;
  snprintf(wrecent[0].q, sizeof wrecent[0].q, "%s", q);
  wlist_save(DATA "/web_recent.txt", wrecent, nwrecent);
}
static void wsearch_drop(WSearch *a, int *n, int k, const char *path) {
  if (k < 0 || k >= *n) return;
  memmove(&a[k], &a[k + 1], (*n - k - 1) * sizeof *a);
  (*n)--;
  wlist_save(path, a, *n);
}
// a post with one of the hidden tags (a whole tag, any case)
static int has_hidden(const char *tags, const char *hidden) {
  const char *h = hidden;
  while (*h) {
    while (*h == ' ') h++;
    const char *e = h;
    while (*e && *e != ' ') e++;
    size_t n = (size_t)(e - h);
    for (const char *t = tags; n && *t;) {
      while (*t == ' ') t++;
      const char *te = t;
      while (*te && *te != ' ') te++;
      if ((size_t)(te - t) == n && !strncasecmp(t, h, n)) return 1;
      t = te;
    }
    h = e;
  }
  return 0;
}
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
  pin_to_core(2); // (off the drawing thread's core)
  LvHttp *h = lv_http_new();
  while (worker_run) {
    pthread_mutex_lock(&wmx);
    while (worker_run && !list_want) pthread_cond_wait(&wcv, &wmx);
    int page = list_want, gen = wgen, ratings = job_q.ratings;
    list_want = 0;
    const LvSite *s = &LV_SITES[job_q.site];
    char url[2048];
    lv_booru_url_ex(s, job_q.q, ratings, job_q.sort, job_q.span, job_q.key, page, url, sizeof url);
    char hidden[512];
    snprintf(hidden, sizeof hidden, "%s", job_q.hidden);
    pthread_mutex_unlock(&wmx);
    if (!worker_run) break;
    uint8_t *b = NULL;
    size_t n = 0;
    const char *err = NULL;
    int st = lv_http_get(h, url, &b, &n, 8 * 1024 * 1024, NULL, NULL, &err);
    int count = 0;
    LvPost *got = st == 200 ? lv_booru_parse(s, (const char *)b, n, ratings, &count) : NULL;
    // (the hidden tags: those posts never reach the grid)
    if (got && hidden[0]) {
      int kept = 0;
      for (int k = 0; k < count; k++) {
        if (has_hidden(got[k].tags, hidden)) { LvPost *q = &got[k]; free(q->preview); free(q->sample); free(q->file); free(q->page); free(q->source); free(q->tags); continue; }
        got[kept++] = got[k];
      }
      // (a page all hidden still counts as a page: more come)
      count = kept;
    }
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
  pin_to_core(1); // (off the drawing thread's core)
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
static void prepare_save(LvHttp *h, const LvPost *p, uint8_t *raw, size_t rawn, WDone *d, int full) {
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
  if (full) {
    // the original as it is, its own format (the PC opens it at its full size)
    d->a = raw; d->an = rawn; d->w = w; d->h = hh; raw = NULL;
    snprintf(d->ext, sizeof d->ext, "%s", !strcmp(p->ext, "jpeg") ? "jpg" : p->ext);
  } else if (lv_jpeg_size(raw, rawn, &w, &hh) && w <= mw && hh <= mh) {
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
  saver_flush(); // (a rewrite of index.csv waiting to be written goes first: this appends to it)
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
  pin_to_core(2); // (off the drawing thread's core)
  LvHttp *h = lv_http_new();
  while (worker_run) {
    pthread_mutex_lock(&wmx);
    while (worker_run && big_job.post < 0) pthread_cond_wait(&wcv, &wmx);
    if (!worker_run) { pthread_mutex_unlock(&wmx); break; }
    int p = big_job.post, gen = big_job.gen, save = big_job.save, vgen_asked = big_job.vgen, full = big_job.full, hires = big_job.hires;
    big_job.hires = 0;
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
      // full size: the original instead of the sample (pictures; GIFs and videos are whole anyway)
      if (full && !lv_post_is_video(&post) && strcmp(post.ext, "gif") && strcmp(post.file, post.sample)) {
        uint8_t *b = NULL;
        size_t n = 0;
        const char *err = NULL;
        if (lv_http_get(h, post.file, &b, &n, 48u * 1024 * 1024, &big_cancel, &big_progress, &err) == 200 && n) { free(raw); raw = b; rawn = n; }
        else { free(b); full = 0; }
      }
      prepare_save(h, &post, raw, rawn, &d, full);
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
      if (hires && raw) {
        // zoomed in: the sample again, from the bytes already here, at up to 1920 px
        b = raw; raw = NULL; n = rawn;
        st = 200;
      } else if (video) {
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
        if (lv_decode(b, n, hires ? 1920 : W, hires ? 1920 : H, &d.px) != 0) {
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

static void custom_sites_load(void);
static void web_start(void) {
  if (web_started) return;
  web_started = 1;
  custom_sites_load();
  lv_net_init();
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024); // (they decode pictures)
  // (a thread that won't start, the memory being short for a moment: tried again next time)
  if (pthread_create(&t, &at, list_main, NULL) != 0) { web_started = 0; return; }
  if (pthread_create(&t, &at, preview_main, NULL) != 0) LOG("a preview thread didn't start\n");
  pthread_create(&t, &at, preview_main, NULL);
  if (pthread_create(&t, &at, sample_main, NULL) != 0) LOG("the sample thread didn't start\n");
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
  job_q.span = lv_booru_has_spans(&LV_SITES[cfg.site]) ? cfg.wspan : 0;
  snprintf(job_q.q, sizeof job_q.q, "%s", cfg.wquery);
  wlists_load();
  snprintf(job_q.key, sizeof job_q.key, "%s", wkeys[cfg.site]);
  snprintf(job_q.hidden, sizeof job_q.hidden, "%s", whidden);
  recent_add(cfg.site, cfg.wquery);
  if (LV_SITES[cfg.site].needs_key && !wkeys[cfg.site][0]) {
    // (no key: nothing asked, and the grid says where to get one)
    list_want = 0;
    wloading = 0;
    wend = 1;
    snprintf(werr, sizeof werr, "%s needs your account's API key: \u25a1 Filter \u2192 API key", LV_SITES[cfg.site].name);
  }
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

static int wv_back = S_WEB;  // (where the full view goes back to: the grid, or the video browser)
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
  release_tex(wv_tex);
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
  big_job.full = cfg.wfull;
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
  // (at most six a frame: the rest the next)
  int n = nwdone < 6 ? nwdone : 6;
  WDone got[24];
  memcpy(got, wdone, n * sizeof *got);
  memmove(wdone, wdone + n, (nwdone - n) * sizeof *wdone);
  nwdone -= n;
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
        if (d->px.rgba) { release_tex(wv_tex); wv_tex = screen_upload(1, &d->px); wv_tex_post = d->post; }
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

enum { SH_NONE, SH_SITES, SH_FILTER, SH_SEARCH };
static int sheet, sheet_sel;

// (the video sources: vbrowse.c)
static int nvsources(void);
static const char *vsource_name(int k);
static const char *vsource_icon(int k);
static const char *vsource_about(int k);
static int vsource_blocked(int k);
static void vb_open(int k);
// (the manga sources: manga_ui.c)
static const char *msource_name(int k);
static const char *msource_icon(int k);
static const char *msource_about(int k);
static int nmsources(void);
static int mg_src_now(void);
static void manga_open(int src);
static int vb_cur = -1;
// sites added by hand (ux0:data/LWDV00001/web_sites.txt: "<name>\t<kind>\t<address>" a line)
// The sites offered (not the hidden ones), in order: tile k is site vis[k]
static int sites_visible(int *vis) {
  int n = 0;
  for (int k = 0; k < LV_NSITES; k++) if (!LV_SITES[k].hidden) vis[n++] = k;
  return n;
}
static int site_tile_of(int site) {
  int vis[LV_MAX_SITES], n = sites_visible(vis);
  for (int k = 0; k < n; k++) if (vis[k] == site) return k;
  return 0;
}
static void custom_sites_load(void) {
  FILE *f = fopen(DATA "/web_sites.txt", "r");
  char ln[300];
  while (f && fgets(ln, sizeof ln, f)) {
    ln[strcspn(ln, "\r\n")] = 0;
    char *a = strchr(ln, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
    if (!a || !b) continue;
    *a = 0; *b = 0;
    lv_booru_add_site(ln, atoi(a + 1), b + 1);
  }
  if (f) fclose(f);
  if (cfg.site >= LV_NSITES || LV_SITES[cfg.site].hidden) cfg.site = 0;
}
static void custom_sites_save(void) {
  FILE *f = fopen(DATA "/web_sites.txt", "w");
  for (int k = LV_NBUILTIN; f && k < LV_NSITES; k++) fprintf(f, "%s\t%d\t%s\n", LV_SITES[k].name, LV_SITES[k].kind, LV_SITES[k].base);
  if (f) fclose(f);
}
// Adding one: which of the four kinds answers at the address (on a thread of its own)
static char add_base[160];
static volatile int add_state; // 0 idle, 1 asking, 2 found (add_kind), 3 none
static volatile int add_kind;
static void *add_main(void *arg) {
  pin_to_core(2); // (off the drawing thread's core)
  (void)arg;
  LvHttp *h = lv_http_new();
  static const int order[] = {BK_E621, BK_DANBOORU, BK_MOEBOORU, BK_GELBOORU};
  int found = -1;
  for (int k = 0; k < 4 && found < 0; k++) {
    char url[300];
    lv_booru_probe_url(order[k], add_base, url, sizeof url);
    uint8_t *b = NULL;
    size_t n = 0;
    const char *err;
    if (lv_http_get(h, url, &b, &n, 2u << 20, NULL, NULL, &err) == 200 && lv_booru_probe(order[k], (const char *)b, n)) found = order[k];
    free(b);
  }
  lv_http_free(h);
  add_kind = found;
  add_state = found >= 0 ? 2 : 3;
  return NULL;
}
static void add_site_start(const char *typed) {
  if (add_state == 1) return;
  char b[160];
  snprintf(b, sizeof b, "%s%s", strncmp(typed, "http", 4) ? "https://" : "", typed);
  size_t n = strlen(b);
  while (n && (b[n - 1] == '/' || b[n - 1] == ' ')) b[--n] = 0;
  snprintf(add_base, sizeof add_base, "%s", b);
  add_state = 1;
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 256 * 1024);
  if (pthread_create(&t, &at, add_main, NULL) == 0) pthread_detach(t);
  else add_state = 3;
  say("Looking at the site…");
}
// (once a frame from the Browser: a site found is added and opened)
static void add_site_poll(void) {
  if (add_state < 2) return;
  if (add_state == 2) {
    // its name: the address without https:// and www.
    const char *h = strstr(add_base, "://");
    h = h ? h + 3 : add_base;
    if (!strncmp(h, "www.", 4)) h += 4;
    char name[48];
    snprintf(name, sizeof name, "%.*s", (int)strcspn(h, "/"), h);
    if (lv_booru_add_site(name, add_kind, add_base) == 0) {
      custom_sites_save();
      cfg.site = LV_NSITES - 1;
      sfx(SFX_DONE);
      say("Site added");
      web_search();
    } else { sfx(SFX_ERROR); say("No room for more sites"); }
  } else { sfx(SFX_ERROR); say("That address doesn't answer like a booru (Danbooru, Moebooru, Gelbooru or e621 kinds)"); }
  add_state = 0;
}

// The site switcher: a window with two tabs, Pictures (the sites, and + to add one) and Videos (the
// video sources, opening the video browser); each with its icon (sites/*.png).
static int sites_tab;      // 0 pictures, 1 videos, 2 manga
static float sites_scroll;
static vita2d_texture *site_tex[64];
static int site_tex_tried[64];
static vita2d_texture *site_icon_named(int slot, const char *name) {
  if (slot < 0 || slot >= 64 || !name) return NULL;
  if (!site_tex_tried[slot]) {
    site_tex_tried[slot] = 1;
    char p[96];
    snprintf(p, sizeof p, "app0:sites/%s.png", name);
    site_tex[slot] = vita2d_load_PNG_file(p);
  }
  return site_tex[slot];
}
// a site of its own, with no picture: a rounded square in a colour from its name, its first letter
static void draw_letter_icon(int x, int y, int sz, const char *name) {
  unsigned h = 2166136261u;
  for (const char *p = name; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
  unsigned col = RGBA(90 + h % 120, 70 + (h >> 8) % 110, 110 + (h >> 16) % 120, 255);
  rrect(x, y, sz, sz, sz / 4, col);
  rrect(x, y, sz, sz / 2, sz / 4, with_alpha(TEXT, 30));
  char l[2] = {(char)toupper((unsigned char)name[0]), 0};
  text_center(x + sz / 2, y + sz / 2 + sz / 5, TEXT, sz / 2, l);
}
static void draw_sites_sheet(void) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 170));
  const int cols = 6, tw = 142, th = 136, gap = 10;
  int vis[LV_MAX_SITES], nvis = sites_visible(vis);
  int n = sites_tab == 2 ? nmsources() : sites_tab ? nvsources() : nvis + 1; // (+ the add tile)
  int rows = (n + cols - 1) / cols, vis_rows = rows < 3 ? rows : 3;
  int ww = cols * tw + (cols - 1) * gap + 36, wh = vis_rows * th + (vis_rows - 1) * gap + 100;
  int wx = (W - ww) / 2, wy = 18;
  rrect(wx, wy, ww, wh, 26, SURF);
  // the tabs: a segmented control
  static const char *tabs[] = {"Pictures", "Videos", "Manga"};
  static const char *tab_icons[] = {"image", "film", "book"};
  int segw = 450, tw3 = segw / 3, segx = W / 2 - segw / 2, segy = wy + 14;
  rrect(segx, segy, segw, 40, 20, SURF2);
  rrect(segx + 3 + sites_tab * tw3, segy + 3, tw3 - 6, 34, 17, ACC);
  for (int t = 0; t < 3; t++) {
    unsigned c = t == sites_tab ? RGBA(20, 12, 16, 255) : TEXT;
    int lw = text_w(17, tabs[t]) + 30, lx = segx + t * tw3 + (tw3 - lw) / 2;
    icon(tab_icons[t], lx + 11, segy + 20, c);
    text(lx + 28, segy + 27, c, 17, tabs[t]);
    if (hit(segx + t * tw3, segy, tw3, 40)) { if (sites_tab != t) { sites_tab = t; sheet_sel = 0; sites_scroll = 0; sfx(SFX_TAB); } in.tap = 0; }
  }
  text(wx + 20, segy + 27, FAINT, 13, "L / R");
  if (in.pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
    sites_tab = (sites_tab + ((in.pressed & SCE_CTRL_RTRIGGER) ? 1 : 2)) % 3;
    sheet_sel = 0; sites_scroll = 0; sfx(SFX_TAB);
    n = sites_tab == 2 ? nmsources() : sites_tab ? nvsources() : nvis + 1;
    rows = (n + cols - 1) / cols;
  }
  if (sheet_sel >= n) sheet_sel = n - 1;
  // (the chosen tile's row kept in sight)
  int srow = sheet_sel / cols;
  if (srow - (int)sites_scroll > 2) sites_scroll = srow - 2;
  if (srow < sites_scroll) sites_scroll = srow;
  int top = wy + 68;
  for (int k = 0; k < n; k++) {
    int r = k / cols - (int)sites_scroll;
    if (r < 0 || r >= vis_rows) continue;
    int x = wx + 18 + (k % cols) * (tw + gap), y = top + r * (th + gap);
    int on = k == sheet_sel;
    const char *name, *about, *ic;
    int cur, blocked = 0, add = !sites_tab && k == nvis, slot;
    int site = !sites_tab && k < nvis ? vis[k] : -1;
    if (sites_tab == 2) { name = msource_name(k); about = msource_about(k); ic = msource_icon(k); cur = screen == S_MANGA && k == mg_src_now(); slot = 48 + k; }
    else if (sites_tab) { name = vsource_name(k); about = vsource_about(k); ic = vsource_icon(k); cur = k == vb_cur; blocked = vsource_blocked(k); slot = 32 + k; }
    else if (add) { name = "Add a site"; about = "Any booru, by its address"; ic = "add"; cur = 0; slot = 31; }
    else { name = LV_SITES[site].name; about = LV_SITES[site].about; ic = LV_SITES[site].icon; cur = site == cfg.site; slot = site; }
    if (on) rrect(x - 3, y - 3, tw + 6, th + 6, 20, ACC);
    rrect(x, y, tw, th, 18, on ? SURF2 : with_alpha(SURF2, 140));
    vita2d_texture *t = site_icon_named(slot, ic);
    int isz = 70;
    if (t) vita2d_draw_texture_tint_scale(t, x + tw / 2 - isz / 2, y + 10, isz / 112.0f, isz / 112.0f, blocked ? RGBA(255, 255, 255, 110) : RGBA(255, 255, 255, 255));
    else draw_letter_icon(x + tw / 2 - isz / 2, y + 10, isz, name);
    text_fit(x + 10, y + 100, cur ? ACC : TEXT, 15, name, tw - 20);
    if (about) text_fit(x + 10, y + 122, FAINT, 12, about, tw - 20);
    if (cur) { circle(x + tw - 16, y + 16, 10, ACC); text_center(x + tw - 16, y + 21, RGBA(20, 12, 16, 255), 13, "✓"); }
    if (!sites_tab && !add && LV_SITES[site].needs_key && !wkeys[site][0]) { rrect(x + 8, y + 8, 36, 20, 10, RGBA(0, 0, 0, 160)); text_center(x + 26, y + 23, DIM, 12, "key"); }
    if (blocked) { rrect(x + 8, y + 8, 50, 20, 10, RGBA(0, 0, 0, 170)); text_center(x + 33, y + 23, DIM, 12, "locked"); }
    if (hit(x, y, tw, th)) { sheet_sel = k; in.pressed |= SCE_CTRL_CROSS; }
  }
  if (rows > vis_rows) {
    // (a scroll bar: there's more below)
    float f = sites_scroll / (float)(rows - vis_rows);
    rrect(wx + ww - 10, top + f * (vis_rows * (th + gap) - 60), 4, 60, 2, with_alpha(TEXT, 80));
    if (in.touching && in.down_y > top) { static float acc; acc += in.drag_dy; if (acc < -60 && sites_scroll < rows - vis_rows) { sites_scroll++; acc = 0; } if (acc > 60 && sites_scroll > 0) { sites_scroll--; acc = 0; } }
  }
  int col = sheet_sel % cols, row = sheet_sel / cols;
  if (in.pressed & SCE_CTRL_RIGHT) sheet_sel = sheet_sel + 1 < n ? sheet_sel + 1 : 0;
  if (in.pressed & SCE_CTRL_LEFT) sheet_sel = sheet_sel > 0 ? sheet_sel - 1 : n - 1;
  if (in.pressed & SCE_CTRL_DOWN) { row = (row + 1) % rows; sheet_sel = row * cols + col < n ? row * cols + col : col; }
  if (in.pressed & SCE_CTRL_UP) { row = (row + rows - 1) % rows; while (row * cols + col >= n) row--; sheet_sel = row * cols + col; }
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_CROSS) {
    if (sites_tab == 2) { sfx(SFX_SELECT); sheet = SH_NONE; manga_open(sheet_sel); }
    else if (sites_tab) {
      if (vsource_blocked(sheet_sel)) { sfx(SFX_ERROR); say("This site only answers after a browser check (Cloudflare), so the Vita can't reach it"); }
      else { sfx(SFX_SELECT); sheet = SH_NONE; vb_open(sheet_sel); }
    } else if (sheet_sel == nvis) {
      if (!ime_open) { sheet = SH_NONE; ime_start(14, "The site's address (like booru.example.org)", "", 0); }
    } else { int st = vis[sheet_sel]; sfx(SFX_SELECT); sheet = SH_NONE; if (st != cfg.site) { cfg.site = st; if (!LV_SITES[cfg.site].hot && cfg.wsort == LV_SORT_HOT) cfg.wsort = LV_SORT_LATEST; web_search(); } }
  }
  // □ on a site added by hand: removed
  else if ((in.pressed & SCE_CTRL_SQUARE) && !sites_tab && sheet_sel < nvis && vis[sheet_sel] >= LV_NBUILTIN) {
    for (int k = vis[sheet_sel]; k < LV_NSITES - 1; k++) LV_SITES[k] = LV_SITES[k + 1];
    LV_NSITES--;
    if (cfg.site >= LV_NSITES) cfg.site = 0;
    custom_sites_save();
    sfx(SFX_DELETE);
    say("Site removed");
  }
  else if ((in.pressed & SCE_CTRL_CIRCLE) || (in.tap && !(in.tap_x >= wx && in.tap_x < wx + ww && in.tap_y >= wy && in.tap_y < wy + wh))) { sfx(SFX_CLOSE); sheet = SH_NONE; }
  in.pressed = 0;
  in.tap = 0;
}

static void open_search_sheet(void);
static void draw_filter_sheet(void) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 150));
  int needs_key = LV_SITES[cfg.site].needs_key;
  enum { R_SEARCH, R_G, R_S, R_Q, R_E, R_SPAN, R_CAT, R_FULL, R_HIDDEN, R_KEY, R_N };
  int nrows = needs_key ? R_N : R_N - 1;
  int w = 440, x = W - w - 12, y = 46, rh = 46;
  rrect(x, y, w, nrows * rh + 18, 16, SURF);
  for (int k = 0; k < nrows; k++) {
    int ry = y + 9 + k * rh;
    if (k == sheet_sel) rrect(x + 6, ry, w - 12, rh - 4, 10, SURF2);
    char v[160];
    const char *name = "", *val = "";
    if (k == R_SEARCH) {
      text(x + 20, ry + 28, TEXT, 17, "Search");
      text_fit(x + 120, ry + 28, cfg.wquery[0] ? ACC : FAINT, 16, cfg.wquery[0] ? cfg.wquery : "recent, bookmarks, tags here…", w - 140);
    } else if (k <= R_E) {
      int bit = 1 << (k - R_G);
      text(x + 20, ry + 28, TEXT, 17, RATING_NAMES[k - R_G]);
      int on = (cfg.ratings & bit) != 0;
      rrect(x + w - 66, ry + 10, 44, 24, 12, on ? ACC : SURF2);
      circle(on ? x + w - 34 : x + w - 54, ry + 22, 9, TEXT);
    } else {
      switch (k) {
        case R_SPAN: name = "Top of"; val = lv_booru_has_spans(&LV_SITES[cfg.site]) ? LV_SPAN_NAMES[cfg.wspan] : "All time (this site)"; break;
        case R_CAT: name = "Save into"; snprintf(v, sizeof v, "‹ %s ›", save_category()); val = v; break;
        case R_FULL: name = "Saves"; val = cfg.wfull ? "Full size (the original)" : "Vita size (smaller)"; break;
        case R_HIDDEN: {
          int nh = 0;
          for (const char *p = whidden; *p;) { while (*p == ' ') p++; if (*p) nh++; while (*p && *p != ' ') p++; }
          name = "Hidden tags";
          if (nh) snprintf(v, sizeof v, "%d", nh); else snprintf(v, sizeof v, "none");
          val = v;
          break;
        }
        case R_KEY: name = "API key"; val = wkeys[cfg.site][0] ? "set (✕ to change)" : "needed: ✕"; break;
      }
      text(x + 20, ry + 28, TEXT, 17, name);
      text(x + w - 22 - text_w(15, val), ry + 28, k == R_SPAN && cfg.wsort != LV_SORT_TOP ? FAINT : ACC, 15, val);
    }
    if (hit(x, ry, w, rh)) { sheet_sel = k; in.pressed |= SCE_CTRL_CROSS; }
  }
  if (sheet_sel == R_KEY && needs_key) text(x + 20, y + nrows * rh + 40, FAINT, 13, !strcmp(LV_SITES[cfg.site].name, "Rule34") ? "rule34.xxx → My Account → Options: API Access Credentials" : "gelbooru.com → My Account → Options: API Access Credentials");
  if (sheet_sel == R_HIDDEN) text(x + 20, y + nrows * rh + 40, FAINT, 13, "Posts with any of these tags never show (spaces between)");
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) sheet_sel = (sheet_sel + 1) % nrows;
  if (in.pressed & SCE_CTRL_UP) sheet_sel = (sheet_sel + nrows - 1) % nrows;
  int d = (in.pressed & SCE_CTRL_RIGHT) ? 1 : (in.pressed & SCE_CTRL_LEFT) ? -1 : (in.pressed & SCE_CTRL_CROSS) ? 1 : 0;
  if (d) {
    if (sheet_sel == R_SEARCH && (in.pressed & SCE_CTRL_CROSS)) { in.pressed = 0; open_search_sheet(); return; }
    else if (sheet_sel >= R_G && sheet_sel <= R_E && (in.pressed & SCE_CTRL_CROSS)) {
      int bit = 1 << (sheet_sel - R_G);
      if ((cfg.ratings & ~bit) & LV_R_ALL || !(cfg.ratings & bit)) { sfx(SFX_TOGGLE); cfg.ratings ^= bit; web_search(); }
      else { sfx(SFX_ERROR); say("At least one rating stays on"); }
    } else if (sheet_sel == R_SPAN) {
      if (!lv_booru_has_spans(&LV_SITES[cfg.site])) { sfx(SFX_ERROR); say("This site can't pick a time span"); }
      else {
        sfx(SFX_TOGGLE);
        cfg.wspan = (cfg.wspan + d + LV_NSPANS) % LV_NSPANS;
        if (cfg.wsort != LV_SORT_TOP) cfg.wsort = LV_SORT_TOP;
        web_search();
      }
    } else if (sheet_sel == R_CAT) {
      // the site's name, then the vault's categories
      int cur = -1;
      for (int c = 0; c < vault.ncats; c++) if (!strcmp(vault.cats[c], cfg.save_cat)) cur = c;
      int nx = cfg.save_cat[0] ? cur + d : (d > 0 ? 0 : vault.ncats - 1);
      if (nx < 0 || nx >= vault.ncats) cfg.save_cat[0] = 0;
      else snprintf(cfg.save_cat, sizeof cfg.save_cat, "%s", vault.cats[nx]);
      sfx(SFX_TOGGLE);
      cfg_save();
    } else if (sheet_sel == R_FULL) { sfx(SFX_TOGGLE); cfg.wfull = !cfg.wfull; cfg_save(); }
    else if (sheet_sel == R_HIDDEN && (in.pressed & SCE_CTRL_CROSS) && !ime_open) { sheet = SH_NONE; ime_start(9, "Hidden tags (spaces between)", whidden, 0); }
    else if (sheet_sel == R_KEY && (in.pressed & SCE_CTRL_CROSS) && !ime_open) { sheet = SH_NONE; ime_start(10, "User ID (a number)", "", 0); }
  }
  if ((in.pressed & SCE_CTRL_CIRCLE) || (in.pressed & SCE_CTRL_SQUARE) || (in.tap && !(in.tap_x >= x && in.tap_y < y + nrows * rh + 18))) { sfx(SFX_CLOSE); sheet = SH_NONE; }
  in.pressed = 0;
  in.tap = 0;
}

// ---------- the search sheet: type one, bookmark it, the tags in these results, recent, bookmarks ----------
typedef struct { char t[48]; int n; } WTagCount;
static WTagCount wtags[12];
static int nwtags;
// The tags most found in the loaded posts (not those already searched for)
static void wcount_tags(void) {
  enum { CAP = 4096 };
  WTagCount *tab = calloc(CAP, sizeof *tab);
  nwtags = 0;
  if (!tab) return;
  for (int k = 0; k < nposts; k++) {
    for (const char *t = posts[k].tags; t && *t;) {
      while (*t == ' ') t++;
      const char *e = t;
      while (*e && *e != ' ') e++;
      size_t n = (size_t)(e - t);
      if (n && n < 48) {
        unsigned h = 2166136261u;
        for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)t[i]) * 16777619u;
        for (unsigned i = h & (CAP - 1), probes = 0; probes < 64; i = (i + 1) & (CAP - 1), probes++) {
          if (!tab[i].n) { memcpy(tab[i].t, t, n); tab[i].t[n] = 0; tab[i].n = 1; break; }
          if (strlen(tab[i].t) == n && !memcmp(tab[i].t, t, n)) { tab[i].n++; break; }
        }
      }
      t = e;
    }
  }
  for (int i = 0; i < CAP; i++) {
    if (tab[i].n < 2 || has_hidden(tab[i].t, cfg.wquery)) continue;
    // (kept: the 12 most found, in order)
    int at = nwtags < 12 ? nwtags++ : 11;
    if (at == 11 && nwtags == 12 && tab[i].n <= wtags[11].n) continue;
    wtags[at] = tab[i];
    while (at > 0 && wtags[at].n > wtags[at - 1].n) { WTagCount x = wtags[at]; wtags[at] = wtags[at - 1]; wtags[at - 1] = x; at--; }
  }
  free(tab);
}
enum { SR_TYPE, SR_MARK, SR_CLEAR, SR_HEAD, SR_TAG, SR_RECENT, SR_BMARK };
typedef struct { int kind, i; } SRow;
static float ssheet_scroll;
static void web_add_tag(const char *t, int minus) {
  char q[160];
  snprintf(q, sizeof q, "%s%s%s%s", cfg.wquery, cfg.wquery[0] ? " " : "", minus ? "-" : "", t);
  char clean[128];
  snprintf(cfg.wquery, sizeof cfg.wquery, "%s", lv_booru_clean(q, clean, sizeof clean));
}
static void draw_search_sheet(void) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 150));
  SRow rows[64];
  int n = 0;
  rows[n++] = (SRow){SR_TYPE, 0};
  if (cfg.wquery[0]) { rows[n++] = (SRow){SR_MARK, 0}; rows[n++] = (SRow){SR_CLEAR, 0}; }
  if (nwtags) { rows[n++] = (SRow){SR_HEAD, 0}; for (int k = 0; k < nwtags; k++) rows[n++] = (SRow){SR_TAG, k}; }
  if (nwrecent) { rows[n++] = (SRow){SR_HEAD, 1}; for (int k = 0; k < nwrecent && n < 60; k++) rows[n++] = (SRow){SR_RECENT, k}; }
  if (nwmarks) { rows[n++] = (SRow){SR_HEAD, 2}; for (int k = 0; k < nwmarks && n < 64; k++) rows[n++] = (SRow){SR_BMARK, k}; }
  if (sheet_sel >= n) sheet_sel = n - 1;
  while (sheet_sel > 0 && rows[sheet_sel].kind == SR_HEAD) sheet_sel--;
  int w = 520, x = W - w - 12, y = 46, rh = 44, view = H - BAR - y - 10;
  rrect(x, y, w, view, 16, SURF);
  // (the chosen row kept in sight)
  float want = sheet_sel * rh;
  if (want - ssheet_scroll > view - rh * 2) ssheet_scroll = want - (view - rh * 2);
  if (want - ssheet_scroll < rh) ssheet_scroll = want - rh;
  if (ssheet_scroll < 0) ssheet_scroll = 0;
  int marked = cfg.wquery[0] && wsearch_find(wmarks, nwmarks, cfg.site, cfg.wquery) >= 0;
  for (int k = 0; k < n; k++) {
    int ry = y + 8 + k * rh - (int)ssheet_scroll;
    if (ry < y || ry + rh > y + view) continue;
    SRow r = rows[k];
    if (r.kind == SR_HEAD) {
      static const char *heads[] = {"Tags in these results  (✕ add, □ leave out)", "Recent  (□ forgets one)", "Bookmarks  (□ removes one)"};
      text(x + 20, ry + 28, FAINT, 14, heads[r.i]);
      continue;
    }
    if (k == sheet_sel) rrect(x + 6, ry, w - 12, rh - 4, 10, SURF2);
    char b[200];
    switch (r.kind) {
      case SR_TYPE: text(x + 20, ry + 27, TEXT, 17, "Search"); text_fit(x + 110, ry + 27, cfg.wquery[0] ? ACC : FAINT, 15, cfg.wquery[0] ? cfg.wquery : "type tags, like blue_hair smile", w - 130); break;
      case SR_MARK: text(x + 20, ry + 27, TEXT, 17, marked ? "★ Bookmarked (✕ to remove)" : "☆ Bookmark this search"); break;
      case SR_CLEAR: text(x + 20, ry + 27, TEXT, 17, "Clear the search"); break;
      case SR_TAG: text(x + 30, ry + 27, TEXT, 17, wtags[r.i].t); snprintf(b, sizeof b, "%d", wtags[r.i].n); text(x + w - 26 - text_w(14, b), ry + 27, FAINT, 14, b); break;
      case SR_RECENT: case SR_BMARK: {
        const WSearch *s = r.kind == SR_RECENT ? &wrecent[r.i] : &wmarks[r.i];
        text_fit(x + 30, ry + 27, TEXT, 17, s->q, w - 170);
        text(x + w - 26 - text_w(13, LV_SITES[s->site].name), ry + 27, FAINT, 13, LV_SITES[s->site].name);
        break;
      }
    }
    if (hit(x, ry, w, rh)) { sheet_sel = k; in.pressed |= SCE_CTRL_CROSS; }
  }
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) do sheet_sel = (sheet_sel + 1) % n; while (rows[sheet_sel].kind == SR_HEAD);
  if (in.pressed & SCE_CTRL_UP) do sheet_sel = (sheet_sel + n - 1) % n; while (rows[sheet_sel].kind == SR_HEAD);
  SRow r = rows[sheet_sel];
  int go = (in.pressed & SCE_CTRL_CROSS) != 0, sq = (in.pressed & SCE_CTRL_SQUARE) != 0;
  if (go || sq) {
    switch (r.kind) {
      case SR_TYPE: if (go && !ime_open) { sheet = SH_NONE; ime_start(3, "Search tags", cfg.wquery, 0); } break;
      case SR_MARK:
        if (!go) break;
        if (marked) wsearch_drop(wmarks, &nwmarks, wsearch_find(wmarks, nwmarks, cfg.site, cfg.wquery), DATA "/web_marks.txt");
        else if (nwmarks < 30) { wmarks[nwmarks].site = cfg.site; snprintf(wmarks[nwmarks].q, sizeof wmarks[nwmarks].q, "%s", cfg.wquery); nwmarks++; wlist_save(DATA "/web_marks.txt", wmarks, nwmarks); }
        sfx(marked ? SFX_CLOSE : SFX_DONE);
        break;
      case SR_CLEAR: if (go) { sfx(SFX_CLOSE); cfg.wquery[0] = 0; sheet = SH_NONE; web_search(); } break;
      case SR_TAG: sfx(SFX_SELECT); web_add_tag(wtags[r.i].t, sq); sheet = SH_NONE; web_search(); break;
      case SR_RECENT: case SR_BMARK: {
        WSearch *a = r.kind == SR_RECENT ? wrecent : wmarks;
        int *na = r.kind == SR_RECENT ? &nwrecent : &nwmarks;
        if (sq) { sfx(SFX_DELETE); wsearch_drop(a, na, r.i, r.kind == SR_RECENT ? DATA "/web_recent.txt" : DATA "/web_marks.txt"); break; }
        sfx(SFX_SELECT);
        WSearch s = a[r.i];
        cfg.site = s.site;
        snprintf(cfg.wquery, sizeof cfg.wquery, "%s", s.q);
        if (!LV_SITES[cfg.site].hot && cfg.wsort == LV_SORT_HOT) cfg.wsort = LV_SORT_LATEST;
        sheet = SH_NONE;
        web_search();
        break;
      }
    }
  }
  if ((in.pressed & SCE_CTRL_CIRCLE) || (in.tap && in.tap_x < x)) { sfx(SFX_CLOSE); sheet = SH_NONE; }
  in.pressed = 0;
  in.tap = 0;
}
static void open_search_sheet(void) {
  wlists_load();
  wcount_tags();
  sheet = SH_SEARCH;
  sheet_sel = 0;
  ssheet_scroll = 0;
  sfx(SFX_OPEN);
}

// ---------- the grid ----------

static void draw_web(void) {
  PT("web in");
  web_start();
  PT("web_start");
  add_site_poll();
  if (add_state == 1) { rrect(W - 210, H - BAR - 52, 194, 40, 20, with_alpha(SURF2, 240)); spinner(W - 186, H - BAR - 32, 10); text(W - 166, H - BAR - 26, DIM, 14, "Looking at the site\u2026"); }
  if (!saved_src && loaded_vault) saved_build();
  PT("saved_build");
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
    wscroll += wscroll_v + in.ly * 14 - in.bdy * 1.5f;
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
      if (wstate[k] == 1) spinner(x + cw / 2, y + ch / 2, 10);
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
    if (!sheet_open && hit((int)x, (int)y, (int)cw, (int)ch) && in.tap_y > TOP && in.tap_y < H - BAR) { wsel = k; sfx(SFX_SELECT); wdetails = 0; wdetails_y = 0; wchrome_until = frame_no + 180; wv_back = S_WEB; screen = S_WEBVIEW; wv_show(k); return; }
  }
  for (int k = last; k < last + 18 && k < nposts && nw < 48; k++) if (!preview_of(k) && wstate[k] != 3) wanted[nw++] = k;
  ask_previews(wanted, nw);
  // the next page, before the end is reached
  if (last + 12 >= nposts) web_more();
  if (!nposts) {
    if (!lv_net_online()) text_center(W / 2, TOP + 150, DIM, 20, "Not connected: turn on Wi-Fi in the Vita's settings");
    else if (wloading) {
      spinner(W / 2, TOP + 150, 22);
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
  text(18, 36, TEXT, 26, "Browser");
  char site[80];
  snprintf(site, sizeof site, "%s  ▾", LV_SITES[cfg.site].name);
  int sx = 18 + text_w(26, "Browser") + 14;
  int ws = text_w(17, site) + 36;
  rrect(sx, 8, ws, 36, 18, ACC);
  text(sx + 18, 32, RGBA(20, 12, 16, 255), 17, site);
  if (!sheet_open && hit(sx, 4, ws, 44)) { sfx(SFX_OPEN); sheet = SH_SITES; sheet_sel = site_tile_of(cfg.site); sites_tab = 0; sites_scroll = 0; }
  // filter and sort on the right
  char filt[40];
  int nr = 0;
  for (int k = 0; k < 4; k++) nr += (cfg.ratings >> k) & 1;
  if (nr == 4) snprintf(filt, sizeof filt, "Filter");
  else { snprintf(filt, sizeof filt, "Filter:"); for (int k = 0; k < 4; k++) if (cfg.ratings & (1 << k)) { char c[3] = {' ', RATING_NAMES[k][0], 0}; strcat(filt, c); } }
  int rx = W - 16, hb;
  rx -= head_button(rx, "sort", LV_SORT_NAMES[cfg.wsort], SURF2, &hb) + 10;
  if (!sheet_open && (hb || (in.pressed & SCE_CTRL_TRIANGLE))) {
    sfx(SFX_TOGGLE);
    do cfg.wsort = (cfg.wsort + 1) % LV_NSORTS; while (cfg.wsort == LV_SORT_HOT && !LV_SITES[cfg.site].hot);
    web_search();
  }
  rx -= head_button(rx, "sliders", filt, nr == 4 ? SURF2 : with_alpha(ACC, 110), &hb) + 10;
  if (!sheet_open && (hb || (in.pressed & SCE_CTRL_SQUARE))) { sfx(SFX_OPEN); sheet = SH_FILTER; sheet_sel = 0; in.pressed &= ~SCE_CTRL_SQUARE; }
  // the search field, the width of the screen, under the title
  int wq = W - 32 - (cfg.wquery[0] ? 48 : 0);
  rrect(16, 50, wq, 36, 18, SURF2);
  icon("search", 38, 68, cfg.wquery[0] ? TEXT : FAINT);
  text_fit(56, 74, cfg.wquery[0] ? TEXT : FAINT, 16, cfg.wquery[0] ? cfg.wquery : "Search tags, or pick from recent and bookmarks", wq - 60);
  if (!sheet_open && hit(16, 50, wq, 36) && !ime_open) open_search_sheet();
  if (cfg.wquery[0]) {
    int cx = 16 + wq + 8;
    rrect(cx, 50, 40, 36, 18, SURF2);
    icon("xmark", cx + 20, 68, DIM);
    if (!sheet_open && hit(cx, 50, 40, 36)) { cfg.wquery[0] = 0; web_search(); }
  }

  if (!sheet_open) {
    // the sites are tabs: L / R go through them
    if (in.pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
      sfx(SFX_TAB);
      do cfg.site = (cfg.site + ((in.pressed & SCE_CTRL_RTRIGGER) ? 1 : LV_NSITES - 1)) % LV_NSITES; while (LV_SITES[cfg.site].hidden);
      if (!LV_SITES[cfg.site].hot && cfg.wsort == LV_SORT_HOT) cfg.wsort = LV_SORT_LATEST;
      web_search();
      char m[80];
      snprintf(m, sizeof m, "%s", LV_SITES[cfg.site].name);
      say(m);
    }
    if ((in.pressed & SCE_CTRL_CROSS) && nposts) { sfx(SFX_SELECT); wdetails = 0; wdetails_y = 0; wchrome_until = frame_no + 180; wv_back = S_WEB; screen = S_WEBVIEW; wv_show(wsel); return; }
    if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_TAB); screen = S_SHOW; plan_sel = 0; }
    if (in.pressed & SCE_CTRL_START) { sfx(SFX_OPEN); sheet = SH_SITES; sheet_sel = site_tile_of(cfg.site); sites_tab = 0; sites_scroll = 0; }
  }
  draw_bar();
  if (sheet == SH_SITES) draw_sites_sheet();
  else if (sheet == SH_FILTER) draw_filter_sheet();
  else if (sheet == SH_SEARCH) draw_search_sheet();
  if (wkey_ask && !ime_open) { wkey_ask = 0; ime_start(11, "API key", "", 0); }
}

// ---------- the full view's drawing ----------

static void draw_webview(void) {
  if (!nposts) { screen = wv_back; return; }
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
    spinner(W / 2, H / 2 - 30, 22);
    float f = big_progress;
    rrect(W / 2 - 120, H - 30, 240, 6, 3, with_alpha(TEXT, 50));
    if (f > 0) rrect(W / 2 - 120, H - 30, 240 * fminf(1, f), 6, 3, ACC);
    if (lv_post_is_video(p)) text_center(W / 2, H - 40, DIM, 14, "Downloading the video…");
  }
  if (wv_err[0]) text_center(W / 2, (int)avail_h - 50, ACC2, 15, wv_err);

  // (zoomed in past 1.25x: the sample decoded again at up to 1920 px, from the bytes already here)
  static int wv_hi_post = -1;
  if (wv_hi_post >= 0 && wv_hi_post != wv) wv_hi_post = -1;
  if (wzoom > 1.25f && wv_hi_post != wv && wv_tex_post == wv && wv_raw_post == wv && wv_raw && !saving && !lv_post_is_video(p)) {
    uint8_t *copy = malloc(wv_rawn);
    if (copy) {
      memcpy(copy, wv_raw, wv_rawn);
      pthread_mutex_lock(&wmx);
      free(big_job.raw);
      big_job.post = wv; big_job.gen = wgen; big_job.save = 0; big_job.hires = 1; big_job.raw = copy; big_job.rawn = wv_rawn;
      pthread_cond_broadcast(&wcv);
      pthread_mutex_unlock(&wmx);
      wv_hi_post = wv;
    }
  }
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
    if (frame_no < wchrome_until && in.tap_x < 70 && in.tap_y < 60) { sfx(SFX_BACK); wv_drop(); screen = wv_back; return; }
    else if (frame_no < wchrome_until && in.tap_x > W - 70 && in.tap_y < 60) wdetails = 1;
    else if (frame_no < wchrome_until && in.tap_x > W - 170 && in.tap_x < W - 70 && in.tap_y < 60) wv_save();
    else wchrome_until = frame_no < wchrome_until ? frame_no : frame_no + 240;
  }

  // --- buttons ---
  if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER)) { sfx(SFX_TICK); wv_show(wv + 1); return; }
  if (in.pressed & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER)) { sfx(SFX_TICK); wv_show(wv - 1); return; }
  if (in.pressed & SCE_CTRL_CIRCLE) { if (wdetails) { sfx(SFX_CLOSE); wdetails = 0; } else { sfx(SFX_BACK); wv_drop(); wsel = wv; screen = wv_back; return; } }
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
    grad(0, 0, W, 64, RGBA(0, 0, 0, 255), 64 * a / 400, 0, 0);
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
