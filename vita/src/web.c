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

#define WSLOTS 72 // (atlas cells 72-143, the Browser's four atlases: main.c. It was 120, past the end of them)
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
  int nsfw, stars, notags; // (a save: the level, the stars, and 1 to leave the site's tags off; -1 level: from the rating)
} big_job = {-1, 0, 0, 0, 0, 0, NULL, 0, "", -1, 5, 0};
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
static unsigned saved_ver; // (one more with each save: is_saved's answers kept until then)
static void saved_build(void) {
  free(saved_src);
  capsaved = vault.n + 64;
  saved_src = malloc(capsaved * sizeof *saved_src);
  nsaved = 0;
  if (!saved_src) { capsaved = 0; return; } // (no ✓ marks then, rather than a crash)
  saved_ver++;
  for (int i = 0; i < vault.n; i++) if (vault.items[i].source[0]) saved_src[nsaved++] = fnv64(vault.items[i].source);
  qsort(saved_src, nsaved, sizeof *saved_src, cmp_u64);
}
static int is_saved(const LvPost *p) {
  if (!saved_src) return 0;
  // (asked for every card on screen every frame: the answer kept per post, keyed by its page string
  //  and the saves' count; a hash and a search only when either changed)
  static struct { const char *page; unsigned ver; signed char ans; } memo[256];
  unsigned slot = ((uintptr_t)p->page >> 3) & 255;
  if (memo[slot].page == p->page && memo[slot].ver == saved_ver) return memo[slot].ans;
  unsigned long long h = fnv64(p->page);
  int ans = bsearch(&h, saved_src, nsaved, sizeof h, cmp_u64) != NULL;
  memo[slot].page = p->page; memo[slot].ver = saved_ver; memo[slot].ans = (signed char)ans;
  return ans;
}
static void saved_add(const char *src) {
  if (nsaved == capsaved) {
    unsigned long long *ns = realloc(saved_src, (capsaved * 2 + 16) * sizeof *ns);
    if (!ns) return;
    saved_src = ns; capsaved = capsaved * 2 + 16;
  }
  // (put in its place: the list stays in order with one move, not a sort of all of it per save)
  unsigned long long h = fnv64(src);
  int lo = 0, hi = nsaved;
  while (lo < hi) { int m = (lo + hi) / 2; if (saved_src[m] < h) lo = m + 1; else hi = m; }
  memmove(saved_src + lo + 1, saved_src + lo, (nsaved - lo) * sizeof *saved_src);
  saved_src[lo] = h;
  nsaved++;
  saved_ver++;
}

// ---------- the threads ----------

// The episode sites: each one's own page read before it's shown (lv_item_check: never-shown words,
// its MP4); the ones that fail, or don't answer, are left out. Stops when *gen moves on from gen.
// (each episode's answer kept for the session: the rows of the video browser overlap, and a page is
//  ~55 KB, slow to fetch again and again on the Vita's Wi-Fi)
#define ICHECK_N 512
static struct { unsigned long long h; signed char ok; char *file; } icheck[ICHECK_N];
static pthread_mutex_t icmx = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long page_hash(const char *s) { unsigned long long h = 1469598103934665603ull; while (*s) h = (h ^ (unsigned char)*s++) * 1099511628211ull; return h | 1; }
static int check_items(LvHttp *h, const LvSite *s, LvPost *p, int n, volatile int *gen, int g) {
  int kept = 0, k = 0;
  for (; k < n && *gen == g; k++) {
    unsigned long long ph = page_hash(p[k].page);
    int slot = (int)(ph % ICHECK_N), ok = -1;
    pthread_mutex_lock(&icmx);
    if (icheck[slot].h == ph) {
      ok = icheck[slot].ok;
      if (ok && icheck[slot].file) { char *f = strdup(icheck[slot].file); if (f) { free(p[k].file); p[k].file = f; } else ok = -1; }
    }
    pthread_mutex_unlock(&icmx);
    if (ok < 0) {
      uint8_t *b = NULL;
      size_t bn = 0;
      int st = lv_http_get(h, p[k].page, &b, &bn, 2u << 20, NULL, NULL, NULL);
      ok = st == 200 && b && lv_item_check(s, &p[k], (char *)b);
      free(b);
      // (kept only when the page answered: one that didn't may answer next time)
      if (st == 200) {
        pthread_mutex_lock(&icmx);
        free(icheck[slot].file);
        icheck[slot].h = ph; icheck[slot].ok = (signed char)ok; icheck[slot].file = ok ? strdup(p[k].file) : NULL;
        pthread_mutex_unlock(&icmx);
      }
    }
    if (ok) p[kept++] = p[k];
    else { LvPost *q = &p[k]; free(q->preview); free(q->sample); free(q->file); free(q->page); free(q->source); free(q->tags); }
  }
  for (; k < n; k++) { LvPost *q = &p[k]; free(q->preview); free(q->sample); free(q->file); free(q->page); free(q->source); free(q->tags); }
  return kept;
}

static void *list_main(void *arg) {
  pin_to_core(2); // (off the drawing thread's core)
  LvHttp *h = lv_http_new();
  while (worker_run) {
    pthread_mutex_lock(&wmx);
    while (worker_run && !list_want) pthread_cond_wait(&wcv, &wmx);
    int page = list_want, gen = wgen, ratings = job_q.ratings;
    list_want = 0;
    // (the site copied while held: the list of sites can change on the drawing thread)
    LvSite site_copy = LV_SITES[job_q.site < LV_NSITES ? job_q.site : 0];
    const LvSite *s = &site_copy;
    char q[128], key[200];
    snprintf(q, sizeof q, "%s", job_q.q);
    snprintf(key, sizeof key, "%s", job_q.key);
    int sort = job_q.sort, span = job_q.span;
    char hidden[512];
    snprintf(hidden, sizeof hidden, "%s", job_q.hidden);
    pthread_mutex_unlock(&wmx);
    if (!worker_run) break;
    uint8_t *b = NULL;
    size_t n = 0;
    const char *err = NULL;
    // e-shuushuu searches by a tag's number: the word looked up first
    char w[128] = "";
    if (s->kind == BK_ESHUU && sscanf(q, "%127s", w) == 1 && w[0] != '#' && w[0] != '-') {
      char tu[600], idq[24] = "";
      lv_esh_tag_url(s, w, tu, sizeof tu);
      if (lv_http_get(h, tu, &b, &n, 1u << 20, NULL, NULL, &err) == 200 && b) lv_esh_tag_id((char *)b, n, w, idq, sizeof idq);
      free(b); b = NULL; n = 0;
      snprintf(q, sizeof q, "%s", idq[0] ? idq : "#0"); // (no such tag: #0, which finds nothing)
    }
    char url[2048];
    lv_booru_url_ex(s, q, ratings, sort, span, key[0] ? key : NULL, page, url, sizeof url);
    // (no address: the site has no more pages)
    int st = url[0] ? lv_http_get(h, url, &b, &n, 8 * 1024 * 1024, NULL, NULL, &err) : 200;
    if (!url[0]) { b = (uint8_t *)strdup(s->kind == BK_BOORUIO ? "{\"data\":[]}" : "[]"); n = b ? strlen((char *)b) : 0; }
    int count = 0;
    LvPost *got = st == 200 ? lv_booru_parse(s, (const char *)b, n, ratings, &count) : NULL;
    if (got && lv_site_checks_items(s)) count = check_items(h, s, got, count, &wgen, gen);
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
    // (asleep with nothing wanted, or while 20 finished ones wait to be taken: woken when they are)
    while (worker_run && (nwwant == 0 || nwdone >= 20)) pthread_cond_wait(&wcv, &wmx);
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
// (the video downloading now, played as it comes: its post, its search, its slot's file and progress)
static LvGrow dl_grow[2];
static char dl_path[2][256];
static volatile int dl_slot, dl_post = -1, dl_gen;

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
static int save_nsfw = -1, save_stars = 5, save_notags; // (the save being written: its choices)
static void write_save(const LvPost *post, WDone *d, int vgen_asked, const char *cat) {
  int video = lv_post_is_video(post), gif = !strcmp(post->ext, "gif");
  snprintf(d->name, sizeof d->name, "%s %lld.%s", LV_SITES[job_q.site].name, post->id, d->ext);
  snprintf(d->cat, sizeof d->cat, "%s", cat);
  now_iso(d->when, sizeof d->when);
  int ri = post->rating == LV_R_EXPLICIT ? 3 : post->rating == LV_R_QUESTIONABLE ? 2 : post->rating == LV_R_SENSITIVE ? 1 : 0;
  LvNew nw = {.name = d->name, .category = d->cat, .kind = video ? "video" : gif ? "gif" : "image", .tags = save_notags ? "" : post->tags,
              .source = post->page, .added = d->when, .nsfw = save_nsfw >= 0 ? save_nsfw : RATING_NSFW[ri], .quality = save_stars, .width = d->w, .height = d->h,
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
  d->nw.tags = strdup(save_notags ? "" : post->tags); d->nw.source = strdup(post->page);
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
    save_nsfw = big_job.nsfw; save_stars = big_job.stars; save_notags = big_job.notags;
    uint8_t *raw = big_job.raw;
    size_t rawn = big_job.rawn;
    big_job.post = -1;
    big_job.raw = NULL;
    LvPost post;
    int have = p < nposts;
    if (have) post_copy(&posts[p], &post);
    int site_of_post = have && post.site1 > 0 && post.site1 <= LV_NSITES ? post.site1 - 1 : job_q.site;
    int site_kind = site_of_post >= 0 && site_of_post < LV_NSITES ? LV_SITES[site_of_post].kind : -1;
    int kvs = site_kind == BK_KVS || site_kind == BK_XV; // (a video whose MP4 is named on its page)
    big_cancel = 0;
    big_progress = 0;
    pthread_mutex_unlock(&wmx);
    if (!have || (save && gen != wgen)) { free(raw); if (save) { pthread_mutex_lock(&wmx); saving = 0; pthread_mutex_unlock(&wmx); } if (have) { free(post.preview); free(post.sample); free(post.file); free(post.page); free(post.source); free(post.tags); } continue; }
    WDone d;
    memset(&d, 0, sizeof d);
    d.post = p;
    d.gen = gen;
    // a KVS site's video: its page first, for the MP4's address (the page's cookie goes with it, on
    // this same connection), and its full tags, checked
    if (kvs && lv_post_is_video(&post)) {
      uint8_t *b = NULL;
      size_t n = 0;
      const char *err = NULL;
      char mp4[1024];
      int nev = 0, ok = lv_http_get(h, post.page, &b, &n, 3u << 20, &big_cancel, NULL, &err) == 200 && b && (site_kind == BK_XV ? lv_xv_video((char *)b, mp4, sizeof mp4, &nev) : lv_kvs_video((char *)b, mp4, sizeof mp4, &nev)) == 0;
      free(b);
      if (ok) { free(post.file); post.file = strdup(mp4); ok = post.file != NULL; }
      if (!ok) {
        d.kind = save ? WD_SAVE : WD_SAMPLE;
        snprintf(d.err, sizeof d.err, "%s", nev ? "Not shown (its tags)" : err && err[0] ? err : "The video's page couldn't be read");
        if (save) { pthread_mutex_lock(&wmx); saving = 0; pthread_mutex_unlock(&wmx); }
        free(post.preview); free(post.sample); free(post.file); free(post.page); free(post.source); free(post.tags);
        free(raw);
        pthread_mutex_lock(&wmx);
        if (nwdone < 24) wdone[nwdone++] = d;
        pthread_mutex_unlock(&wmx);
        continue;
      }
    }
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
        // (played as it comes: the drawing thread opens the player once the first part is here,
        //  stream_ready; two files in turn, so the one before can still be closing)
        int slot = dl_slot ^ 1;
        LvGrow *g = &dl_grow[slot];
        g->have = 0; g->total = 0; g->done = 0; g->failed = 0;
        snprintf(dl_path[slot], sizeof dl_path[slot], DATA "/play%d.%s", slot, post.ext);
        snprintf(video_path, sizeof video_path, "%s", dl_path[slot]);
        // (as long as the card has room for, less a margin; 1.5 GB at most)
        long long room = free_room("ux0:");
        size_t cap = room > 0 ? (size_t)(room - 96ll * 1024 * 1024 > 16ll * 1024 * 1024 ? (room - 96ll * 1024 * 1024 < 1536ll * 1024 * 1024 ? room - 96ll * 1024 * 1024 : 1536ll * 1024 * 1024) : 16ll * 1024 * 1024) : 400u * 1024 * 1024;
        dl_slot = slot;
        __sync_synchronize();
        dl_post = p; dl_gen = gen;
        st = lv_http_stream_file(h, post.file, video_path, cap, &big_cancel, &big_progress, &g->have, &g->total, &err);
        // (its 480p copy not there: the site's smaller one)
        char alt[1024];
        if ((st == 403 || st == 404 || st == 410) && !big_cancel && lv_video_fallback(post.file, alt, sizeof alt) == 0) {
          g->have = 0; g->total = 0;
          st = lv_http_stream_file(h, alt, video_path, cap, &big_cancel, &big_progress, &g->have, &g->total, &err);
        }
        if (st == 200) g->done = 1; else g->failed = 1;
        n = (size_t)g->have;
        dl_post = -1;
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
  lv_net_base(); // (the rest on the worker that fetches)
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
static int fav_offline_next; // (post + 1 to make available offline once it's all here: the save page then)
static int fav_toggle_video(const LvPost *p, int site); // (favs.c)
static void sw_open(void);
static void wv_offline(void);
static int fav_find(int kind, const char *page);
static LvVideo *wv_vid;
static const LvGrow *wv_stream; // (the player's download, while it's still coming)
static float wzoom = 1, wpan_x, wpan_y, wswipe;
static int wdetails;
static float wdetails_y;
static unsigned wchrome_until;

static void wv_drop(void) {
  // (a video still downloading for this view: stopped, it isn't wanted any more)
  if (dl_post >= 0) { pthread_mutex_lock(&wmx); if (!saving) big_cancel = 1; pthread_mutex_unlock(&wmx); }
  if (wv_vid) { video_close_later(wv_vid); wv_vid = NULL; }
  wv_stream = NULL;
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
// (the save page's choices; -1 level: from the post's rating; "" category: the usual one)
static int sw_nsfw = -1, sw_stars = 5, sw_notags;
static char sw_cat[64];
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
  snprintf(big_job.cat, sizeof big_job.cat, "%s", sw_cat[0] ? sw_cat : save_category());
  big_job.nsfw = sw_nsfw; big_job.stars = sw_stars; big_job.notags = sw_notags;
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
  if (!page_ready.ready && !nwdone) return; // (nothing come: no lock taken, a look at two numbers)
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
  // (at most six a frame, two while the grid is flung fast: the rest the next)
  int wcap = fabsf(wscroll_v) > 30 ? 2 : 6;
  int n = nwdone < wcap ? nwdone : wcap;
  WDone got[24];
  memcpy(got, wdone, n * sizeof *got);
  memmove(wdone, wdone + n, (nwdone - n) * sizeof *wdone);
  nwdone -= n;
  if (n && nwwant) pthread_cond_broadcast(&wcv); // (room again for the preview threads)
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
        if (wv < nposts && lv_post_is_video(&posts[wv]) && !d->err[0] && !wv_vid) { // (not if it's already playing as it came)
          wv_vid = lv_video_open(video_path, NULL, 1);
          LOG("video open %s -> %p\n", video_path, (void *)wv_vid);
          if (wv_vid) lv_video_set_volume(wv_vid, cfg.volume * 32768 / 100);
          else snprintf(wv_err, sizeof wv_err, "This video can't be played on the Vita (it can still be saved)");
        }
        // (asked to be made available offline while it came: the save page, now it's all here)
        if (fav_offline_next == wv + 1 && !d->err[0]) { fav_offline_next = 0; wv_offline(); } // (480p, or refused)
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
  static int last[48], nlast = -1, last_gen = -1;
  if (n == nlast && last_gen == wgen && !memcmp(list, last, n * sizeof *list)) { // (as last frame)
    int unasked = 0;
    for (int k = 0; k < n && !unasked; k++) unasked = list[k] < nposts && wstate[list[k]] == 0;
    if (!unasked) return;
  }
  nlast = n < 48 ? n : 48; last_gen = wgen;
  memcpy(last, list, nlast * sizeof *list);
  pthread_mutex_lock(&wmx);
  for (int k = 0; k < nwwant; k++) if (wwant[k] < nposts && wstate[wwant[k]] == 1) wstate[wwant[k]] = 0;
  nwwant = 0;
  for (int k = 0; k < n && nwwant < 48; k++)
    if (list[k] < nposts && wstate[list[k]] == 0) { wstate[list[k]] = 1; wwant[nwwant++] = list[k]; }
  if (nwwant) pthread_cond_broadcast(&wcv);
  pthread_mutex_unlock(&wmx);
}

// ---------- sheets: the sites, and the filter ----------

enum { SH_NONE, SH_SITES, SH_FILTER, SH_SEARCH, SH_FIND };
static int sheet, sheet_sel;

// (the video sources: vbrowse.c)
static int nvsources(void);
static const char *vsource_name(int k);
static const char *vsource_icon(int k);
static const char *vsource_about(int k);
static int vsource_blocked(int k);
static void vb_open(int k);
static int vsrc_has_site(const char *site);
static int vsrc_add(const char *site, const char *tags);
static int vsrc_is_added(int k);
static const char *vsource_tags(int k);
static int vsource_site(int k);
static void vsrc_remove(int k);
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
  // (the boorus first, then the image sites)
  for (int pass = 0; pass < 2; pass++)
    for (int k = 0; k < LV_NSITES; k++) if (!LV_SITES[k].hidden && lv_site_is_booru(&LV_SITES[k]) == !pass) vis[n++] = k;
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

// ---------- finding sources: the lists other booru tools keep, read on the Vita ----------
// gallery-dl's supported sites, the "booru" library's sites.json and Grabber's site lists
// (discover.c), merged; each site found then asked once, as the Browser would (does it answer, and
// with videos?). Pictures: the sites not here yet. Videos: those, and the sites here with videos that
// the video browser hasn't got yet. One press adds one.
typedef struct {
  LvCand c;
  int site;   // (one of LV_SITES already: its place; -1 a new one)
  int state;  // 0 to ask, 1 asking, 2 answers, 3 doesn't answer (why), 4 added (pictures), 5 added (videos)
  int videos; // (of the latest 40)
  char why[40];
} FRow;
#define FROWS 48
static FRow frows[FROWS];
static int nfrows;
static volatile int find_state; // 0 not yet, 1 reading the lists, 2 asking the sites, 3 done
static volatile int find_lists;  // (the lists read)
static volatile int find_cached; // (none read: the last look's, from the card)
#define FIND_LISTS 6
static int find_tab, find_sel;
static float find_scroll;
static pthread_mutex_t fdmx = PTHREAD_MUTEX_INITIALIZER;
static const char *video_tags_for(int kind) { return kind == BK_DANBOORU ? "filetype:mp4" : kind == BK_GELBOORU ? "video" : ""; }
static void draw_letter_icon(int x, int y, int sz, const char *name);
static const char *KIND_NAMES[] = {"Danbooru", "Gelbooru", "Moebooru", "e621"};
static LvSite find_sites[LV_MAX_SITES];
static int find_nsites;
static void *find_main(void *arg) {
  (void)arg;
  pin_to_core(2); // (off the drawing thread's core)
  LvHttp *h = lv_http_new();
  static LvCand c[160];
  int n = 0;
  const char *err;
  // the lists (each from GitHub, else jsDelivr's copy of it)
  for (int k = 0; k < FIND_LISTS && h; k++) {
    char url[300], mirror[300];
    if (k == 0) snprintf(url, sizeof url, "%s", LV_INDEX_GALLERYDL);
    else if (k == 1) snprintf(url, sizeof url, "%s", LV_INDEX_BOORUJS);
    else if (k == 2) snprintf(url, sizeof url, "%s", LV_INDEX_PYBOORU);
    else snprintf(url, sizeof url, "%s%s/sites.txt", LV_INDEX_GRABBER, LV_GRABBER_DIRS[k - 3]);
    lv_disc_mirror(url, mirror, sizeof mirror);
    for (int attempt = 0; attempt < 2; attempt++) {
      uint8_t *b = NULL;
      size_t bn = 0;
      int got = lv_http_get(h, attempt ? mirror : url, &b, &bn, 4u << 20, NULL, NULL, &err) == 200 && b && bn > 10;
      if (got) {
        int before = n;
        if (k == 0) lv_disc_gallerydl((char *)b, bn, c, &n, 160);
        else if (k == 1) lv_disc_boorujs((char *)b, bn, c, &n, 160);
        else if (k == 2) lv_disc_pybooru((char *)b, bn, c, &n, 160);
        else lv_disc_lines((char *)b, bn, LV_GRABBER_KINDS[k - 3], c, &n, 160);
        // (the big lists answering with nothing readable count as not read: the mirror is tried)
        if (n > before || k >= 2) find_lists++;
        else got = 0;
      }
      free(b);
      if (got || !mirror[0]) break;
    }
  }
  // what was found, kept for next time; nothing read: what was found last time
  {
    static char buf[24000];
    if (n >= 3) {
      size_t len = lv_disc_save(c, n, buf, sizeof buf);
      FILE *f = fopen(DATA "/finder_cache.txt", "w");
      if (f) { fwrite(buf, 1, len, f); fclose(f); }
    } else {
      FILE *f = fopen(DATA "/finder_cache.txt", "r");
      if (f) { size_t len = fread(buf, 1, sizeof buf - 1, f); buf[len] = 0; fclose(f); if (lv_disc_load(buf, c, &n, 160) > 0) find_cached = 1; }
    }
  }
  // the rows: the sites found that aren't here; then the ones here, for their videos (from the copy
  // of the sites made when the finder opened: the list can change on the drawing thread meanwhile)
  pthread_mutex_lock(&fdmx);
  nfrows = 0;
  for (int k = 0; k < n && nfrows < FROWS; k++) {
    int here = 0;
    for (int s = 0; s < find_nsites && !here; s++) here = lv_disc_same_site(find_sites[s].base, c[k].base) && !find_sites[s].preset;
    if (here) continue;
    FRow *r = &frows[nfrows++];
    memset(r, 0, sizeof *r);
    r->c = c[k]; r->site = -1;
  }
  for (int s = 0; s < find_nsites && nfrows < FROWS; s++) {
    const LvSite *st = &find_sites[s];
    if (st->hidden || st->preset || st->kind == BK_E621 || vsrc_has_site(st->name)) continue;
    FRow *r = &frows[nfrows++];
    memset(r, 0, sizeof *r);
    snprintf(r->c.name, sizeof r->c.name, "%s", st->name);
    snprintf(r->c.base, sizeof r->c.base, "%s", st->base);
    r->c.kind = st->kind; r->site = s;
  }
  find_state = 2;
  pthread_mutex_unlock(&fdmx);
  // each asked, as the Browser would: the latest 40, and how many are videos
  for (int k = 0; h && k < nfrows; k++) {
    pthread_mutex_lock(&fdmx);
    FRow r = frows[k];
    frows[k].state = 1;
    pthread_mutex_unlock(&fdmx);
    LvSite site = {.name = r.c.name, .kind = r.c.kind, .base = r.c.base};
    if (r.site >= 0) site = find_sites[r.site];
    char url[600];
    int state = 3, videos = 0;
    char why[40] = "doesn't answer";
    const char *key = r.site >= 0 && wkeys[r.site][0] ? wkeys[r.site] : NULL;
    if (site.needs_key && !key) snprintf(why, sizeof why, "needs an account key");
    else {
      for (int pass = 0; pass < 2; pass++) {
        // (first as a picture site, then for its videos)
        lv_booru_url_ex(&site, pass ? video_tags_for(site.kind) : "", LV_R_ALL, LV_SORT_LATEST, 0, key, 0, url, sizeof url);
        uint8_t *b = NULL;
        size_t bn = 0;
        int st = lv_http_get(h, url, &b, &bn, 4u << 20, NULL, NULL, &err);
        int np = 0;
        LvPost *p = st == 200 && b ? lv_booru_parse(&site, (char *)b, bn, LV_R_ALL, &np) : NULL;
        if (!pass) {
          if (p && np > 0) state = 2;
          else if (st == 403 || st == 503 || (b && bn > 15 && strstr((char *)b, "Just a moment"))) snprintf(why, sizeof why, "browser check (Cloudflare)");
          else if (st == 200) snprintf(why, sizeof why, "answers, but not as a booru");
        } else for (int q = 0; q < np; q++) videos += lv_post_is_video(&p[q]);
        lv_posts_free(p, np);
        free(b);
        if (state != 2) break;
      }
    }
    pthread_mutex_lock(&fdmx);
    frows[k].state = state; frows[k].videos = videos;
    snprintf(frows[k].why, sizeof frows[k].why, "%s", why);
    pthread_mutex_unlock(&fdmx);
  }
  lv_http_free(h);
  find_state = 3;
  return NULL;
}
static void find_open(int tab) {
  find_tab = tab; find_sel = 0; find_scroll = 0;
  sheet = SH_FIND;
  if (find_state == 1 || find_state == 2) return; // (still going)
  find_state = 1; find_lists = 0; find_cached = 0;
  memcpy(find_sites, LV_SITES, sizeof find_sites); find_nsites = LV_NSITES;
  nvsources(); // (the video sources read here, on this thread: the finder's thread only looks at them)
  pthread_mutex_lock(&fdmx); nfrows = 0; pthread_mutex_unlock(&fdmx);
  lv_net_base(); // (the rest on the worker that fetches)
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 256 * 1024);
  if (pthread_create(&t, &at, find_main, NULL) == 0) pthread_detach(t);
  else { find_state = 3; say("It couldn't start"); }
}
// the rows a tab shows (into list: places in frows)
static int find_rows(int tab, int *list) {
  int m = 0;
  for (int k = 0; k < nfrows; k++) {
    const FRow *r = &frows[k];
    if (tab == 0 && r->site < 0) list[m++] = k;
    else if (tab == 1 && (r->state <= 1 || r->videos > 0 || r->state == 5) && (r->state != 3)) list[m++] = k;
  }
  return m;
}
static void find_add(int k) {
  FRow *r = &frows[k];
  if (r->state != 2) {
    if (r->state < 2) say("Still asking that site…");
    else if (r->state == 3) { sfx(SFX_ERROR); char m[96]; snprintf(m, sizeof m, "%s %s", r->c.name, r->why); say(m); }
    return;
  }
  int site = r->site;
  if (site < 0) {
    for (int s = 0; s < LV_NSITES; s++) if (lv_disc_same_site(LV_SITES[s].base, r->c.base) && !LV_SITES[s].preset) site = s;
    if (site < 0) {
      if (lv_booru_add_site(r->c.name, r->c.kind, r->c.base) != 0) { sfx(SFX_ERROR); say("No room for more sites (□ in the switcher removes one)"); return; }
      custom_sites_save();
      site = LV_NSITES - 1;
    }
  }
  if (find_tab == 1) {
    if (vsrc_add(LV_SITES[site].name, video_tags_for(LV_SITES[site].kind)) != 0) { sfx(SFX_ERROR); say("It's there already, or there's no room for more"); return; }
    r->state = 5;
    sfx(SFX_DONE);
    char m[96]; snprintf(m, sizeof m, "%s added to the video sources", LV_SITES[site].name); say(m);
  } else {
    r->state = 4;
    sfx(SFX_DONE);
    char m[96]; snprintf(m, sizeof m, "%s added to the sites", LV_SITES[site].name); say(m);
  }
  r->site = site;
}
static void draw_find_sheet(void) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 170));
  int wx = 60, wy = 18, ww = W - 120, wh = H - 36 - BAR + 40;
  rrect(wx, wy, ww, wh, 26, SURF);
  text(wx + 24, wy + 40, TEXT, 22, "Find sources");
  // the tabs
  static const char *tabs[] = {"Pictures", "Videos", "Comics"};
  static const char *tab_icons[] = {"image", "film", "book"};
  int segw = 390, tw3 = segw / 3, segx = wx + ww - segw - 20, segy = wy + 14;
  rrect(segx, segy, segw, 40, 20, SURF2);
  rrect(segx + 3 + find_tab * tw3, segy + 3, tw3 - 6, 34, 17, ACC);
  for (int t = 0; t < 3; t++) {
    unsigned c = t == find_tab ? RGBA(20, 12, 16, 255) : TEXT;
    int lw = text_w(16, tabs[t]) + 30, lx = segx + t * tw3 + (tw3 - lw) / 2;
    icon(tab_icons[t], lx + 11, segy + 20, c);
    text(lx + 28, segy + 26, c, 16, tabs[t]);
    if (hit(segx + t * tw3, segy, tw3, 40) && find_tab != t) { find_tab = t; find_sel = 0; find_scroll = 0; sfx(SFX_TAB); }
  }
  if (in.pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) { find_tab = (find_tab + ((in.pressed & SCE_CTRL_RTRIGGER) ? 1 : 2)) % 3; find_sel = 0; find_scroll = 0; sfx(SFX_TAB); }
  pthread_mutex_lock(&fdmx);
  int list[FROWS], m = find_tab < 2 ? find_rows(find_tab, list) : 0;
  int asking = 0, total = nfrows;
  for (int k = 0; k < nfrows; k++) asking += frows[k].state <= 1;
  // the line under the title: where it's at
  char st[160];
  if (find_state == 1) snprintf(st, sizeof st, "Reading the lists of boorus (gallery-dl, booru, Pybooru, Grabber)\u2026 %d of %d", find_lists, FIND_LISTS);
  else if (find_state == 2) snprintf(st, sizeof st, "Asking each site, as the Browser would… %d of %d", total - asking, total);
  else if (!find_lists && find_cached) snprintf(st, sizeof st, "The lists couldn't be read: what the last look found, asked again now");
  else if (!find_lists) snprintf(st, sizeof st, "The lists couldn't be read: is the Vita online?");
  else snprintf(st, sizeof st, "From %d lists: %d sites of the kinds the Browser reads (anime and hentai only)", find_lists, total);
  text_fit(wx + 24, wy + 72, FAINT, 14, st, ww - 48);
  if (find_state == 1 || find_state == 2) spinner(wx + ww - 34, wy + 66, 9);
  int top = wy + 90, rh = 66, vis = (wh - 150) / rh;
  if (find_tab == 2) {
    static const char *lines[] = {
      "The Comics tab reads MangaDex and E-Hentai: their own APIs and pages, which no list describes",
      "for other sites (each reader site is a program of its own, not a kind like the boorus).",
      "So there's nothing to add here yet: open Comics in the switcher for both.",
      "Comics already on the PC or in Google Drive show in the Library's Comics mode as they are."};
    for (int k = 0; k < 4; k++) text_fit(wx + 30, top + 40 + k * 30, k == 2 ? TEXT : DIM, 16, lines[k], ww - 60);
  } else {
    if (find_sel >= m) find_sel = m ? m - 1 : 0;
    if (find_sel < find_scroll) find_scroll = find_sel;
    if (find_sel >= find_scroll + vis) find_scroll = find_sel - vis + 1;
    if (in.touching && in.down_y > top) { static float acc; acc += in.drag_dy; if (acc < -50 && find_scroll + vis < m) { find_scroll++; acc = 0; } if (acc > 50 && find_scroll > 0) { find_scroll--; acc = 0; } }
    for (int j = 0; j < vis && find_scroll + j < m; j++) {
      int k = list[(int)find_scroll + j];
      FRow *r = &frows[k];
      int y = top + j * rh, on = (int)find_scroll + j == find_sel;
      if (on) rrect(wx + 14, y, ww - 28, rh - 6, 16, SURF2);
      draw_letter_icon(wx + 24, y + 8, 44, r->c.name);
      text_fit(wx + 80, y + 28, TEXT, 18, r->c.name, ww - 330);
      char sub[220];
      const char *host = strstr(r->c.base, "://");
      snprintf(sub, sizeof sub, "%s · %s%s", host ? host + 3 : r->c.base, KIND_NAMES[r->c.kind & 3], r->site >= 0 && find_tab == 1 && r->state < 4 ? " · in your sites" : "");
      if (find_tab == 1 && r->state == 2) { size_t o = strlen(sub); snprintf(sub + o, sizeof sub - o, " · %d of the latest 40 are videos", r->videos); }
      text_fit(wx + 80, y + 50, FAINT, 13, sub, ww - 330);
      // the button
      int bx = wx + ww - 200, by = y + 12;
      if (r->state <= 1) { spinner(bx + 150, by + 18, 9); text(bx + 40, by + 24, FAINT, 14, "asking…"); }
      else if (r->state == 3) text_fit(bx - 20, by + 24, ACC2, 13, r->why, 200);
      else if (r->state >= 4) { rrect(bx + 40, by, 140, 36, 18, SURF2); text_center(bx + 110, by + 24, DIM, 15, "✓ Added"); }
      else {
        rrect(bx + 40, by, 140, 36, 18, ACC);
        icon("plus", bx + 64, by + 18, RGBA(20, 12, 16, 255));
        text(bx + 80, by + 24, RGBA(20, 12, 16, 255), 15, "Add");
        if (hit(bx + 40, by, 140, 36)) { find_sel = (int)find_scroll + j; in.pressed |= SCE_CTRL_CROSS; }
      }
      if (hit(wx + 14, y, ww - 260, rh - 6)) find_sel = (int)find_scroll + j;
    }
    if (!m && find_state == 3) text_center(W / 2, top + 80, DIM, 17, find_tab ? "No more sites with videos found" : "Every site the lists know (of the kinds the Browser reads) is here already");
    if (in.pressed & SCE_CTRL_DOWN) { if (find_sel + 1 < m) { find_sel++; sfx(SFX_TICK); } }
    if (in.pressed & SCE_CTRL_UP) { if (find_sel > 0) { find_sel--; sfx(SFX_TICK); } }
    if ((in.pressed & SCE_CTRL_CROSS) && m) find_add(list[find_sel]);
  }
  pthread_mutex_unlock(&fdmx);
  // the foot: by its address instead (the old way), and closing
  int fy = wy + wh - 52, bw = text_w(15, "Type an address") + 50;
  rrect(wx + 20, fy, bw, 38, 19, SURF2);
  icon("search", wx + 42, fy + 19, TEXT);
  text(wx + 58, fy + 25, TEXT, 15, "Type an address");
  text(wx + 40 + bw, fy + 25, FAINT, 13, "□ type an address   ○ close");
  if ((hit(wx + 20, fy, bw, 38) || (in.pressed & SCE_CTRL_SQUARE)) && !ime_open) { sheet = SH_NONE; ime_start(14, "The site's address (like booru.example.org)", "", 0); }
  else if ((in.pressed & SCE_CTRL_CIRCLE) || (in.tap && !(in.tap_x >= wx && in.tap_x < wx + ww && in.tap_y >= wy && in.tap_y < wy + wh))) { sfx(SFX_CLOSE); sheet = SH_SITES; }
  in.pressed = 0;
  in.tap = 0;
}

// The site switcher: a window with two tabs, Pictures (the sites, and + to add one) and Videos (the
// video sources, opening the video browser); each with its icon (sites/*.png).
static int sites_tab;      // 0 pictures, 1 videos, 2 manga
static float sites_scroll;
// (the icons by slot: the sites 0-30, the add tile 31, the video sources 32-63, the comics' 64-95)
static vita2d_texture *site_tex[96];
static int site_tex_tried[96];
static vita2d_texture *site_icon_named(int slot, const char *name) {
  if (slot < 0 || slot >= 96 || !name) return NULL;
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
// ---------- Check sources: each source of a tab asked once; the ones that don't answer go grey, last ----------
// (kept by name, for the next launch too: ux0:data/LWDV00001/source_status.txt, "<tab>\t<name>\t<0/1>")
typedef struct { char name[48]; int tab, st; } SrcStat; // st: 1 asking, 2 answers, 3 doesn't
static SrcStat srcstat[64];
static int nsrcstat, srcstat_loaded;
static pthread_mutex_t ssmx = PTHREAD_MUTEX_INITIALIZER;
static volatile int check_left; // (still to ask)
static SrcStat *srcstat_of(int tab, const char *name, int make) {
  for (int k = 0; k < nsrcstat; k++) if (srcstat[k].tab == tab && !strcmp(srcstat[k].name, name)) return &srcstat[k];
  if (!make || nsrcstat >= 64) return NULL;
  SrcStat *s = &srcstat[nsrcstat++];
  memset(s, 0, sizeof *s);
  snprintf(s->name, sizeof s->name, "%s", name);
  s->tab = tab;
  return s;
}
static void srcstat_load(void) {
  srcstat_loaded = 1;
  FILE *f = fopen(DATA "/source_status.txt", "r");
  char ln[120];
  while (f && fgets(ln, sizeof ln, f)) {
    ln[strcspn(ln, "\r\n")] = 0;
    char *a = strchr(ln, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
    if (!a || !b) continue;
    *a = 0; *b = 0;
    SrcStat *s = srcstat_of(atoi(ln), a + 1, 1);
    if (s) s->st = atoi(b + 1) ? 2 : 3;
  }
  if (f) fclose(f);
}
static void srcstat_save(void) {
  FILE *f = fopen(DATA "/source_status.txt", "w");
  pthread_mutex_lock(&ssmx);
  for (int k = 0; f && k < nsrcstat; k++) if (srcstat[k].st >= 2) fprintf(f, "%d\t%s\t%d\n", srcstat[k].tab, srcstat[k].name, srcstat[k].st == 2);
  pthread_mutex_unlock(&ssmx);
  if (f) fclose(f);
}
// (read on the drawing thread while the checking thread writes: under its lock)
static int src_state(int tab, const char *name) {
  if (!srcstat_loaded) srcstat_load();
  // (the sources window asks three or four times per tile per frame: the states copied once a frame
  //  under the lock, then looked up in the copy)
  static SrcStat snap[64];
  static int nsnap;
  static unsigned at = ~0u;
  if (at != frame_no) {
    at = frame_no;
    pthread_mutex_lock(&ssmx);
    nsnap = nsrcstat;
    memcpy(snap, srcstat, nsnap * sizeof *snap);
    pthread_mutex_unlock(&ssmx);
  }
  for (int k = 0; k < nsnap; k++) if (snap[k].tab == tab && !strcmp(snap[k].name, name)) return snap[k].st;
  return 0;
}
static int src_failing(int tab, const char *name) { return src_state(tab, name) == 3; }
static int src_asking(int tab, const char *name) { return src_state(tab, name) == 1; }
typedef struct { int tab, kind, msrc; char name[48], url[1200]; LvSite site; } CheckJob;
static CheckJob *check_jobs;
static int ncheck_jobs;
static void *check_main(void *arg) {
  (void)arg;
  pin_to_core(2);
  LvHttp *h = lv_http_new();
  for (int k = 0; k < ncheck_jobs; k++) {
    CheckJob *j = &check_jobs[k];
    uint8_t *b = NULL;
    size_t n = 0;
    const char *err = NULL;
    int ok = 0;
    int st = h ? lv_http_get(h, j->url, &b, &n, 6u << 20, NULL, NULL, &err) : -1;
    if (st == 200 && b) {
      if (j->tab == 2 && j->msrc == MS_MANGADEX) { LvBook *bk = calloc(40, sizeof *bk); int tot = 0; ok = bk && lv_md_parse_list((char *)b, n, bk, 40, &tot) > 0; free(bk); }
      else if (j->tab == 2) { long long g[25]; char t[25][16]; ok = lv_eh_parse_list((char *)b, g, t, 25) > 0; }
      else { int np = 0; LvPost *p = lv_booru_parse(&j->site, (char *)b, n, LV_R_ALL, &np); ok = np > 0; lv_posts_free(p, np); }
    }
    free(b);
    pthread_mutex_lock(&ssmx);
    SrcStat *s = srcstat_of(j->tab, j->name, 1);
    if (s) s->st = ok ? 2 : 3;
    pthread_mutex_unlock(&ssmx);
    check_left--;
  }
  lv_http_free(h);
  free(check_jobs); check_jobs = NULL; ncheck_jobs = 0;
  srcstat_save();
  check_left = 0;
  return NULL;
}
static void check_sources(int tab) {
  if (check_left > 0) return;
  if (!srcstat_loaded) srcstat_load();
  int vis[LV_MAX_SITES], nvis = sites_visible(vis);
  int n = tab == 2 ? nmsources() : tab ? nvsources() : nvis;
  CheckJob *jobs = calloc(n + 1, sizeof *jobs);
  if (!jobs) return;
  int m = 0;
  for (int k = 0; k < n; k++) {
    CheckJob *j = &jobs[m];
    j->tab = tab;
    if (tab == 2) {
      j->msrc = k;
      snprintf(j->name, sizeof j->name, "%s", msource_name(k));
      if (k == MS_MANGADEX) lv_md_list_url("", 1, 0, 0, j->url, sizeof j->url); else lv_eh_list_url("", 0, j->url, sizeof j->url);
    } else {
      int site = tab ? vsource_site(k) : vis[k];
      snprintf(j->name, sizeof j->name, "%s", tab ? vsource_name(k) : LV_SITES[site].name);
      if (site < 0) { // (the locked ones: they don't answer an app)
        SrcStat *s = srcstat_of(tab, j->name, 1);
        if (s) s->st = 3;
        continue;
      }
      j->site = LV_SITES[site];
      if (j->site.needs_key && !wkeys[site][0]) { // (it would answer, with a key: not a failing source)
        SrcStat *s = srcstat_of(tab, j->name, 1);
        if (s) s->st = 4;
        continue;
      }
      lv_booru_url_ex(&j->site, tab ? vsource_tags(k) : "", LV_R_ALL, LV_SORT_LATEST, 0, wkeys[site][0] ? wkeys[site] : NULL, 1, j->url, sizeof j->url);
    }
    SrcStat *s = srcstat_of(tab, j->name, 1);
    if (s) s->st = 1;
    m++;
  }
  check_jobs = jobs; ncheck_jobs = m; check_left = m;
  lv_net_base(); // (the rest on the worker that fetches)
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 256 * 1024);
  if (pthread_create(&t, &at, check_main, NULL) == 0) { pthread_detach(t); sfx(SFX_OPEN); say("Checking each source\u2026"); }
  else { free(jobs); check_jobs = NULL; ncheck_jobs = 0; check_left = 0; say("It couldn't start"); }
}

static void draw_sites_sheet(void) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 170));
  const int cols = 6, tw = 142, th = 136, gap = 10;
  int vis[LV_MAX_SITES], nvis = sites_visible(vis);
  int n = (sites_tab == 2 ? nmsources() : sites_tab ? nvsources() : nvis) + 1; // (+ the add tile)
  int rows = (n + cols - 1) / cols, vis_rows = rows < 3 ? rows : 3;
  int pages = (rows + 2) / 3; // (pages of three rows: eighteen tiles a page)
  int ww = cols * tw + (cols - 1) * gap + 36, wh = vis_rows * th + (vis_rows - 1) * gap + 100;
  int wx = (W - ww) / 2, wy = 18;
  rrect(wx, wy, ww, wh, 26, SURF);
  // the tabs: a segmented control
  static const char *tabs[] = {"Pictures", "Videos", "Comics"};
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
  }
  // (the tab may have changed, by L/R or a tap: its own count, its list loaded)
  n = (sites_tab == 2 ? nmsources() : sites_tab ? nvsources() : nvis) + 1;
  rows = (n + cols - 1) / cols;
  if (sheet_sel >= n) sheet_sel = n - 1;
  // the order: the sources that answered (or weren't checked) first, the ones that didn't last
  int ord[64];
  {
    int m = 0;
    for (int pass = 0; pass < 2; pass++)
      for (int k = 0; k < n - 1 && k < 63; k++) {
        const char *nm = sites_tab == 2 ? msource_name(k) : sites_tab ? vsource_name(k) : LV_SITES[vis[k]].name;
        if (src_failing(sites_tab, nm) == pass) ord[m++] = k;
      }
    ord[m] = n - 1;
  }
  // Check sources, top right
  {
    char cl[48];
    if (check_left > 0) snprintf(cl, sizeof cl, "Checking\u2026 %d", check_left); else snprintf(cl, sizeof cl, "Check sources");
    int bw = text_w(15, cl) + 50, bx = wx + ww - bw - 18, by = wy + 14;
    rrect(bx, by, bw, 40, 20, SURF2);
    if (check_left > 0) spinner(bx + 22, by + 20, 8); else icon("refresh", bx + 22, by + 20, TEXT);
    text(bx + 38, by + 26, TEXT, 15, cl);
    if (hit(bx, by - 4, bw, 48) || (in.pressed & SCE_CTRL_TRIANGLE)) { check_sources(sites_tab); in.tap = 0; in.pressed &= ~SCE_CTRL_TRIANGLE; }
  }
  // (the chosen tile's page shown)
  int srow = sheet_sel / cols;
  sites_scroll = (float)(srow / 3 * 3);
  int top = wy + 68;
  for (int k = 0; k < n; k++) {
    int r = k / cols - (int)sites_scroll;
    if (r < 0 || r >= vis_rows) continue;
    int x = wx + 18 + (k % cols) * (tw + gap), y = top + r * (th + gap);
    int on = k == sheet_sel;
    const char *name, *about, *ic;
    int cur, blocked = 0, add = k == n - 1, slot;
    int kk = k;
    k = ord[k]; // (the k-th tile shows this source)
    int site = !sites_tab && k < nvis ? vis[k] : -1;
    if (add) { name = sites_tab == 2 ? "More comics" : sites_tab ? "Add a source" : "Add a site"; about = "Found for you, one press"; ic = "add"; cur = 0; slot = 31; }
    else if (sites_tab == 2) { name = msource_name(k); about = msource_about(k); ic = msource_icon(k); cur = screen == S_MANGA && k == mg_src_now(); slot = 64 + k; }
    else if (sites_tab) { name = vsource_name(k); about = vsource_about(k); ic = vsource_icon(k); cur = k == vb_cur; blocked = vsource_blocked(k); slot = 32 + k; }
    else { name = LV_SITES[site].name; about = LV_SITES[site].about; ic = LV_SITES[site].icon; cur = site == cfg.site; slot = site; }
    rrect(x, y, tw, th, 18, on ? SURF2 : with_alpha(SURF2, 140));
    if (on) focus_ring(x, y, tw, th);
    vita2d_texture *t = site_icon_named(slot, ic);
    int isz = 70;
    if (t) vita2d_draw_texture_tint_scale(t, x + tw / 2 - isz / 2, y + 10, isz / 112.0f, isz / 112.0f, blocked ? RGBA(255, 255, 255, 110) : RGBA(255, 255, 255, 255));
    else draw_letter_icon(x + tw / 2 - isz / 2, y + 10, isz, name);
    text_fit(x + 10, y + 100, cur ? ACC : TEXT, 15, name, tw - 20);
    if (about) text_fit(x + 10, y + 122, FAINT, 12, about, tw - 20);
    if (cur) { circle(x + tw - 16, y + 16, 10, ACC); text_center(x + tw - 16, y + 21, RGBA(20, 12, 16, 255), 13, "✓"); }
    if (sites_tab == 1 && !add && vsrc_is_added(k)) { rrect(x + 8, y + 8, 50, 20, 10, RGBA(0, 0, 0, 160)); text_center(x + 33, y + 23, DIM, 12, "\u25a1 off"); }
    // (an image site, not a booru: said so on its tile)
    if (!sites_tab && !add && site >= 0 && !lv_site_is_booru(&LV_SITES[site])) { int lw = text_w(11, "IMAGE SITE") + 12; rrect(x + 8, y + 8, lw, 18, 9, RGBA(70, 150, 220, 220)); text(x + 14, y + 21, TEXT, 11, "IMAGE SITE"); }
    if (!sites_tab && !add && LV_SITES[site].needs_key && !wkeys[site][0]) { rrect(x + 8, y + 8, 36, 20, 10, RGBA(0, 0, 0, 160)); text_center(x + 26, y + 23, DIM, 12, "key"); }
    if (blocked) { rrect(x + 8, y + 8, 50, 20, 10, RGBA(0, 0, 0, 170)); text_center(x + 33, y + 23, DIM, 12, "locked"); }
    // (a source that didn't answer when checked: grey; one being asked: a small spinner)
    if (!add && src_failing(sites_tab, name)) { rrect(x, y, tw, th, 18, RGBA(20, 16, 22, 170)); rrect(x + tw - 76, y + 8, 68, 20, 10, RGBA(0, 0, 0, 190)); text_center(x + tw - 42, y + 23, ACC2, 12, "no answer"); }
    else if (!add && src_asking(sites_tab, name)) spinner(x + tw - 18, y + 18, 7);
    else if (!add && src_state(sites_tab, name) == 2) { circle(x + tw - 16, y + 95, 5, RGBA(90, 214, 120, 255)); }
    k = kk;
    if (hit(x, y, tw, th)) { sheet_sel = k; in.pressed |= SCE_CTRL_CROSS; }
  }
  if (pages > 1) {
    // the pages: a dot each, arrows either side; a swipe sideways across the tiles turns the page
    // (in the window's top row, at the left, where "L / R" is otherwise)
    int page = (int)sites_scroll / 3, dy = wy + 34, lx = wx + 14, dw = pages * 18;
    rrect(wx + 8, wy + 12, dw + 100, 44, 22, SURF);
    int turn = 0;
    rrect(lx, dy - 18, 36, 36, 18, page > 0 ? SURF2 : with_alpha(SURF2, 80));
    icon("chev_l", lx + 18, dy, page > 0 ? TEXT : FAINT);
    for (int p = 0; p < pages; p++) {
      circle(lx + 46 + p * 18 + 9, dy, p == page ? 6 : 4, p == page ? ACC : with_alpha(TEXT, 70));
      if (hit(lx + 46 + p * 18, dy - 16, 18, 32)) { sheet_sel = p * 18 < n ? p * 18 : sheet_sel; in.tap = 0; sfx(SFX_TICK); }
    }
    int rx = lx + 46 + dw + 6;
    rrect(rx, dy - 18, 36, 36, 18, page < pages - 1 ? SURF2 : with_alpha(SURF2, 80));
    icon("chev_r", rx + 18, dy, page < pages - 1 ? TEXT : FAINT);
    if (hit(lx, dy - 20, 36, 40)) { turn = -1; in.tap = 0; }
    if (hit(rx, dy - 20, 36, 40)) { turn = 1; in.tap = 0; }
    if (in.flicked && in.down_y > top && fabsf(in.fvx) > 12 && fabsf(in.fvx) > 2 * fabsf(in.fvy)) turn = in.fvx < 0 ? 1 : -1;
    if (turn && page + turn >= 0 && page + turn < pages) {
      int np = page + turn, k = np * 18 + sheet_sel % 18;
      sheet_sel = k < n ? k : n - 1;
      sfx(SFX_TICK);
    }
  }
  int col = sheet_sel % cols, row = sheet_sel / cols;
  if (in.pressed & SCE_CTRL_RIGHT) sheet_sel = sheet_sel + 1 < n ? sheet_sel + 1 : 0;
  if (in.pressed & SCE_CTRL_LEFT) sheet_sel = sheet_sel > 0 ? sheet_sel - 1 : n - 1;
  if (in.pressed & SCE_CTRL_DOWN) { row = (row + 1) % rows; sheet_sel = row * cols + col < n ? row * cols + col : col; }
  if (in.pressed & SCE_CTRL_UP) { row = (row + rows - 1) % rows; while (row * cols + col >= n) row--; sheet_sel = row * cols + col; }
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) sfx(SFX_TICK);
  int pick = sheet_sel >= 0 && sheet_sel < n ? ord[sheet_sel] : 0; // (the source the chosen tile shows)
  if ((in.pressed & SCE_CTRL_CROSS) && sheet_sel == n - 1) { sfx(SFX_OPEN); find_open(sites_tab); }
  else if ((in.pressed & SCE_CTRL_SQUARE) && sites_tab == 1 && vsrc_is_added(pick)) { vsrc_remove(pick); sfx(SFX_DELETE); say("Video source removed"); }
  else if (in.pressed & SCE_CTRL_CROSS) {
    if (sites_tab == 2) { sfx(SFX_SELECT); sheet = SH_NONE; manga_open(pick); }
    else if (sites_tab) {
      if (vsource_blocked(pick)) { sfx(SFX_ERROR); say("This site only answers after a browser check (Cloudflare), so the Vita can't reach it"); }
      else { sfx(SFX_SELECT); sheet = SH_NONE; vb_open(pick); }
    } else { int st = vis[pick]; sfx(SFX_SELECT); sheet = SH_NONE; if (st != cfg.site) { cfg.site = st; if (!LV_SITES[cfg.site].hot && cfg.wsort == LV_SORT_HOT) cfg.wsort = LV_SORT_LATEST; web_search(); } }
  }
  // □ on a site added by hand: removed
  else if ((in.pressed & SCE_CTRL_SQUARE) && !sites_tab && pick < nvis && vis[pick] >= LV_NBUILTIN) {
    for (int k = vis[pick]; k < LV_NSITES - 1; k++) LV_SITES[k] = LV_SITES[k + 1];
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
  static WTagCount tab[CAP]; // (one table kept, cleared each time: not 200 KB asked of the heap for every count)
  memset(tab, 0, sizeof tab);
  nwtags = 0;
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
}
enum { SR_TYPE, SR_MARK, SR_CLEAR, SR_HEAD, SR_TAG, SR_RECENT, SR_BMARK, SR_CHIPS, SR_AC };
// ---- completing the last word typed: the tags it could be (from these results' tags and the words of
// past searches), shown after the keyboard closes: in the sheet and as chips over the grid ----
static char ac_list[10][48];
static int nac, ac_chip;
static unsigned ac_until; // (the chips over the grid: until then)
static void wcount_tags(void);
static void ac_build(const char *q) {
  nac = 0;
  const char *w = strrchr(q, ' ');
  w = w ? w + 1 : q;
  if (*w == '-' || *w == '~') w++;
  size_t n = strlen(w);
  if (n < 2) return;
  wcount_tags();
  const char *cand[64];
  int nc = 0;
  for (int k = 0; k < nwtags && nc < 64; k++) cand[nc++] = wtags[k].t;
  // (the words of past searches)
  static char words[48][48];
  int nw = 0;
  for (int k = 0; k < nwrecent && nw < 48; k++) {
    char b[160];
    snprintf(b, sizeof b, "%s", wrecent[k].q);
    for (char *s = strtok(b, " "); s && nw < 48; s = strtok(NULL, " ")) { if (*s == '-' || *s == '~') s++; snprintf(words[nw++], 48, "%s", s); }
  }
  for (int k = 0; k < nw && nc < 64; k++) cand[nc++] = words[k];
  // starting with it first, then containing it; not itself; once each
  for (int pass = 0; pass < 2; pass++)
    for (int k = 0; k < nc && nac < 10; k++) {
      const char *c = cand[k];
      if (!strcasecmp(c, w)) continue;
      int ok = pass == 0 ? !strncasecmp(c, w, n) : strncasecmp(c, w, n) && strcasestr(c, w) != NULL;
      if (!ok) continue;
      int dup = 0;
      for (int j = 0; j < nac; j++) dup |= !strcmp(ac_list[j], c);
      if (!dup) snprintf(ac_list[nac++], 48, "%s", c);
    }
  ac_chip = 0;
  ac_until = nac ? frame_no + 600 : 0;
}
// the last word put in place of one of them, and searched again
static void ac_apply(int k) {
  if (k < 0 || k >= nac) return;
  char q[128];
  snprintf(q, sizeof q, "%s", cfg.wquery);
  char *w = strrchr(q, ' ');
  w = w ? w + 1 : q;
  if (*w == '-' || *w == '~') w++;
  *w = 0;
  snprintf(cfg.wquery, sizeof cfg.wquery, "%s%s", q, ac_list[k]);
  nac = 0; ac_until = 0;
  sfx(SFX_SELECT);
  web_search();
}
// chips: a row of them from x, at most to x + w; the one chosen (sel, or -1) ringed. Returns the one
// tapped, or -1.
static int chips_row(int x, int y, int w, const char *const *labels, int n, int sel) {
  int cx = x, got = -1;
  for (int k = 0; k < n; k++) {
    int cw = text_w(15, labels[k]) + 26;
    if (cx + cw > x + w) break;
    rrect(cx, y, cw, 30, 15, k == sel ? with_alpha(ACC, 230) : SURF2);
    text(cx + 13, y + 21, k == sel ? RGBA(20, 12, 16, 255) : TEXT, 15, labels[k]);
    if (hit(cx, y, cw, 30)) got = k;
    cx += cw + 8;
  }
  return got;
}
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
  if (nac && cfg.wquery[0]) rows[n++] = (SRow){SR_AC, 0};
  // (the recent searches on this site, as chips under it)
  const char *chip_l[8];
  int chip_i[8], nchip = 0;
  for (int k = 0; k < nwrecent && nchip < 8; k++) if (wrecent[k].site == cfg.site && wrecent[k].q[0]) { chip_i[nchip] = k; chip_l[nchip++] = wrecent[k].q; }
  if (nchip) rows[n++] = (SRow){SR_CHIPS, 0};
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
      case SR_CHIPS: {
        static int chip_sel;
        if (chip_sel >= nchip) chip_sel = nchip - 1;
        if (k == sheet_sel && (in.pressed & SCE_CTRL_RIGHT) && chip_sel < nchip - 1) { chip_sel++; sfx(SFX_TICK); }
        if (k == sheet_sel && (in.pressed & SCE_CTRL_LEFT) && chip_sel > 0) { chip_sel--; sfx(SFX_TICK); }
        int t = chips_row(x + 20, ry + 6, w - 40, chip_l, nchip, k == sheet_sel ? chip_sel : -1);
        if (t >= 0) { chip_sel = t; sheet_sel = k; in.pressed |= SCE_CTRL_CROSS; in.tap = 0; }
        r.i = chip_i[chip_sel < 0 ? 0 : chip_sel];
        rows[k].i = r.i;
        break;
      }
      case SR_AC: {
        const char *l[10];
        for (int j = 0; j < nac; j++) l[j] = ac_list[j];
        if (k == sheet_sel && (in.pressed & SCE_CTRL_RIGHT) && ac_chip < nac - 1) { ac_chip++; sfx(SFX_TICK); }
        if (k == sheet_sel && (in.pressed & SCE_CTRL_LEFT) && ac_chip > 0) { ac_chip--; sfx(SFX_TICK); }
        int t = chips_row(x + 20, ry + 6, w - 40, l, nac, k == sheet_sel ? ac_chip : -1);
        if (t >= 0) { ac_chip = t; sheet_sel = k; in.pressed |= SCE_CTRL_CROSS; in.tap = 0; }
        break;
      }
      case SR_TAG: text(x + 30, ry + 27, TEXT, 17, wtags[r.i].t); snprintf(b, sizeof b, "%d", wtags[r.i].n); text(x + w - 26 - text_w(14, b), ry + 27, FAINT, 14, b); break;
      case SR_RECENT: case SR_BMARK: {
        const WSearch *s = r.kind == SR_RECENT ? &wrecent[r.i] : &wmarks[r.i];
        text_fit(x + 30, ry + 27, TEXT, 17, s->q, w - 170);
        text(x + w - 26 - text_w(13, LV_SITES[s->site].name), ry + 27, FAINT, 13, LV_SITES[s->site].name);
        break;
      }
    }
    if (r.kind != SR_CHIPS && r.kind != SR_AC && hit(x, ry, w, rh)) { sheet_sel = k; in.pressed |= SCE_CTRL_CROSS; }
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
      case SR_AC: if (go) { sheet = SH_NONE; ac_apply(ac_chip); } break;
      case SR_CHIPS: r.kind = SR_RECENT; // (a chip: a recent search, as its row would be)
      /* fall through */
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
  {
    static float last;
    if (!sheet_open) chrome_follow(wscroll - last, wscroll); else chrome_target = 0;
    last = wscroll;
  }

  int first = grid_first(&wgrid, wscroll - 8);
  static int wlast_top_row = -1;
  if (first < wgrid.n && wgrid.row[first] != wlast_top_row) { if (wlast_top_row >= 0 && (in.touching || wscroll_v != 0 || in.ly)) sfx(SFX_TICK); wlast_top_row = wgrid.row[first]; }
  int wanted[48], nw = 0, last = first;
  // (a tap on the "Did you mean" strip is the strip's, not the cards' under it)
  int strip_on = nac && !sheet_open && (frame_no < ac_until || (!nposts && !wloading));
  int strip_tap = strip_on && in.tap && in.tap_y >= TOP + 2 && in.tap_y < TOP + 44;
  if (strip_tap) in.tap = 0;
  int flinging = fabsf(wscroll_v) > 30; // (flung fast: no previews asked for in passing; rings shown)
  float fr_x = -1, fr_y = 0, fr_w = 0, fr_h = 0;
  for (int k = first; k < nposts && k < wgrid.n; k++) {
    Rect rc = wgrid.r[k];
    float x = rc.x, y = TOP + 6 + rc.y - wscroll, cw = rc.w, ch = rc.h;
    if (y > H) break;
    last = k + 1;
    if (y + ch < TOP - 4) continue;
    LvPost *p = &posts[k];
    vita2d_texture *t = preview_of(k);
    int on = k == wsel && !in.touching && !sheet_open;
    float g = on ? 3 : 0;
    if (on) { fr_x = x - g; fr_y = y - g; fr_w = cw + 2 * g; fr_h = ch + 2 * g; }
    if (t) draw_cover(t, x - g, y - g, cw + 2 * g, ch + 2 * g, RGBA(255, 255, 255, 255));
    else {
      if (wstate[k] == 3) { rrect(x - g, y - g, cw + 2 * g, ch + 2 * g, 8, SURF); text_center(x + cw / 2, y + ch / 2, FAINT, 14, "No preview"); }
      else {
        skeleton(x - g, y - g, cw + 2 * g, ch + 2 * g, 8);
        spinner(x + cw / 2, y + ch / 2, 8); // (a loading ring: the grid never waits for its preview)
        if (!flinging && nw < 48) wanted[nw++] = k;
      }
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
  if (fr_x >= 0) focus_ring(fr_x, fr_y, fr_w, fr_h);
  // (the last word typed could be one of these: chips over the grid for a while, or while there's
  //  nothing found)
  if (strip_on && nac) {
    if (strip_tap) in.tap = 1;
    const char *l[10];
    for (int j = 0; j < nac; j++) l[j] = ac_list[j];
    int y = TOP + 8;
    int lw = text_w(14, "Did you mean") + 24;
    rrect(14, y - 4, W - 28, 38, 19, with_alpha(SURF, 235));
    text(28, y + 19, DIM, 14, "Did you mean");
    int t = chips_row(14 + lw, y, W - 28 - lw - 40, l, nac, -1);
    rrect(W - 52, y, 30, 30, 15, SURF2);
    text_center(W - 37, y + 21, DIM, 14, "\u2715");
    if (hit(W - 56, y - 4, 40, 38)) { nac = 0; ac_until = 0; in.tap = 0; }
    else if (t >= 0) { in.tap = 0; ac_apply(t); return; }
  }
  // (the next screenful the way it's going)
  if (!flinging) {
    int up = wscroll_v < -1 || in.ly < -0.2f || (in.touching && in.drag_dy > 2);
    for (int j = 0; j < 18 && nw < 48; j++) {
      int k = up ? first - 1 - j : last + j;
      if (k < 0 || k >= nposts) break;
      if (!preview_of(k) && wstate[k] != 3) wanted[nw++] = k;
    }
  }
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

  // --- the top: one row: the site (the tab button), the search, the filter, the sort, the light ---
  int hy = HDR_Y;
  vita2d_draw_rectangle(0, hy, W, TOP_FULL, with_alpha(BG, 250));
  char site[80];
  snprintf(site, sizeof site, "%s", LV_SITES[cfg.site].name);
  int ws = text_w(17, site) + 84;
  if (ws > 250) ws = 250;
  rrect(10, hy + 8, ws, 40, 20, ACC);
  vita2d_texture *sic = site_icon_named(cfg.site, LV_SITES[cfg.site].icon);
  if (sic) vita2d_draw_texture_scale(sic, 16, hy + 12, 32 / 112.0f, 32 / 112.0f);
  text_fit(sic ? 54 : 26, hy + 34, RGBA(20, 12, 16, 255), 17, site, ws - (sic ? 74 : 50));
  icon("chev_r", 10 + ws - 18, hy + 28, RGBA(20, 12, 16, 255));
  if (!sheet_open && hit(10, hy + 4, ws, 48)) { sfx(SFX_OPEN); sheet = SH_SITES; sheet_sel = site_tile_of(cfg.site); sites_tab = 0; sites_scroll = 0; }
  // filter, sort and the light on the right
  char filt[40];
  int nr = 0;
  for (int k = 0; k < 4; k++) nr += (cfg.ratings >> k) & 1;
  if (nr == 4) snprintf(filt, sizeof filt, "Filter");
  else { snprintf(filt, sizeof filt, "Filter:"); for (int k = 0; k < 4; k++) if (cfg.ratings & (1 << k)) { char c[3] = {' ', RATING_NAMES[k][0], 0}; strcat(filt, c); } }
  int rx = W - 12, hb;
  rx -= online_button(rx) + 8;
  rx -= head_button(rx, "sort", LV_SORT_NAMES[cfg.wsort], SURF2, &hb) + 8;
  if (!sheet_open && (hb || (in.pressed & SCE_CTRL_TRIANGLE))) {
    sfx(SFX_TOGGLE);
    do cfg.wsort = (cfg.wsort + 1) % LV_NSORTS; while (cfg.wsort == LV_SORT_HOT && !LV_SITES[cfg.site].hot);
    web_search();
  }
  rx -= head_button(rx, "sliders", filt, nr == 4 ? SURF2 : with_alpha(ACC, 110), &hb) + 8;
  if (!sheet_open && (hb || (in.pressed & SCE_CTRL_SQUARE))) { sfx(SFX_OPEN); sheet = SH_FILTER; sheet_sel = 0; in.pressed &= ~SCE_CTRL_SQUARE; }
  // the search field: the room between
  int qx = 10 + ws + 8, wq = rx - qx - (cfg.wquery[0] ? 48 : 0);
  rrect(qx, hy + 8, wq, 40, 20, SURF2);
  icon("search", qx + 22, hy + 28, cfg.wquery[0] ? TEXT : FAINT);
  text_fit(qx + 40, hy + 34, cfg.wquery[0] ? TEXT : FAINT, 16, cfg.wquery[0] ? cfg.wquery : "Search tags", wq - 50);
  if (!sheet_open && hit(qx, hy + 4, wq, 48) && !ime_open) open_search_sheet();
  if (cfg.wquery[0]) {
    int cx = qx + wq + 6;
    rrect(cx, hy + 8, 40, 40, 20, SURF2);
    icon("xmark", cx + 20, hy + 28, DIM);
    if (!sheet_open && hit(cx, hy + 4, 40, 48)) { cfg.wquery[0] = 0; web_search(); }
  }

  if (!sheet_open) {
    // a flick sideways across the grid: the next or previous site, as L / R
    if (in.flicked && in.down_y > TOP && fabsf(in.fvx) > 16 && fabsf(in.fvx) > 2.5f * fabsf(in.fvy))
      in.pressed |= in.fvx < 0 ? SCE_CTRL_RTRIGGER : SCE_CTRL_LTRIGGER;
    // the sites are tabs: L / R go through them
    if (in.pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
      sfx(SFX_TAB);
      { // (in the sites window's order: the boorus, then the image sites)
        int vis[LV_MAX_SITES], nv = sites_visible(vis), at = 0;
        for (int k = 0; k < nv; k++) if (vis[k] == cfg.site) at = k;
        if (nv) cfg.site = vis[(at + ((in.pressed & SCE_CTRL_RTRIGGER) ? 1 : nv - 1)) % nv];
      }
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
  else if (sheet == SH_FIND) draw_find_sheet();
  if (wkey_ask && !ime_open) { wkey_ask = 0; ime_start(11, "API key", "", 0); }
}

// ---------- the full view's drawing ----------


// ---------- the save page: a few steps, each the whole screen, then an overview ----------
// Opened by Save (or □) in the Browser's full view: 1 how explicit (the level, 0-10), 2 the
// category, 3 stars and the site's tags, then the overview (a tap on a row goes back to its step)
// and Save. The picture stays on the left all along. ← → change, ✕ next, ○ back, △ save now.
static int sw_on, sw_step, sw_catsel;
static float sw_anim;
static const char *SW_STEPS[4] = {"Level", "Category", "Stars and tags", "Overview"};
static int sw_ncats(void) { return (loaded_vault ? vault.ncats : 0) + 2; } // (the usual one, the vault's, a new one)
static const char *sw_cat_name(int k) {
  if (k == 0) return save_category();
  if (k - 1 < vault.ncats) return vault.cats[k - 1];
  // (a name just typed, not a category yet: it shows here until the save makes it one)
  if (sw_cat[0]) { int known = 0; for (int c = 0; c < vault.ncats; c++) if (!strcmp(vault.cats[c], sw_cat)) known = 1; if (!known) return sw_cat; }
  return "+ New category";
}
static void sw_open(void) {
  if (wv >= nposts) return;
  if (is_saved(&posts[wv])) { sfx(SFX_ERROR); say("Already in your vault"); return; }
  if (!loaded_vault) { say("Open a vault first"); return; }
  const LvPost *p = &posts[wv];
  int ri = p->rating == LV_R_EXPLICIT ? 3 : p->rating == LV_R_QUESTIONABLE ? 2 : p->rating == LV_R_SENSITIVE ? 1 : 0;
  sw_nsfw = RATING_NSFW[ri]; // (from its rating, each time: a different post, a different level)
  sw_catsel = 0;
  for (int k = 0; k < vault.ncats; k++) if (sw_cat[0] && !strcmp(vault.cats[k], sw_cat)) sw_catsel = k + 1;
  if (sw_catsel == 0) sw_cat[0] = 0;
  sw_step = 0; sw_on = 1; sw_anim = 0;
  if (wv_vid && !lv_video_paused(wv_vid)) lv_video_pause(wv_vid, 1);
  sfx(SFX_OPEN);
}
static void sw_do_save(void) {
  sw_on = 0;
  wv_save();
}
static int big_btn(int x, int y, int w, int h, const char *label, unsigned bg, unsigned fg) {
  rrect(x, y, w, h, h / 2, bg);
  text_center(x + w / 2, y + h / 2 + 7, fg, 19, label);
  return hit(x, y - 4, w, h + 8);
}
static void draw_save_wizard(void) {
  const LvPost *p = &posts[wv];
  // (the background: the frame is cleared to it already)
  sw_anim += (1 - sw_anim) * 0.3f;
  // the picture, on the left
  vita2d_texture *t = wv_tex_post == wv && wv_tex ? wv_tex : preview_of(wv);
  const int PW = 360;
  rrect(0, 0, PW, H, 0, RGBA(0, 0, 0, 255));
  if (t) {
    float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t), sc = fminf((PW - 24) / tw, (H - 24) / th);
    vita2d_draw_texture_scale(t, PW / 2 - tw * sc / 2, H / 2 - th * sc / 2, sc, sc);
  } else spinner(PW / 2, H / 2, 20);
  // the step's place: four dots, and its name
  int x0 = PW + 36, xr = W - 30;
  for (int k = 0; k < 4; k++) {
    int on = k == sw_step;
    rrect(x0 + k * 56, 26, 46, 8, 4, on ? ACC : k < sw_step ? with_alpha(ACC, 120) : SURF2);
    if (hit(x0 + k * 56, 10, 46, 40) && k < sw_step) { sw_step = k; sfx(SFX_TICK); }
  }
  char st[64];
  snprintf(st, sizeof st, "Step %d of 4", sw_step + 1);
  text(x0, 64, FAINT, 14, st);
  // save now, top right (steps before the overview)
  if (sw_step < 3) {
    int bw = text_w(16, "Save now") + 40;
    rrect(xr - bw, 14, bw, 40, 20, SURF2);
    text(xr - bw + 20, 40, TEXT, 16, "Save now");
    if (hit(xr - bw, 10, bw, 48) || (in.pressed & SCE_CTRL_TRIANGLE)) { sw_do_save(); in.pressed = 0; in.tap = 0; return; }
  }
  int y = 110;
  int dx = (int)((1 - sw_anim) * 40); // (each step slides in a little)
  x0 += dx;
  switch (sw_step) {
    case 0: { // the level
      text(x0, y, TEXT, 28, "How explicit is it?");
      text(x0, y + 30, DIM, 15, "0 is safe for anyone, 10 is as explicit as it gets");
      const int bw = 72, bh = 64;
      for (int k = 0; k <= 10; k++) {
        int bx = x0 + (k % 6) * (bw + 12), by = y + 60 + (k / 6) * (bh + 14);
        int on = k == sw_nsfw;
        rrect(bx, by, bw, bh, 18, on ? ACC : SURF2);
        char n[4];
        snprintf(n, sizeof n, "%d", k);
        text_center(bx + bw / 2, by + bh / 2 + 10, on ? RGBA(20, 12, 16, 255) : TEXT, 28, n);
        if (hit(bx, by, bw, bh)) { sw_nsfw = k; sfx(SFX_TICK); }
      }
      static const char *words[] = {"Safe", "Safe", "Mild", "Mild", "Suggestive", "Suggestive", "Lewd", "Lewd", "Explicit", "Explicit", "Extreme"};
      char m[80];
      snprintf(m, sizeof m, "%s  ·  the site rates it %s", words[sw_nsfw < 0 ? 0 : sw_nsfw > 10 ? 10 : sw_nsfw],
               RATING_NAMES[p->rating == LV_R_EXPLICIT ? 3 : p->rating == LV_R_QUESTIONABLE ? 2 : p->rating == LV_R_SENSITIVE ? 1 : 0]);
      text(x0, y + 230, DIM, 16, m);
      if (in.pressed & SCE_CTRL_RIGHT) { if (sw_nsfw < 10) sw_nsfw++; sfx(SFX_TICK); }
      if (in.pressed & SCE_CTRL_LEFT) { if (sw_nsfw > 0) sw_nsfw--; sfx(SFX_TICK); }
      if (in.pressed & SCE_CTRL_DOWN) { sw_nsfw = sw_nsfw + 6 > 10 ? 10 : sw_nsfw + 6; sfx(SFX_TICK); }
      if (in.pressed & SCE_CTRL_UP) { sw_nsfw = sw_nsfw - 6 < 0 ? 0 : sw_nsfw - 6; sfx(SFX_TICK); }
      break;
    }
    case 1: { // the category
      text(x0, y, TEXT, 28, "Which category?");
      text(x0, y + 30, DIM, 15, "Where it shows in the Library");
      int n = sw_ncats(), cx = x0, cy = y + 56;
      if (sw_catsel >= n) sw_catsel = n - 1;
      for (int k = 0; k < n; k++) {
        const char *nm = sw_cat_name(k);
        if (k > 0 && k < n - 1 && !strcmp(nm, save_category())) continue; // (the usual one is first already)
        char lb[80];
        snprintf(lb, sizeof lb, "%s%s", nm, k == 0 ? "  (usual)" : "");
        int w = text_w(17, lb) + 44;
        if (w > xr - x0) w = xr - x0;
        if (cx + w > xr) { cx = x0; cy += 58; }
        if (cy > H - 140) break;
        int on = k == sw_catsel;
        rrect(cx, cy, w, 48, 24, on ? ACC : k == n - 1 ? with_alpha(SURF2, 160) : SURF2);
        text_fit(cx + 22, cy + 31, on ? RGBA(20, 12, 16, 255) : TEXT, 17, lb, w - 34);
        if (hit(cx, cy, w, 48)) { sw_catsel = k; sfx(SFX_TICK); if (k == n - 1 && !ime_open) ime_start(16, "The new category's name", "", 0); }
        cx += w + 10;
      }
      if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_LEFT)) {
        int d = (in.pressed & SCE_CTRL_RIGHT) ? 1 : n - 1;
        sw_catsel = (sw_catsel + d) % n;
        if (sw_catsel > 0 && sw_catsel < n - 1 && !strcmp(sw_cat_name(sw_catsel), save_category())) sw_catsel = (sw_catsel + d) % n;
        sfx(SFX_TICK);
      }
      if (sw_catsel == n - 1 && (in.pressed & SCE_CTRL_CROSS) && !ime_open) { ime_start(16, "The new category's name", "", 0); in.pressed &= ~SCE_CTRL_CROSS; }
      if (sw_catsel > 0 && sw_catsel < n - 1) snprintf(sw_cat, sizeof sw_cat, "%s", vault.cats[sw_catsel - 1]);
      else if (sw_catsel == 0) sw_cat[0] = 0;
      break;
    }
    case 2: { // stars and tags
      text(x0, y, TEXT, 28, "How good is it?");
      for (int k = 1; k <= 5; k++) {
        int bx = x0 + (k - 1) * 84, by = y + 44;
        unsigned c = k <= sw_stars ? ACC : SURF2;
        rrect(bx, by, 72, 72, 20, k <= sw_stars ? with_alpha(ACC, 50) : SURF2);
        text_center(bx + 36, by + 50, c, 36, "★");
        if (hit(bx, by, 72, 72)) { sw_stars = k; sfx(SFX_TICK); }
      }
      if (in.pressed & SCE_CTRL_RIGHT) { if (sw_stars < 5) sw_stars++; sfx(SFX_TICK); }
      if (in.pressed & SCE_CTRL_LEFT) { if (sw_stars > 1) sw_stars--; sfx(SFX_TICK); }
      // the site's tags: kept or not
      int ty = y + 150;
      int ntags = 0;
      for (const char *c = p->tags; *c; c++) if (*c != ' ' && (c == p->tags || c[-1] == ' ')) ntags++;
      rrect(x0, ty, xr - x0, 56, 18, SURF2);
      text(x0 + 20, ty + 35, TEXT, 18, "Keep the site's tags");
      char tn[32];
      snprintf(tn, sizeof tn, "%d tags", ntags);
      text(x0 + 220, ty + 35, FAINT, 14, tn);
      int sw_x = xr - 80;
      rrect(sw_x, ty + 12, 60, 32, 16, sw_notags ? SURF : ACC);
      circle(sw_notags ? sw_x + 16 : sw_x + 44, ty + 28, 12, TEXT);
      if (hit(x0, ty, xr - x0, 56) || (in.pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN))) { sw_notags = !sw_notags; sfx(SFX_TOGGLE); }
      if (!sw_notags) {
        char tl[260];
        snprintf(tl, sizeof tl, "%.250s", p->tags);
        for (char *c = tl; *c; c++) if (*c == '_') *c = ' ';
        text_fit(x0 + 4, ty + 84, FAINT, 13, tl, xr - x0 - 8);
      }
      break;
    }
    default: { // the overview
      text(x0, y, TEXT, 28, "Ready to save");
      const char *labels[3] = {"Level", "Category", "Stars and tags"};
      char vals[3][120];
      snprintf(vals[0], sizeof vals[0], "%d of 10", sw_nsfw);
      snprintf(vals[1], sizeof vals[1], "%s", sw_cat[0] ? sw_cat : save_category());
      snprintf(vals[2], sizeof vals[2], "%d ★  ·  %s", sw_stars, sw_notags ? "no tags" : "the site's tags");
      for (int k = 0; k < 3; k++) {
        int ry = y + 36 + k * 64;
        rrect(x0, ry, xr - x0, 56, 18, SURF2);
        text(x0 + 20, ry + 35, DIM, 16, labels[k]);
        text_fit(x0 + 190, ry + 35, TEXT, 18, vals[k], xr - x0 - 260);
        icon("chev_r", xr - 26, ry + 28, FAINT);
        if (hit(x0, ry, xr - x0, 56)) { sw_step = k; sw_anim = 0; sfx(SFX_TICK); }
      }
      char sz[80];
      snprintf(sz, sizeof sz, "%s  ·  %s", lv_post_is_video(p) ? "Video, as it is" : cfg.wfull ? "Full size" : "Vita size (Filter → Saves)", LV_SITES[cfg.site].name);
      text(x0, y + 250, FAINT, 14, sz);
      break;
    }
  }
  x0 -= dx;
  // the foot: back and next (or save)
  int fy = H - 76;
  int bk = big_btn(x0, fy, 150, 56, sw_step ? "Back" : "Cancel", SURF2, TEXT);
  int nx = big_btn(xr - 200, fy, 200, 56, sw_step == 3 ? "Save" : "Next", ACC, RGBA(20, 12, 16, 255));
  text(x0, fy - 14, FAINT, 13, sw_step == 3 ? "✕ save   ○ back" : "←→ change   ✕ next   ○ back   △ save now");
  if (bk || (in.pressed & SCE_CTRL_CIRCLE)) { sfx(SFX_BACK); if (sw_step) { sw_step--; sw_anim = 0; } else sw_on = 0; }
  else if (nx || (in.pressed & SCE_CTRL_CROSS)) {
    if (sw_step == 3) { sfx(SFX_SELECT); sw_do_save(); }
    else { sfx(SFX_TICK); sw_step++; sw_anim = 0; }
  }
  in.pressed = 0; in.tap = 0;
}
// The full view's buttons at the top right: a long video (an episode) gets ♥ Favorite (its link
// kept) and ↓ Offline (downloaded into the vault); anything else, Save.
static int wv_site(void) { return wv < nposts && posts[wv].site1 > 0 && posts[wv].site1 <= LV_NSITES ? posts[wv].site1 - 1 : cfg.site; }
static int wv_is_long(void) { return wv < nposts && lv_is_long(&LV_SITES[wv_site()], &posts[wv]); }
static void wv_labels(const char **l1, const char **l2) {
  const LvPost *p = &posts[wv];
  int saved = is_saved(p);
  if (!wv_is_long()) { *l1 = NULL; *l2 = saved ? "\u2713 Saved" : saving ? "Saving\u2026" : "\u2193 Save"; return; }
  *l1 = fav_find(0, p->page) >= 0 ? "\u2665 Favorite" : "\u2661 Favorite";
  *l2 = saved ? "\u2713 Offline" : saving ? "Saving\u2026" : fav_offline_next == wv + 1 ? "Offline when here" : "\u2193 Offline";
}
static void wv_btns(int *x1, int *w1, int *x2, int *w2) {
  const char *l1, *l2;
  wv_labels(&l1, &l2);
  *w2 = text_w(15, l2) + 24;
  *x2 = W - 72 - *w2;
  *w1 = l1 ? text_w(15, l1) + 24 : 0;
  *x1 = *x2 - 8 - *w1;
}
// ---------- an offline copy made 480p by a conversion service ----------
// (the Vita can't re-encode video: the original's address goes to one of your services, which makes an
//  854 x 480 H.264 MP4; the Vita downloads that into the vault. Each service with a key is tried in turn.)
typedef struct { char src[1500], page[600], preview[600], name[200], cat[64], tags[4000]; int w, h, nsfw, vgen; } ConvJob;
typedef struct { LvNew nw; LvAdded added; char name[200], cat[64], when[40], page[600], tags[4000]; int vgen; } ConvDone;
static volatile int conv_on;
static char conv_msg[200];
static volatile unsigned conv_msg_seq;
static ConvDone conv_done;
static volatile int conv_done_ready;
static void conv_say(const char *m) { snprintf(conv_msg, sizeof conv_msg, "%s", m); conv_msg_seq++; }
static void *conv_main(void *arg) {
  ConvJob *j = arg;
  pin_to_core(1);
  LvHttp *h = lv_http_new();
  char result[1500] = "";
  for (int s = 0; s < CV_N && h && !result[0]; s++) {
    if (!conv_keys[s][0]) continue;
    char url[300], hdr[3][300], id[200], m[200];
    static char body[5000];
    lv_conv_start(s, conv_keys[s], j->src, url, sizeof url, body, sizeof body, hdr);
    const char *hs[3] = {hdr[0], hdr[1], NULL};
    LvReq rq = {.method = "POST", .headers = hs, .body = body, .body_len = strlen(body)};
    uint8_t *b = NULL;
    size_t n = 0;
    const char *err = NULL;
    snprintf(m, sizeof m, "Making it 480p on %s\u2026", lv_conv_name(s));
    conv_say(m);
    int st = lv_http_req(h, url, &rq, &b, &n, 1u << 20, NULL, &err);
    int ok = (st == 200 || st == 201) && b && lv_conv_parse_start(s, (char *)b, n, id, sizeof id) == 0;
    free(b);
    if (!ok) { snprintf(m, sizeof m, "%s didn't take it (%d): trying the next", lv_conv_name(s), st); conv_say(m); continue; }
    // (asked every five seconds, for forty minutes at most)
    const char *hg[2] = {hdr[0], NULL};
    LvReq get = {.headers = hg};
    char purl[300];
    lv_conv_poll_url(s, id, purl, sizeof purl);
    int r = 0;
    for (int t = 0; t < 480 && r == 0 && worker_run; t++) {
      sceKernelDelayThread(5 * 1000 * 1000);
      b = NULL; n = 0;
      st = lv_http_req(h, purl, &get, &b, &n, 4u << 20, NULL, &err);
      if (st == 200 && b) r = lv_conv_parse_poll(s, (char *)b, n, result, sizeof result);
      else if (st == 401 || st == 403 || st == 404) r = -1;
      free(b);
    }
    if (r != 1) { result[0] = 0; snprintf(m, sizeof m, "%s couldn't make it: trying the next", lv_conv_name(s)); conv_say(m); }
  }
  lv_http_free(h);
  if (!result[0]) { conv_say("No conversion service could make it 480p (Settings \u203a Video conversion)"); free(j); conv_on = 0; return NULL; }
  // the 480p copy: down onto the card (a new connection: the result's address needs no key)
  conv_say("Downloading the 480p copy\u2026");
  LvHttp *d = lv_http_new();
  size_t n = 0;
  const char *err = NULL;
  char path[] = DATA "/conv480.mp4";
  long long room = free_room("ux0:");
  size_t cap = room > 0 && room - 96ll * 1024 * 1024 > 0 ? (size_t)(room - 96ll * 1024 * 1024 < 1536ll * 1024 * 1024 ? room - 96ll * 1024 * 1024 : 1536ll * 1024 * 1024) : 400u * 1024 * 1024;
  int st = d ? lv_http_get_file(d, result, path, &n, cap, NULL, NULL, &err) : -1;
  // its thumbnail: the post's preview, at a card's size
  uint8_t *thumb = NULL, *pb = NULL;
  size_t tn = 0, pn = 0;
  int tw = 0, th = 0;
  if (st == 200 && d && lv_http_get(d, j->preview, &pb, &pn, 4u << 20, NULL, NULL, NULL) == 200) {
    LvPixels px;
    int cw, ch;
    cover_size(pb, pn, &cw, &ch);
    if (lv_decode(pb, pn, cw, ch, &px) == 0) { thumb = lv_encode_jpeg(&px, 85, &tn); tw = px.w; th = px.h; free(px.rgba); }
  }
  free(pb);
  lv_http_free(d);
  if (st != 200) { conv_say(err && err[0] ? err : "The 480p copy didn't download"); free(thumb); free(j); conv_on = 0; return NULL; }
  // its size: the original's, fitted in 854 x 480 (even numbers, as the service makes them)
  double sc = j->w > 0 && j->h > 0 ? fmin(1.0, fmin(854.0 / j->w, 480.0 / j->h)) : 1.0;
  ConvDone c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "%s", j->name);
  snprintf(c.cat, sizeof c.cat, "%s", j->cat);
  snprintf(c.page, sizeof c.page, "%s", j->page);
  snprintf(c.tags, sizeof c.tags, "%s", j->tags);
  now_iso(c.when, sizeof c.when);
  LvNew nw = {.name = c.name, .category = c.cat, .kind = "video", .tags = c.tags, .source = c.page, .added = c.when, .nsfw = j->nsfw,
              .quality = 5, .width = ((int)(j->w * sc)) & ~1, .height = ((int)(j->h * sc)) & ~1, .from = path};
  saver_flush();
  pthread_mutex_lock(&vmx);
  const char *why = NULL;
  int ok = loaded_vault && vgen == j->vgen && lv_vault_add_files(&vault, &nw, NULL, n, "mp4", thumb, tn, tw, th, &c.added, &why) == 0;
  pthread_mutex_unlock(&vmx);
  free(thumb);
  remove(path);
  if (!ok) { conv_say(why ? why : "It couldn't be saved"); free(j); conv_on = 0; return NULL; }
  c.nw = nw; c.vgen = j->vgen;
  conv_done = c;
  conv_done_ready = 1;
  free(j);
  conv_on = 0;
  return NULL;
}
// (the drawing thread: its messages as they come, and the saved copy into the loaded vault)
static void conv_take(void) {
  static unsigned seen;
  if (seen != conv_msg_seq) { seen = conv_msg_seq; say(conv_msg); }
  if (!conv_done_ready) return;
  ConvDone *c = &conv_done;
  if (c->vgen == vgen && loaded_vault) {
    c->nw.name = c->name; c->nw.category = c->cat; c->nw.added = c->when; c->nw.tags = c->tags; c->nw.source = c->page;
    int before = vault.n;
    pthread_mutex_lock(&vmx);
    int ok = lv_vault_add_memory(&vault, &c->nw, &c->added) == 0;
    pthread_mutex_unlock(&vmx);
    if (ok) library_grew(before);
    saved_add(c->page);
    sfx(SFX_SAVED);
    say("Saved at 480p: available offline");
  }
  conv_done_ready = 0;
}
static void conv_start(const LvPost *p, int site) {
  if (conv_on) { say("Another video is being made 480p: one at a time"); return; }
  ConvJob *j = calloc(1, sizeof *j);
  if (!j) return;
  snprintf(j->src, sizeof j->src, "%s", p->file);
  snprintf(j->page, sizeof j->page, "%s", p->page);
  snprintf(j->preview, sizeof j->preview, "%s", p->preview);
  snprintf(j->name, sizeof j->name, "%s %lld (480p).mp4", LV_SITES[site].name, p->id);
  snprintf(j->cat, sizeof j->cat, "%s", save_category());
  snprintf(j->tags, sizeof j->tags, "%s", p->tags);
  int ri = p->rating == LV_R_EXPLICIT ? 3 : p->rating == LV_R_QUESTIONABLE ? 2 : p->rating == LV_R_SENSITIVE ? 1 : 0;
  j->nsfw = RATING_NSFW[ri];
  j->w = p->width; j->h = p->height; j->vgen = vgen;
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024); // (it decodes the thumbnail)
  conv_on = 1;
  if (pthread_create(&t, &at, conv_main, j) != 0) { conv_on = 0; free(j); say("It couldn't start"); return; }
  pthread_detach(t);
  sfx(SFX_OPEN);
}

static void wv_offline(void) {
  if (wv >= nposts) return;
  if (is_saved(&posts[wv])) { say("It's in your vault already"); return; }
  // (offline copies are 480p: the episode sites give that file; a booru keeps only the original, and
  //  says its size: bigger than 480p, it isn't made available offline, it stays a favourite)
  {
    const LvPost *p = &posts[wv];
    int w = p->width, h = p->height, small_side = w < h ? w : h;
    if (lv_site_is_booru(&LV_SITES[wv_site()]) && small_side > 480) {
      // (made 480p by one of your conversion services; with none, it stays a favourite)
      if (conv_any_key()) { conv_start(p, wv_site()); return; }
      sfx(SFX_ERROR);
      char m[200];
      snprintf(m, sizeof m, "Only at %dx%d here: keep it as a favourite (or turn on Settings \u203a Video conversion)", w, h);
      say(m);
      return;
    }
  }
  if (wv_raw_post == wv) { sw_open(); return; }
  fav_offline_next = wv + 1;
  sfx(SFX_TOGGLE);
  say("It goes into the vault once it's all downloaded (it plays meanwhile)");
}
// A seek in the video, never past what has downloaded yet (less a margin): the player would wait there.
static void wv_seek(int64_t ms) {
  if (!wv_vid) return;
  uint64_t d = lv_video_duration(wv_vid);
  if (ms < 0) ms = 0;
  if (wv_stream && !wv_stream->done && wv_stream->total > 0 && d > 0) {
    double f = (double)wv_stream->have / (double)wv_stream->total - 0.03;
    int64_t lim = (int64_t)(d * (f > 0 ? f : 0));
    if (ms > lim) { ms = lim; say("Only as far as it has downloaded"); }
  }
  lv_video_seek(wv_vid, ms);
}
static HeroClose whero; // (the Browser's full view, swiped down to close)
static int web_card_target(int k, Rect *o, float *sc_out) {
  if (wv_back != S_WEB || k < 0 || k >= wgrid.n) return 0;
  Rect r = wgrid.r[k];
  float sc = wscroll;
  if (r.y < sc || r.y + r.h > sc + H - TOP - BAR) sc = r.y - 40 > 0 ? r.y - 40 : 0;
  o->x = r.x; o->y = TOP + 6 + r.y - sc; o->w = r.w; o->h = r.h;
  if (sc_out) *sc_out = sc;
  return 1;
}
// A post's name for people: its tags, the underscores as spaces, the first letter a capital
static void post_title(const LvPost *p, char *o, size_t n) {
  snprintf(o, n, "%s", p->tags ? p->tags : "");
  for (char *c = o; *c; c++) if (*c == '_') *c = ' ';
  if (o[0] >= 'a' && o[0] <= 'z') o[0] -= 32;
}
// An episode (a long video, or one from an episode site): its card, at the top left while it loads
// or is paused: the title, its length and where it's from, and ♥ Favorite / ↓ Offline as big pills
static void wv_offline(void);
static int wv_episode_card(const LvPost *p) {
  const float x = 30, y = 74, w = 430, h = 150;
  rrect(x, y, w, h, 18, RGBA(0, 0, 0, 170));
  rrect(x, y, 5, h, 3, ACC);
  // (its name split into two lines once per post, kept: not measured word by word every frame)
  static char t[400], l1[400];
  static const char *l2;
  static const LvPost *made_for;
  static const char *made_page;
  if (made_for != p || made_page != p->page) {
  made_for = p; made_page = p->page;
  post_title(p, t, sizeof t);
  // (two lines at most)
  int cut = (int)strlen(t);
  while (cut > 0 && text_w(19, t) > (w - 40) * 2 - 30) { cut = cut * 9 / 10; t[cut] = 0; }
  l2 = NULL;
  snprintf(l1, sizeof l1, "%s", t);
  if (text_w(19, l1) > w - 40) {
    // (the first line: up to the last space where it still fits; the rest on the second)
    int cut = -1;
    for (int k = 1; t[k]; k++) {
      if (t[k] != ' ') continue;
      snprintf(l1, sizeof l1, "%.*s", k, t);
      if (text_w(19, l1) > w - 40) break;
      cut = k;
    }
    if (cut > 0) { snprintf(l1, sizeof l1, "%.*s", cut, t); l2 = t + cut + 1; }
    else snprintf(l1, sizeof l1, "%s", t);
  }
  }
  text_fit((int)x + 20, (int)y + 32, TEXT, 19, l1, (int)w - 40);
  if (l2) text_fit((int)x + 20, (int)y + 56, TEXT, 19, l2, (int)w - 40);
  char meta[120], len[24] = "";
  if (p->duration > 0) { int d = p->duration; snprintf(len, sizeof len, d >= 3600 ? "%d:%02d:%02d" : "%d:%02d", d >= 3600 ? d / 3600 : d / 60, d >= 3600 ? d / 60 % 60 : d % 60, d % 60); }
  snprintf(meta, sizeof meta, "%s%s%s", len, len[0] ? "  \u00b7  " : "", LV_SITES[wv_site()].name);
  text((int)x + 20, (int)y + (l2 ? 80 : 58), DIM, 14, meta);
  // the pills
  int fav = fav_find(0, p->page) >= 0, saved = is_saved(p); // (both remembered, not searched each frame)
  const char *f1 = fav ? "\u2665 Favorite" : "\u2661 Favorite";
  const char *f2 = saved ? "\u2713 Offline" : saving ? "Saving\u2026" : fav_offline_next == wv + 1 ? "Offline when here" : "\u2193 Offline";
  float py = y + h - 54, pw1 = text_w(17, f1) + 40, pw2 = text_w(17, f2) + 40;
  rrect(x + 20, py, pw1, 40, 20, fav ? ACC : SURF2);
  text_center((int)(x + 20 + pw1 / 2), (int)py + 27, fav ? RGBA(20, 12, 16, 255) : TEXT, 17, f1);
  rrect(x + 30 + pw1, py, pw2, 40, 20, saved ? SURF2 : with_alpha(TEXT, 230));
  text_center((int)(x + 30 + pw1 + pw2 / 2), (int)py + 27, saved ? DIM : RGBA(20, 12, 16, 255), 17, f2);
  if (in.tap && in.tap_y >= py && in.tap_y < py + 40) {
    if (in.tap_x >= x + 20 && in.tap_x < x + 20 + pw1) { fav_toggle_video(&posts[wv], wv_site()); return 1; }
    if (in.tap_x >= x + 30 + pw1 && in.tap_x < x + 30 + pw1 + pw2) { wv_offline(); return 1; }
  }
  return in.tap && in.tap_x >= x && in.tap_x < x + w && in.tap_y >= y && in.tap_y < y + h; // (a tap on the card: its own)
}
// "Similar to this": a search for what this post is most about (similar_tags, main.c), on its site
static int wv_similar(const LvPost *p) {
  char q[160];
  similar_tags(p->tags, NULL, q, sizeof q);
  if (!q[0]) { sfx(SFX_ERROR); say("No tags here to go by"); return 0; }
  sfx(SFX_SELECT);
  snprintf(cfg.wquery, sizeof cfg.wquery, "%s", q);
  wv_drop();
  wdetails = 0;
  screen = S_WEB;
  web_search();
  char m[200];
  snprintf(m, sizeof m, "Like this: %s", q);
  say(m);
  return 1;
}
static void draw_webview(void) {
  if (!nposts) { screen = wv_back; return; }
  { static int last_wv = -1; if (wv != last_wv) { last_wv = wv; if (!whero.closing) memset(&whero, 0, sizeof whero); } } // (a new one: not mid-swipe)
  if (sw_on && wv < nposts) { draw_save_wizard(); return; }
  // a video downloading: played as soon as its first part is here (MP4: the system player)
  static int stream_tried = -1, stream_tried_gen; // (once a video: a player that won't open isn't asked every frame)
  if (!wv_vid && dl_post == wv && dl_gen == wgen && wv < nposts && lv_post_is_video(&posts[wv]) && !strcmp(posts[wv].ext, "mp4") && !(stream_tried == wv && stream_tried_gen == wgen)) {
    LvGrow *g = &dl_grow[dl_slot];
    // (a head start first: 2 MB, or 2% of the file, whichever is more)
    int64_t need = g->total > 0 && g->total / 50 > 2 * 1024 * 1024 ? g->total / 50 : 2 * 1024 * 1024;
    if (g->have >= need || (g->total > 0 && g->have >= g->total)) {
      stream_tried = wv; stream_tried_gen = wgen;
      wv_vid = lv_video_open_growing(dl_path[dl_slot], g);
      LOG("stream open %s have=%lld total=%lld -> %p\n", dl_path[dl_slot], (long long)g->have, (long long)g->total, (void *)wv_vid);
      if (wv_vid) { lv_video_set_volume(wv_vid, cfg.volume * 32768 / 100); wv_loading = 0; wv_stream = g; }
    }
  }
  LvPost *p = &posts[wv];
  int backdrop = hero_backdrop(&whero);
  if (backdrop < 255) vita2d_draw_rectangle(0, 0, W, H, BG);
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, backdrop));
  vita2d_texture *t = NULL;
  int is_frame = 0;
  if (wv_vid) { t = lv_video_frame(wv_vid); is_frame = t != NULL; }
  if (!t && wv_tex_post == wv) t = wv_tex;
  if (!t) t = preview_of(wv);
  float avail_h = H - wdetails_y * 300;
  if (t && is_frame && (lv_video_width(wv_vid) < 1 || lv_video_height(wv_vid) < 1)) t = preview_of(wv), is_frame = 0; // (no size yet)
  if (t && (vita2d_texture_get_width(t) < 1 || vita2d_texture_get_height(t) < 1)) t = NULL;
  if (t) {
    float tw = is_frame ? lv_video_width(wv_vid) : vita2d_texture_get_width(t);
    float th = is_frame ? lv_video_height(wv_vid) : vita2d_texture_get_height(t);
    float s = fminf(W / tw, avail_h / th) * wzoom;
    float dw = tw * s, dh = th * s;
    float mx = fmaxf(0, (dw - W) / 2), my = fmaxf(0, (dh - avail_h) / 2);
    wpan_x = fminf(mx, fmaxf(-mx, wpan_x));
    wpan_y = fminf(my, fmaxf(-my, wpan_y));
    float x = (W - dw) / 2 + wpan_x + wswipe, y = (avail_h - dh) / 2 + wpan_y;
    if (whero.drag > 0 || whero.closing) {
      Rect card;
      int has = web_card_target(wv, &card, NULL);
      float e = hero_place(&whero, &x, &y, &dw, &dh, card, has);
      draw_in_rect(t, tw, th, x, y, dw, dh, e, RGBA(255, 255, 255, 255));
    } else {
      hero_place(&whero, &x, &y, &dw, &dh, (Rect){0}, 0);
      vita2d_draw_texture_scale(t, x, y, s, s);
    }
  }
  if (whero.closing || (!t && whero.drag > 0)) {
    if (hero_done(&whero) || !t) {
      float sc;
      Rect card;
      if (web_card_target(wv, &card, &sc)) wscroll = sc;
      memset(&whero, 0, sizeof whero);
      wv_drop(); wsel = wv; screen = wv_back;
    }
    return;
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
      else if (hero_drag(&whero, fabsf(wswipe) < 4 && wdetails_y < 0.05f)) {}
      else if (fabsf((float)(in.tx - in.down_x)) > fabsf((float)(in.ty - in.down_y))) wswipe += in.drag_dx;
      else if (in.ty - in.down_y < -60) wdetails = 1;
    }
  } else {
    pd0 = 0;
    if (hero_release(&whero)) { sfx(SFX_BACK); return; }
    if (wswipe > 120) { wswipe = 0; sfx(SFX_TICK); wv_show(wv - 1); return; }
    if (wswipe < -120) { wswipe = 0; sfx(SFX_TICK); wv_show(wv + 1); return; }
    wswipe *= 0.7f;
    if (wzoom < 1.01f) wzoom = 1;
  }
  int64_t vseek = INT64_MIN;
  int vchrome = frame_no < wchrome_until || (wv_vid && lv_video_paused(wv_vid));
  // (an episode loading or paused: its card, whose taps are its own)
  int card_tap = wdetails_y < 0.05f && whero.drag <= 0 && wv_is_long() && (!wv_vid || lv_video_paused(wv_vid)) && !sw_on && wv_episode_card(p);
  if (card_tap) {}
  else if (in.dbl && wv_vid && wdetails_y < 0.5f && vctl_dbl_side(in.tap_x)) {
    int sd = vctl_dbl_side(in.tap_x);
    vseek = (int64_t)lv_video_pos(wv_vid) + sd * 10000;
    vflash(sd);
  }
  else if (in.dbl) { wzoom = wzoom > 1.5f ? 1 : 2.5f; wpan_x = (W / 2.0f - in.tap_x) * (wzoom - 1); wpan_y = (H / 2.0f - in.tap_y) * (wzoom - 1); }
  else if (in.tap && wdetails_y < 0.5f) {
    // (a tap on the video's bar: there)
    if (wv_vid && vchrome && in.tap_y > 70 && vctl_tap(wv_vid, in.tap_x, in.tap_y, &vseek)) wchrome_until = frame_no + 180;
    else if (frame_no < wchrome_until && in.tap_x < 70 && in.tap_y < 60) { sfx(SFX_BACK); wv_drop(); screen = wv_back; return; }
    else if (frame_no < wchrome_until && in.tap_x > W - 70 && in.tap_y < 60) wdetails = 1;
    else if (frame_no < wchrome_until && in.tap_y < 60 && in.tap_x < W - 64 && in.tap_x > W - 420) {
      int x1, w1, x2, w2;
      wv_btns(&x1, &w1, &x2, &w2);
      if (in.tap_x >= x2 - 4) { if (wv_is_long()) wv_offline(); else sw_open(); }
      else if (w1 && in.tap_x >= x1 - 4) fav_toggle_video(&posts[wv], wv_site());
    }
    else wchrome_until = frame_no < wchrome_until ? frame_no : frame_no + 240;
  }

  // --- buttons ---
  if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER)) { sfx(SFX_TICK); wv_show(wv + 1); return; }
  if (in.pressed & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER)) { sfx(SFX_TICK); wv_show(wv - 1); return; }
  if (in.pressed & SCE_CTRL_CIRCLE) { if (wdetails) { sfx(SFX_CLOSE); wdetails = 0; } else { sfx(SFX_BACK); wv_drop(); wsel = wv; screen = wv_back; return; } }
  if (in.pressed & SCE_CTRL_TRIANGLE) { sfx(wdetails ? SFX_CLOSE : SFX_OPEN); wdetails = !wdetails; }
  // □: Save (an episode: ♥ Favorite); START: an episode's Offline
  if (in.pressed & SCE_CTRL_SQUARE) { if (wv_is_long()) fav_toggle_video(&posts[wv], wv_site()); else sw_open(); wchrome_until = frame_no + 120; }
  if ((in.pressed & SCE_CTRL_START) && wv_is_long()) { wv_offline(); wchrome_until = frame_no + 120; }
  if ((in.pressed & SCE_CTRL_CROSS) && wdetails) { if (wv_similar(p)) return; }
  else if (in.pressed & SCE_CTRL_CROSS) {
    if (wv_vid) lv_video_pause(wv_vid, !lv_video_paused(wv_vid));
    else { wzoom = wzoom > 1.5f ? 1 : 2.5f; wpan_x = wpan_y = 0; }
  }
  if (wzoom > 1.01f) { wpan_x -= in.lx * 12; wpan_y -= in.ly * 12; }
  // (a video: the left stick scrubs, further for a harder push; ↑ / ↓ ten seconds back or on)
  else if (wv_vid && lv_video_duration(wv_vid) > 0) {
    if (fabsf(in.lx) > 0.4f && frame_no % 6 == 0) { wv_seek((int64_t)lv_video_pos(wv_vid) + (int64_t)(in.lx * 8000)); wchrome_until = frame_no + 120; }
    if (in.pressed & SCE_CTRL_UP) { vseek = (int64_t)lv_video_pos(wv_vid) + 10000; vflash(1); wchrome_until = frame_no + 120; }
    if (in.pressed & SCE_CTRL_DOWN) { vseek = (int64_t)lv_video_pos(wv_vid) - 10000; vflash(-1); wchrome_until = frame_no + 120; }
  }
  if (vseek != INT64_MIN && wv_vid) { wv_seek(vseek < 0 ? 0 : vseek); wchrome_until = frame_no + 150; }

  // --- the controls on top ---
  if ((frame_no < wchrome_until || wdetails) && whero.drag <= 0) {
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
    {
      const char *l1, *l2;
      int x1, w1, x2, w2;
      wv_labels(&l1, &l2);
      wv_btns(&x1, &w1, &x2, &w2);
      rrect(x2, 16, w2, 28, 14, saved ? with_alpha(SURF2, a) : with_alpha(ACC, a));
      text(x2 + 12, 35, saved ? with_alpha(TEXT, a) : RGBA(20, 12, 16, a), 15, l2);
      if (l1) {
        int fav = fav_find(0, p->page) >= 0;
        rrect(x1, 16, w1, 28, 14, fav ? with_alpha(ACC, a) : with_alpha(SURF2, a));
        text(x1 + 12, 35, fav ? RGBA(20, 12, 16, a) : with_alpha(TEXT, a), 15, l1);
      }
    }
    circle(W - 38, 30, 13, with_alpha(TEXT, a));
    circle(W - 38, 30, 11, RGBA(0, 0, 0, a));
    text_center(W - 38, 36, with_alpha(TEXT, a), 15, "i");
    (void)0;
  }
  // a video playing as it downloads: paused when it catches up with the download, until there's a
  // margin again ("Buffering"), so the player never reads past what has come
  static int auto_paused;
  static const LvVideo *paused_vid;
  if (paused_vid != wv_vid) { auto_paused = 0; paused_vid = wv_vid; } // (another video: its own state)
  if (auto_paused && wv_vid && (!wv_stream || wv_stream->done)) { lv_video_pause(wv_vid, 0); auto_paused = 0; } // (all here now)
  if (wv_vid && wv_stream && !wv_stream->done && wv_stream->total > 0 && lv_video_duration(wv_vid) > 0) {
    float have = (float)wv_stream->have / (float)wv_stream->total, at = (float)lv_video_pos(wv_vid) / (float)lv_video_duration(wv_vid);
    if (!lv_video_paused(wv_vid) && have < at + 0.015f && !wv_stream->failed) { lv_video_pause(wv_vid, 1); auto_paused = 1; }
    else if (auto_paused && (have > at + 0.05f || wv_stream->failed)) { lv_video_pause(wv_vid, 0); auto_paused = 0; }
    if (auto_paused) { spinner(W / 2, H / 2, 20); text_center(W / 2, H / 2 + 46, DIM, 15, "Buffering\u2026"); }
  }
  if ((frame_no < wchrome_until || wdetails || (wv_vid && lv_video_paused(wv_vid) && !auto_paused)) && wv_vid && whero.drag <= 0) {
    int a = frame_no + 30 > wchrome_until && !wdetails ? (int)((wchrome_until - frame_no) * 8) : 240;
    if (a > 240 || a < 0 || lv_video_paused(wv_vid)) a = 240;
    // (still downloading as it plays: how far along, the bar's lighter part)
    float have = wv_stream && !wv_stream->done && wv_stream->total > 0 ? (float)wv_stream->have / (float)wv_stream->total : -1;
    if (!auto_paused) vctl_draw(wv_vid, a, have);
  }
  if (wv_vid) vctl_flash_draw();

  // --- the details, pulled up ---
  wdetails_y += ((wdetails ? 1 : 0) - wdetails_y) * 0.25f;
  if (wdetails_y > 0.01f) {
    float top = H - wdetails_y * 300;
    rrect(0, top, W, 330, 20, with_alpha(SURF, 250));
    rrect(W / 2 - 24, top + 8, 48, 5, 3, with_alpha(TEXT, 60));
    char line[300];
    snprintf(line, sizeof line, "%s #%lld", LV_SITES[cfg.site].name, p->id);
    text(30, top + 46, TEXT, 22, line);
    // ≈ Similar (✕ while the details are up)
    rrect(W - 200, top + 22, 170, 36, 18, ACC);
    text_center(W - 115, (int)top + 46, RGBA(20, 12, 16, 255), 16, "\u2248 Similar  \u2715");
    if (hit(W - 200, (int)top + 22, 170, 36)) { if (wv_similar(p)) return; }
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
