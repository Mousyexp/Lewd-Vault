// Lewd Vault for the PS Vita: the phone app's library, full view and slideshow, native (vita2d), on
// the same vault folder as the PC and the phones (vault.c reads it the way src-tauri does).
//
// The vault is copied onto the memory card (USB or VitaShell's FTP) into ux0:data/LewdVault/; an
// Untraceable vault stays encrypted there and opens with its password. Views, hearts and time go
// into its views.csv, like on the PC, so copying the folder back carries them home.
//
// Touch works as on the phone (tap, drag, swipe, pinch); the buttons do it all too:
//   Library  D-pad/stick move · ✕ open (hold: quick actions) · L/R category · △ sort · □ filters ·
//            START slideshow · ○ next tab · SELECT cover
//   Viewer   ←/→ or L/R previous/next · ✕ play/pause or zoom · △ details · □ heart · ○ back
//   Video    ←/→ 10 s · stick scrubs · ↑ 2×/4× · ↓ A-B loop · L/R previous/next
//   Slideshow tab: the PC's level sessions (show.c) or a simple one; Continue watching is shared
//            with the PC through the vault's _app/kv/resume.json
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>
#include <psp2/kernel/threadmgr/callback.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <pthread.h>
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <vita2d.h>

#include "image.h"
#include "sfx.h"
#include "lvcrypt.h"
#include "vault.h"
#include "video.h"
#include "json.h"
#include "booru.h"
#include "drive.h"
#include "net.h"
#include <psp2/kernel/clib.h>
#ifdef LV_DEBUG
#define LOG(...) do { char lb_[256]; int ln_ = snprintf(lb_, sizeof lb_, __VA_ARGS__); SceUID lf_ = sceIoOpen("ux0:data/LWDV00001/log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666); if (lf_ >= 0) { sceIoWrite(lf_, lb_, ln_ > 255 ? 255 : ln_); sceIoClose(lf_); } } while (0)
#else
#define LOG(...) do { } while (0)
#endif

int _newlib_heap_size_user = 200 * 1024 * 1024;

#define W 960
#define H 544
#define DATA "ux0:data/LWDV00001"
#define TOP 66
#define BAR 58
#define COLS 6
#define CARD_W 148
#define CARD_H 197
#define GAP 8
#define MARGIN ((W - COLS * CARD_W - (COLS - 1) * GAP) / 2)
// (a thumbnail is kept whole, up to 336 x 168: a 2:1 landscape picture at a row's full height, or
// a portrait one 168 tall)
#define CELL_W 336
#define CELL_H 168
#define SLOTS 72 // (four atlases of 18 cells; the Browser has four more)

// ---------- colours (the vault's theme picks the accent, as on the phone) ----------
#define RGBA(r, g, b, a) ((unsigned)(r) | (unsigned)(g) << 8 | (unsigned)(b) << 16 | (unsigned)(a) << 24)
static unsigned BG = RGBA(18, 14, 17, 255), SURF = RGBA(34, 27, 32, 255), SURF2 = RGBA(48, 39, 45, 255);
static unsigned TEXT = RGBA(247, 232, 236, 255), DIM = RGBA(190, 170, 180, 255), FAINT = RGBA(120, 105, 114, 255);
static unsigned ACC = RGBA(255, 92, 138, 255), ACC2 = RGBA(255, 160, 110, 255);
static const struct { const char *name; unsigned a, b; } THEMES[] = {
  {"rose", RGBA(255, 92, 138, 255), RGBA(255, 160, 110, 255)}, {"berry", RGBA(214, 70, 160, 255), RGBA(140, 90, 255, 255)},
  {"ocean", RGBA(40, 170, 220, 255), RGBA(60, 220, 190, 255)}, {"sunset", RGBA(255, 120, 70, 255), RGBA(255, 70, 130, 255)},
  {"matcha", RGBA(120, 190, 100, 255), RGBA(200, 220, 120, 255)}, {"cyber", RGBA(0, 230, 200, 255), RGBA(255, 40, 160, 255)},
  {"vaporwave", RGBA(255, 110, 200, 255), RGBA(90, 200, 255, 255)}, {"honey", RGBA(245, 180, 40, 255), RGBA(255, 120, 60, 255)},
  {"noir", RGBA(220, 220, 220, 255), RGBA(150, 150, 160, 255)}, {"galaxy", RGBA(140, 110, 255, 255), RGBA(255, 110, 200, 255)},
};
static unsigned with_alpha(unsigned c, int a) { return (c & 0x00FFFFFF) | (unsigned)a << 24; }

// ---------- text ----------
static vita2d_font *font;
static vita2d_pgf *pgf; // the system's font (Japanese and more), where the firmware has it
static int has_wide(const char *s) {
  for (; *s; s++) if ((unsigned char)*s >= 0xE3) return 1;
  return 0;
}
static int text_w(int size, const char *s) {
  if (pgf && has_wide(s)) return vita2d_pgf_text_width(pgf, size / 17.0f, s);
  return vita2d_font_text_width(font, size, s);
}
static void text(int x, int y, unsigned col, int size, const char *s) {
  if (pgf && has_wide(s)) vita2d_pgf_draw_text(pgf, x, y, col, size / 17.0f, s);
  else vita2d_font_draw_text(font, x, y, col, size, s);
}
static void textf(int x, int y, unsigned col, int size, const char *fmt, ...) {
  char b[512];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  text(x, y, col, size, b);
}
// cut to fit max_w, with an ellipsis
static void text_fit(int x, int y, unsigned col, int size, const char *s, int max_w) {
  if (text_w(size, s) <= max_w) { text(x, y, col, size, s); return; }
  char b[512];
  size_t n = strlen(s);
  if (n > sizeof b - 4) n = sizeof b - 4;
  memcpy(b, s, n);
  while (n > 0) {
    n--;
    while (n > 0 && ((unsigned char)b[n] & 0xC0) == 0x80) n--; // (whole UTF-8 characters)
    memcpy(b + n, "…", 4);
    if (text_w(size, b) <= max_w) break;
  }
  text(x, y, col, size, b);
}
static void text_center(int cx, int y, unsigned col, int size, const char *s) { text(cx - text_w(size, s) / 2, y, col, size, s); }

// Circles and round corners from one small anti-aliased disc texture: a single quad each, where
// vita2d's own filled circle is a fan of 100 triangles out of its per-frame vertex pool (a busy
// screen of cards and chips would ask for hundreds of those every frame).
static vita2d_texture *disc;
#define DISC 64
static void make_disc(void) {
  disc = vita2d_create_empty_texture(DISC, DISC);
  if (!disc) return;
  uint8_t *d = vita2d_texture_get_datap(disc);
  unsigned stride = vita2d_texture_get_stride(disc);
  for (int y = 0; y < DISC; y++)
    for (int x = 0; x < DISC; x++) {
      float dx = x + 0.5f - DISC / 2.0f, dy = y + 0.5f - DISC / 2.0f;
      float a = DISC / 2.0f - sqrtf(dx * dx + dy * dy) + 0.5f; // (one pixel of soft edge)
      uint8_t *p = d + y * stride + x * 4;
      p[0] = p[1] = p[2] = 255;
      p[3] = a <= 0 ? 0 : a >= 1 ? 255 : (uint8_t)(a * 255);
    }
  vita2d_texture_set_filters(disc, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
}
static void circle(float cx, float cy, float r, unsigned c) {
  if (r <= 0) return;
  if (!disc) { vita2d_draw_fill_circle(cx, cy, r, c); return; }
  float s = r * 2 / DISC;
  vita2d_draw_texture_tint_scale(disc, cx - r, cy - r, s, s, c);
}
static void rrect(float x, float y, float w, float h, float r, unsigned c) {
  if (r * 2 > h) r = h / 2;
  if (r * 2 > w) r = w / 2;
  if (r < 1 || !disc) { vita2d_draw_rectangle(x, y, w, h, c); return; }
  vita2d_draw_rectangle(x + r, y, w - 2 * r, h, c);
  vita2d_draw_rectangle(x, y + r, r, h - 2 * r, c);
  vita2d_draw_rectangle(x + w - r, y + r, r, h - 2 * r, c);
  float s = r / (DISC / 2.0f), q = DISC / 2.0f;
  vita2d_draw_texture_tint_part_scale(disc, x, y, 0, 0, q, q, s, s, c);
  vita2d_draw_texture_tint_part_scale(disc, x + w - r, y, q, 0, q, q, s, s, c);
  vita2d_draw_texture_tint_part_scale(disc, x, y + h - r, 0, q, q, q, s, s, c);
  vita2d_draw_texture_tint_part_scale(disc, x + w - r, y + h - r, q, q, q, q, s, s, c);
}
static void heart(float cx, float cy, float s, unsigned c) {
  circle(cx - s * 0.5f, cy - s * 0.2f, s * 0.55f, c);
  circle(cx + s * 0.5f, cy - s * 0.2f, s * 0.55f, c);
  for (int i = 0; i < (int)(s * 1.3f); i++) {
    float t = i / (s * 1.3f), half = s * 1.02f * (1 - t);
    vita2d_draw_rectangle(cx - half, cy - s * 0.1f + i, half * 2, 1, c);
  }
}

// ---------- the grid: justified rows ----------
// Each card keeps its picture's shape (a landscape one wide, a portrait one narrow), the cards of a
// row share its height, and each row is stretched to the screen's width (as Photos lays them out).
static int ROW_H = 168; // (the card size in Filters changes it)
#define JMARGIN 8
typedef struct { float x, y, w, h; } Rect;
typedef struct { Rect *r; short *row; int n, cap; float height; } Grid;
static float clamp_aspect(float a) { return !(a > 0) ? 0.75f : a < 0.5f ? 0.5f : a > 2.0f ? 2.0f : a; }
static void grid_build(Grid *g, int n, float (*aspect)(int k)) {
  if (n + 1 > g->cap) {
    int cap = n + 64;
    Rect *r = realloc(g->r, cap * sizeof *r);
    short *rw = realloc(g->row, cap * sizeof *rw);
    if (r) g->r = r;
    if (rw) g->row = rw;
    if (!r || !rw) { g->n = 0; g->height = 0; return; }
    g->cap = cap;
  }
  g->n = n;
  float avail = W - 2 * JMARGIN;
  int k = 0, row = 0;
  float y = 0;
  while (k < n) {
    int start = k;
    float sum = 0;
    while (k < n) {
      float a = clamp_aspect(aspect(k));
      if (k > start && (sum + a) * ROW_H + (k - start) * GAP > avail) break;
      sum += a;
      k++;
    }
    float h = (avail - (k - start - 1) * GAP) / sum;
    if (k >= n && h > ROW_H) h = ROW_H; // (the last row isn't stretched)
    if (h > ROW_H * 1.25f) h = ROW_H * 1.25f;
    float x = JMARGIN;
    for (int j = start; j < k; j++) {
      float w = clamp_aspect(aspect(j)) * h;
      g->r[j] = (Rect){x, y, w, h};
      g->row[j] = (short)(row < 32767 ? row : 32767);
      x += w + GAP;
    }
    y += h + GAP;
    row++;
  }
  g->height = y;
}
// the first card whose bottom is below `top`
static int grid_first(const Grid *g, float top) {
  int lo = 0, hi = g->n;
  while (lo < hi) { int m = (lo + hi) / 2; if (g->r[m].y + g->r[m].h < top) lo = m + 1; else hi = m; }
  // (back to the start of that row)
  while (lo > 0 && lo < g->n && g->row[lo - 1] == g->row[lo]) lo--;
  return lo;
}
// ↑ / ↓: the card in the row above or below nearest the same place across
static int grid_move(const Grid *g, int k, int dir) {
  if (k < 0 || k >= g->n) return k;
  float cx = g->r[k].x + g->r[k].w / 2;
  int want = g->row[k] + dir, best = k;
  float bd = 1e9f;
  for (int j = k + dir; j >= 0 && j < g->n; j += dir) {
    if (g->row[j] == g->row[k]) continue;
    if (g->row[j] != want) break;
    float d = fabsf(g->r[j].x + g->r[j].w / 2 - cx);
    if (d < bd) { bd = d; best = j; }
  }
  return best;
}

// ---------- the app's own settings (ux0:data/LWDV00001/config.txt) ----------
static struct {
  char vault[512];
  int sort, kind, interval, shuffle, volume;
  int site, ratings, wsort; // the Browser's site, ratings (LV_R_* bits) and order
  char wquery[128], save_cat[64];
  int wpages, wsel; // where the Browser was: pages loaded, the post picked (it picks up there)
  int sounds;       // the interface sounds' volume, 0-100 (0: off)
  int dbltap, glow; // double tap in the full view: 0 zoom, 1 heart; the picture's colours behind it
  int wspan, wfull; // the Browser: Top's span (LV_SPAN_*), saves at full size
  int hide, tlock;  // SELECT shows: 0 the clock, 1 the calculator; touch lock's time (privacy.c)
} cfg = {"", 0, 0, 5, 0, 80, 0, 15, 0, "", "", 0, 0, 70, 0, 1, 0, 0, 0, 0};
static void cfg_load(void) {
  FILE *f = fopen(DATA "/config.txt", "r");
  if (!f) return;
  char line[600];
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq++ = 0;
    if (!strcmp(line, "vault")) snprintf(cfg.vault, sizeof cfg.vault, "%s", eq);
    else if (!strcmp(line, "sort")) cfg.sort = atoi(eq);
    else if (!strcmp(line, "kind")) cfg.kind = atoi(eq);
    else if (!strcmp(line, "interval")) cfg.interval = atoi(eq);
    else if (!strcmp(line, "shuffle")) cfg.shuffle = atoi(eq);
    else if (!strcmp(line, "volume")) cfg.volume = atoi(eq);
    else if (!strcmp(line, "site")) cfg.site = atoi(eq);
    else if (!strcmp(line, "ratings")) cfg.ratings = atoi(eq);
    else if (!strcmp(line, "wsort")) cfg.wsort = atoi(eq);
    else if (!strcmp(line, "wquery")) snprintf(cfg.wquery, sizeof cfg.wquery, "%s", eq);
    else if (!strcmp(line, "save_cat")) snprintf(cfg.save_cat, sizeof cfg.save_cat, "%s", eq);
    else if (!strcmp(line, "wpages")) cfg.wpages = atoi(eq);
    else if (!strcmp(line, "wsel")) cfg.wsel = atoi(eq);
    else if (!strcmp(line, "sounds")) cfg.sounds = atoi(eq);
    else if (!strcmp(line, "dbltap")) cfg.dbltap = atoi(eq) != 0;
    else if (!strcmp(line, "glow")) cfg.glow = atoi(eq) != 0;
    else if (!strcmp(line, "wspan")) cfg.wspan = atoi(eq);
    else if (!strcmp(line, "wfull")) cfg.wfull = atoi(eq) != 0;
    else if (!strcmp(line, "hide")) cfg.hide = atoi(eq) != 0;
    else if (!strcmp(line, "tlock")) cfg.tlock = atoi(eq) & 3;
  }
  fclose(f);
  if (cfg.interval < 2 || cfg.interval > 60) cfg.interval = 5;
  if (cfg.volume < 0 || cfg.volume > 100) cfg.volume = 80;
  if (cfg.site < 0 || cfg.site >= 64) cfg.site = 0;
  if (!(cfg.ratings & 15)) cfg.ratings = 15;
  if (cfg.wsort < 0 || cfg.wsort > 3) cfg.wsort = 0;
  if (cfg.wpages < 0 || cfg.wpages > 50) cfg.wpages = 0;
  if (cfg.kind < 0 || cfg.kind > 3) cfg.kind = 0;
  if (cfg.sort < 0 || cfg.sort >= 7) cfg.sort = 0;
  if (cfg.wsel < 0) cfg.wsel = 0;
  if (cfg.sounds < 0 || cfg.sounds > 100) cfg.sounds = 70;
  if (cfg.wspan < 0 || cfg.wspan >= LV_NSPANS) cfg.wspan = 0;
}
static void cfg_save(void) {
  FILE *f = fopen(DATA "/config.txt.part", "w");
  if (!f) return;
  fprintf(f, "vault=%s\nsort=%d\nkind=%d\ninterval=%d\nshuffle=%d\nvolume=%d\nsite=%d\nratings=%d\nwsort=%d\nwquery=%s\nsave_cat=%s\nwpages=%d\nwsel=%d\nsounds=%d\ndbltap=%d\nglow=%d\nwspan=%d\nwfull=%d\nhide=%d\ntlock=%d\n",
          cfg.vault, cfg.sort, cfg.kind, cfg.interval, cfg.shuffle, cfg.volume, cfg.site, cfg.ratings, cfg.wsort, cfg.wquery, cfg.save_cat,
          cfg.wpages, cfg.wsel, cfg.sounds, cfg.dbltap, cfg.glow, cfg.wspan, cfg.wfull, cfg.hide, cfg.tlock);
  fclose(f);
  sceIoRemove(DATA "/config.txt");
  sceIoRename(DATA "/config.txt.part", DATA "/config.txt");
}

// ---------- the vault, and what the library shows ----------
static LvVault vault;
static int loaded_vault;
static int *shown, nshown;     // indices into vault.items
static int cat = -1;           // -1: every category
static char query[128];
static const char *SORTS[] = {"Newest", "Oldest", "Most viewed", "Top rated", "Most hearted", "Shuffle", "For you"};
#define NSORTS 7
static int lib_pass(int i);
static void lib_prepare(void);
static void sort_foryou(void);

static int is_video(const LvItem *it) { return !strcmp(it->kind, "video"); }
static int is_gif(const LvItem *it) { return !strcmp(it->kind, "gif"); }
static int ext_is(const char *path, const char *e1, const char *e2, const char *e3) {
  const char *d = path ? strrchr(path, '.') : NULL;
  if (!d) return 0;
  return !strcasecmp(d + 1, e1) || (e2 && !strcasecmp(d + 1, e2)) || (e3 && !strcasecmp(d + 1, e3));
}
static unsigned rnd_state = 12345;
static unsigned rnd(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5; return rnd_state; }

// Sorting reads only the compact hot records (vault.h), never the items' strings.
static int sort_mode;
static long long views_at(int32_t view) { return view >= 0 ? vault.views[view].views : 0; }
static long long likes_at(int32_t view) { return view >= 0 ? vault.views[view].likes : 0; }
static int cmp_items(const void *a, const void *b) {
  const LvHot *x = &vault.hot[*(const int *)a], *y = &vault.hot[*(const int *)b];
  long long d = 0;
  switch (sort_mode) {
    case 1: d = y->added - x->added; return d < 0 ? 1 : d > 0 ? -1 : 0;
    case 2: d = views_at(y->view) - views_at(x->view); break;
    case 3: d = (int)y->quality - (int)x->quality; break;
    case 4: d = likes_at(y->view) - likes_at(x->view); break;
  }
  if (d) return d > 0 ? 1 : -1;
  d = y->added - x->added;
  return d > 0 ? 1 : d < 0 ? -1 : 0;
}
static Grid lgrid;
static float lib_aspect(int k) {
  const LvItem *it = &vault.items[shown[k]];
  return it->width > 0 && it->height > 0 ? (float)it->width / it->height : 0.75f;
}

static void refilter(void) {
  free(shown);
  shown = malloc((vault.n + 1) * sizeof *shown);
  nshown = 0;
  if (!shown) { grid_build(&lgrid, 0, NULL); return; }
  lib_prepare();
  for (int i = 0; i < vault.n; i++) {
    if (cat >= 0 && vault.hot[i].cat != cat) continue;
    if (!lib_pass(i)) continue;
    shown[nshown++] = i;
  }
  sort_mode = cfg.sort;
  if (cfg.sort == 6) sort_foryou();
  else if (cfg.sort == 5) {
    for (int k = nshown - 1; k > 0; k--) { int j = rnd() % (k + 1), t = shown[k]; shown[k] = shown[j]; shown[j] = t; }
  } else {
    qsort(shown, nshown, sizeof *shown, cmp_items);
  }
  grid_build(&lgrid, nshown, lib_aspect);
}

static void now_iso(char *out, size_t cap) {
  time_t t = time(NULL);
  struct tm g;
  gmtime_r(&t, &g);
  strftime(out, cap, "%Y-%m-%dT%H:%M:%S.000Z", &g);
}

// ---------- pictures, decoded on a worker thread ----------
// The worker takes what the screen wants most (the big picture first, then the cards in view, then
// the ones about to scroll in) and hands back pixels; this thread turns them into textures.
// Thumbnails are cached small as WebP in ux0:data/LWDV00001/cache (encrypted for an Untraceable
// vault, with its key), so a card costs a few milliseconds after the first time.
typedef struct { int item; vita2d_texture *tex; unsigned used; } Slot;
static Slot slots[SLOTS];
static signed char *tstate; // per item: 0 nothing, 1 asked, 2 in a slot, 3 failed
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
// The vault and the thumbnail thread: the thread holds vmx while it reads an item; whatever changes
// the loaded vault (opening, closing, a save from the Browser) holds it too. vgen counts the vaults
// opened: work for one that's gone is dropped, never used.
static pthread_mutex_t vmx = PTHREAD_MUTEX_INITIALIZER;
static volatile int vgen = 1;

static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int want[64], nwant;
static struct { int item, gen, max_w, max_h; } big_ask = {-1, 0, 0, 0};
typedef struct { int item, big, gen, vgen; LvPixels px; } Done;
static Done done[16];
static int ndone;
static volatile int worker_run = 1;
static unsigned frame_no;
static char cache_dir[600];

static unsigned long fnv(const char *s) {
  unsigned long h = 2166136261u;
  while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
  return h;
}

// The size to decode a thumbnail at: whole, inside a cell (the grid keeps each picture's shape).
static void cover_size(const uint8_t *b, size_t n, int *mw, int *mh) {
  (void)b; (void)n;
  *mw = CELL_W;
  *mh = CELL_H;
}

static int cache_broken;
// The average difference of a sample of pixels, small enough for a lossy copy of the same picture.
static int pixels_close(const LvPixels *a, const LvPixels *b) {
  long long diff = 0, n = 0;
  size_t total = (size_t)a->w * a->h;
  for (size_t k = 0; k < total; k += 7, n++)
    for (int c = 0; c < 3; c++) diff += abs(a->rgba[k * 4 + c] - b->rgba[k * 4 + c]);
  return n && diff / (n * 3) < 24;
}

static int thumb_pixels(int i, LvPixels *px) {
  LvItem *it = &vault.items[i];
  char cp[800];
  snprintf(cp, sizeof cp, "%s/%08lx%08lx-w.jpg", cache_dir, fnv(it->thumb), fnv(it->id)); // (-w: whole, not cropped)
  size_t n = 0;
  uint8_t *b = lv_read_all(cp, lv_key(&vault), &n);
  if (b) {
    int cw, ch;
    cover_size(b, n, &cw, &ch);
    int r = lv_decode(b, n, cw, ch, px);
    free(b);
    if (r == 0) return 0;
  }
  b = lv_thumb_bytes(&vault, it, &n);
  if (!b && it->path && !is_video(it) && !is_gif(it)) b = lv_read_all(it->path, lv_key(&vault), &n); // (no thumbnail: the picture itself)
  if (!b) return -1;
  // cover the card: decode to its width (the card crops the height)
  int sw = 0, shh = 0, small = lv_image_size(b, n, &sw, &shh) && sw <= CELL_W * 3 / 2 && shh <= CELL_H * 3 / 2;
  int cw, ch;
  cover_size(b, n, &cw, &ch);
  int r = lv_decode(b, n, cw, ch, px);
  free(b);
  if (r != 0) return -1;
  if (small) return 0; // (already card-sized, like the Browser's saves: nothing to gain from a copy)
  // Kept small for next time, once the copy is known to come back right: it is decoded again and
  // compared, and if the encoder ever gets it wrong (Vita3K's does) nothing more is cached.
  if (!cache_broken) {
    size_t en = 0;
    uint8_t *enc = lv_encode_jpeg(px, 85, &en);
    LvPixels back;
    if (enc && lv_decode(enc, en, px->w, px->h, &back) == 0) {
      int ok = back.w == px->w && back.h == px->h && pixels_close(px, &back);
      free(back.rgba);
      if (ok) lv_write_all(cp, lv_key(&vault), enc, en);
      else cache_broken = 1;
    }
    free(enc);
  }
  return 0;
}

static void *worker(void *arg) {
  while (worker_run) {
    pthread_mutex_lock(&mx);
    while (worker_run && big_ask.item < 0 && nwant == 0) pthread_cond_wait(&cv, &mx);
    if (!worker_run) { pthread_mutex_unlock(&mx); break; }
    int item = -1, big = 0, gen = 0, mw = 0, mh = 0;
    if (big_ask.item >= 0) {
      item = big_ask.item; gen = big_ask.gen; mw = big_ask.max_w; mh = big_ask.max_h; big = 1;
      big_ask.item = -1;
    } else {
      item = want[0];
      memmove(want, want + 1, --nwant * sizeof *want);
    }
    int full = ndone >= (int)(sizeof done / sizeof *done);
    int my_vgen = vgen;
    pthread_mutex_unlock(&mx);
    if (full) { sceKernelDelayThread(4000); pthread_mutex_lock(&mx); if (!big && my_vgen == vgen && item < vault.n) tstate[item] = 0; pthread_mutex_unlock(&mx); continue; }
    LvPixels px = {0};
    int r = -1;
    pthread_mutex_lock(&vmx);
    if (my_vgen != vgen || !loaded_vault || item < 0 || item >= vault.n) { pthread_mutex_unlock(&vmx); continue; } // (that vault is gone)
    if (big) {
      LvItem *it = &vault.items[item];
      size_t n = 0;
      uint8_t *b = it->path ? lv_read_all(it->path, lv_key(&vault), &n) : NULL;
      if (b) { r = lv_decode(b, n, mw, mh, &px); free(b); }
      if (r != 0) { b = lv_thumb_bytes(&vault, it, &n); if (b) { r = lv_decode(b, n, mw, mh, &px); free(b); } }
    } else {
      r = thumb_pixels(item, &px);
    }
    pthread_mutex_unlock(&vmx);
    pthread_mutex_lock(&mx);
    if (my_vgen != vgen) { free(px.rgba); }
    else if (r == 0) done[ndone++] = (Done){item, big, gen, my_vgen, px};
    else if (!big) tstate[item] = 3;
    else done[ndone++] = (Done){item, big, gen, my_vgen, {NULL, 0, 0}};
    pthread_mutex_unlock(&mx);
  }
  return NULL;
}

// Textures are freed at the start of the next frame, once the GPU is done with the last one: never
// while a frame is being put together (it may already use them). The list grows as it must.
static vita2d_texture **pending_free;
static int npending, cappending;
static void free_later(vita2d_texture *t) {
  if (!t) return;
  if (npending == cappending) {
    int cap = cappending ? cappending * 2 : 64;
    vita2d_texture **p = realloc(pending_free, cap * sizeof *p);
    if (!p) return; // (a leak, not a crash)
    pending_free = p;
    cappending = cap;
  }
  pending_free[npending++] = t;
}
static vita2d_texture *upload(const LvPixels *px) {
  vita2d_texture *t = vita2d_create_empty_texture(px->w, px->h);
  if (!t) return NULL;
  uint8_t *d = vita2d_texture_get_datap(t);
  unsigned stride = vita2d_texture_get_stride(t);
  for (int y = 0; y < px->h; y++) memcpy(d + (size_t)y * stride, px->rgba + (size_t)y * px->w * 4, (size_t)px->w * 4);
  vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
  return t;
}

// The cards' thumbnails live in a few big atlas textures, 30 card-sized cells each, made once: no
// texture is made or freed while scrolling (the GPU's memory doesn't fragment over a long session).
// Each cell is a texture of its own as far as drawing goes: a view onto its part of the atlas
// (sceGxmTextureInitLinearStrided).
#define ACOLS 3
#define AROWS 6
#define CELLS_PER_ATLAS (ACOLS * AROWS)
#define ATLASES 8 // (cells 0-71 the Library's, 72-143 the Browser's)
static vita2d_texture *atlas[ATLASES];
static vita2d_texture cells[ATLASES * CELLS_PER_ATLAS];
static void atlas_init(void) {
  for (int a = 0; a < ATLASES; a++) {
    atlas[a] = vita2d_create_empty_texture(1024, 1024);
    if (!atlas[a]) continue;
    uint8_t *d = vita2d_texture_get_datap(atlas[a]);
    unsigned stride = vita2d_texture_get_stride(atlas[a]);
    for (int k = 0; k < CELLS_PER_ATLAS; k++) {
      int x = (k % ACOLS) * (CELL_W + 2), y = (k / ACOLS) * (CELL_H + 2);
      vita2d_texture *c = &cells[a * CELLS_PER_ATLAS + k];
      memset(c, 0, sizeof *c);
      sceGxmTextureInitLinearStrided(&c->gxm_tex, d + (size_t)y * stride + (size_t)x * 4, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, CELL_W, CELL_H, stride);
      sceGxmTextureSetMinFilter(&c->gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
      sceGxmTextureSetMagFilter(&c->gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
    }
  }
}
// A thumbnail into its cell, whole: it was decoded to fit CELL_W x CELL_H, so it's copied as it is
// (scaled down only if it somehow isn't), and the cell's view takes its size.
static vita2d_texture *put_cell(int cell, const LvPixels *px) {
  vita2d_texture *at = atlas[cell / CELLS_PER_ATLAS];
  if (!at || !px->rgba || px->w <= 0 || px->h <= 0) return NULL;
  int k = cell % CELLS_PER_ATLAS;
  unsigned stride = vita2d_texture_get_stride(at);
  uint8_t *d = (uint8_t *)vita2d_texture_get_datap(at) + (size_t)((k / ACOLS) * (CELL_H + 2)) * stride + (size_t)((k % ACOLS) * (CELL_W + 2)) * 4;
  float s = fminf(1.0f, fminf((float)CELL_W / px->w, (float)CELL_H / px->h));
  int w = (int)(px->w * s), h = (int)(px->h * s);
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  for (int y = 0; y < h; y++) {
    int yy = s == 1.0f ? y : (int)((y + 0.5f) / s);
    if (yy >= px->h) yy = px->h - 1;
    const uint32_t *srow = (const uint32_t *)(px->rgba + (size_t)yy * px->w * 4);
    uint32_t *drow = (uint32_t *)(d + (size_t)y * stride);
    if (s == 1.0f) { for (int x = 0; x < w; x++) drow[x] = srow[x] | 0xFF000000u; continue; }
    for (int x = 0; x < w; x++) {
      int xx = (int)((x + 0.5f) / s);
      drow[x] = srow[xx < px->w ? xx : px->w - 1] | 0xFF000000u;
    }
  }
  vita2d_texture *c = &cells[cell];
  sceGxmTextureInitLinearStrided(&c->gxm_tex, d, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, w, h, stride);
  sceGxmTextureSetMinFilter(&c->gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
  sceGxmTextureSetMagFilter(&c->gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
  return c;
}


static vita2d_texture *big_tex;
static int big_item = -1, big_gen, big_failed;
// (a slideshow that fades: the picture before stays a moment, fading out under the new one)
static vita2d_texture *fade_tex;
static unsigned fade_at;
static int xfade_wanted;

static short *slot_of; // per item: its slot, or -1 (no search through the slots for every card)
static vita2d_texture *thumb_of(int i) {
  int k = slot_of[i];
  if (tstate[i] == 2 && k >= 0 && slots[k].item == i && slots[k].tex) { slots[k].used = frame_no; return slots[k].tex; }
  return NULL;
}
static void take_results(void) {
  pthread_mutex_lock(&mx);
  int n = ndone;
  Done got[16];
  memcpy(got, done, n * sizeof *got);
  ndone = 0;
  pthread_mutex_unlock(&mx);
  for (int k = 0; k < n; k++) {
    Done *d = &got[k];
    if (d->vgen != vgen || d->item >= vault.n) { free(d->px.rgba); continue; }
    if (d->big) {
      if (d->gen == big_gen) {
        if (xfade_wanted && big_tex && big_item != d->item) { free_later(fade_tex); fade_tex = big_tex; fade_at = frame_no; }
        else free_later(big_tex);
        big_tex = d->px.rgba ? upload(&d->px) : NULL;
        big_item = d->item;
        LOG("big %d: %dx%d tex=%p\n", d->item, d->px.w, d->px.h, (void *)big_tex);
        big_failed = !big_tex;
      }
    } else {
      // the slot used longest ago
      int best = 0;
      for (int s = 1; s < SLOTS; s++) if (slots[s].used < slots[best].used) best = s;
      if (slots[best].tex && slots[best].item >= 0 && slots[best].item < vault.n) { tstate[slots[best].item] = 0; slot_of[slots[best].item] = -1; }
      slots[best].tex = put_cell(best, &d->px);
      slots[best].item = d->item;
      slot_of[d->item] = (short)best;
      slots[best].used = frame_no;
      tstate[d->item] = slots[best].tex ? 2 : 3;
    }
    free(d->px.rgba);
  }
}
// The cards the screen wants now; the ones asked for before and no longer wanted go back to "not
// asked" (only those few are looked at, not every item, every frame).
static void ask_thumbs(const int *items, int n) {
  pthread_mutex_lock(&mx);
  for (int k = 0; k < nwant; k++) if (tstate[want[k]] == 1) tstate[want[k]] = 0;
  nwant = 0;
  for (int k = 0; k < n && nwant < 64; k++)
    if (tstate[items[k]] == 0) { tstate[items[k]] = 1; want[nwant++] = items[k]; }
  if (nwant) pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mx);
}
static void ask_big(int item) {
  pthread_mutex_lock(&mx);
  big_gen++;
  big_ask.item = item;
  big_ask.gen = big_gen;
  big_ask.max_w = 1920;
  big_ask.max_h = 1920;
  big_failed = 0;
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mx);
}
static void drop_textures(void) {
  vita2d_wait_rendering_done();
  for (int k = 0; k < SLOTS; k++) { slots[k].tex = NULL; slots[k].item = -1; slots[k].used = 0; } // (the cells stay: the atlases are for good)
  if (big_tex) vita2d_free_texture(big_tex);
  big_tex = NULL;
  big_item = -1;
  if (fade_tex) vita2d_free_texture(fade_tex);
  fade_tex = NULL;
  for (int k = 0; k < npending; k++) vita2d_free_texture(pending_free[k]);
  npending = 0;
}

// Draws a texture to cover a box (cropped to fill it, like object-fit: cover).
static void draw_cover(vita2d_texture *t, float x, float y, float w, float h, unsigned tint) {
  float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
  float s = fmaxf(w / tw, h / th);
  float sw = w / s, sh = h / s;
  float sx = (tw - sw) / 2, sy = (th - sh) * 0.3f; // (faces tend to be near the top)
  vita2d_draw_texture_tint_part_scale(t, x, y, sx, sy, sw, sh, s, s, tint);
}

// ---------- input ----------
typedef struct {
  unsigned now, pressed, held_ms[32];
  float lx, ly;
  int touching, tx, ty, tcount, t2x, t2y;
  int tap, tap_x, tap_y, dbl;
  float drag_dx, drag_dy; // since the last frame
  int down_x, down_y, moved, flicked;
  float fvx, fvy;
  unsigned down_frame, last_tap_frame;
  int last_tap_x, last_tap_y;
  float bdx, bdy; // the rear touchpad: how far a finger on it moved since the last frame
  int back_on, back_x0;
} Input;
static Input in;
static unsigned prev_buttons;
static int prev_touch, prev_tx, prev_ty;

static void read_input(void) {
  SceCtrlData c;
  sceCtrlPeekBufferPositive(0, &c, 1);
  in.now = c.buttons;
  in.pressed = c.buttons & ~prev_buttons;
  // held buttons repeat (D-pad through the grid)
  for (int b = 0; b < 32; b++) {
    unsigned m = 1u << b;
    if (c.buttons & m) {
      in.held_ms[b] += 16;
      if (in.held_ms[b] > 380 && (in.held_ms[b] - 380) % 80 < 16) in.pressed |= m;
    } else in.held_ms[b] = 0;
  }
  prev_buttons = c.buttons;
  in.lx = (c.lx - 128) / 128.0f;
  in.ly = (c.ly - 128) / 128.0f;
  if (fabsf(in.lx) < 0.25f) in.lx = 0;
  if (fabsf(in.ly) < 0.25f) in.ly = 0;

  SceTouchData t;
  // the rear touchpad: drags only (it's easily touched by the hands holding the Vita: no taps)
  {
    static int bprev, bpx, bpy;
    SceTouchData b;
    in.bdx = in.bdy = 0;
    if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &b, 1) >= 0 && b.reportNum > 0) {
      int x = b.report[0].x / 2, y = b.report[0].y / 2;
      if (bprev) { in.bdx = x - bpx; in.bdy = y - bpy; } else in.back_x0 = x;
      bpx = x; bpy = y; bprev = 1;
      in.back_on = 1;
    } else { bprev = 0; in.back_on = 0; }
  }
  sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1);
  in.tap = in.dbl = in.flicked = 0;
  in.drag_dx = in.drag_dy = 0;
  in.tcount = t.reportNum;
  if (t.reportNum > 0) {
    int x = t.report[0].x / 2, y = t.report[0].y / 2;
    if (t.reportNum > 1) { in.t2x = t.report[1].x / 2; in.t2y = t.report[1].y / 2; }
    if (!prev_touch) { in.down_x = x; in.down_y = y; in.moved = 0; in.down_frame = frame_no; in.fvx = in.fvy = 0; }
    else {
      in.drag_dx = x - prev_tx;
      in.drag_dy = y - prev_ty;
      in.fvx = in.fvx * 0.5f + in.drag_dx * 0.5f;
      in.fvy = in.fvy * 0.5f + in.drag_dy * 0.5f;
    }
    if (abs(x - in.down_x) > 12 || abs(y - in.down_y) > 12) in.moved = 1;
    in.touching = 1;
    in.tx = x; in.ty = y;
    prev_tx = x; prev_ty = y;
    prev_touch = 1;
  } else {
    if (prev_touch) {
      if (!in.moved && frame_no - in.down_frame < 30) {
        in.tap = 1; in.tap_x = prev_tx; in.tap_y = prev_ty;
        if (frame_no - in.last_tap_frame < 18 && abs(prev_tx - in.last_tap_x) < 40 && abs(prev_ty - in.last_tap_y) < 40) { in.dbl = 1; in.last_tap_frame = 0; }
        else { in.last_tap_frame = frame_no; in.last_tap_x = prev_tx; in.last_tap_y = prev_ty; }
      } else if (in.moved) in.flicked = 1;
    }
    in.touching = 0;
    prev_touch = 0;
  }
}
static int hit(int x, int y, int w, int h) { return in.tap && in.tap_x >= x && in.tap_x < x + w && in.tap_y >= y && in.tap_y < y + h; }

// ---------- the system keyboard ----------
static uint16_t ime_title[64], ime_text[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1], ime_init[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static int ime_open, ime_for; // 1 password, 2 search
static int ime_number; // (the next one: digits only)
static void to16(const char *s, uint16_t *out, int cap) {
  int k = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p && k < cap - 1;) {
    unsigned c = *p++;
    if (c >= 0xE0 && p[0] && p[1]) { c = (c & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F); p += 2; }
    else if (c >= 0xC0 && p[0]) { c = (c & 0x1F) << 6 | (p[0] & 0x3F); p += 1; }
    out[k++] = (uint16_t)c;
  }
  out[k] = 0;
}
static void to8(const uint16_t *s, char *out, int cap) {
  int k = 0;
  for (; *s && k < cap - 4; s++) {
    unsigned c = *s;
    if (c < 0x80) out[k++] = (char)c;
    else if (c < 0x800) { out[k++] = 0xC0 | c >> 6; out[k++] = 0x80 | (c & 0x3F); }
    else { out[k++] = 0xE0 | c >> 12; out[k++] = 0x80 | ((c >> 6) & 0x3F); out[k++] = 0x80 | (c & 0x3F); }
  }
  out[k] = 0;
}
static void ime_start(int what, const char *title, const char *initial, int password) {
  SceImeDialogParam p;
  sceImeDialogParamInit(&p);
  to16(title, ime_title, 64);
  to16(initial, ime_init, SCE_IME_DIALOG_MAX_TEXT_LENGTH);
  memset(ime_text, 0, sizeof ime_text);
  p.supportedLanguages = 0;
  p.languagesForced = SCE_FALSE;
  p.type = ime_number ? SCE_IME_TYPE_NUMBER : SCE_IME_TYPE_DEFAULT;
  ime_number = 0;
  p.option = password ? SCE_IME_OPTION_NO_AUTO_CAPITALIZATION | SCE_IME_OPTION_NO_ASSISTANCE : 0;
  p.textBoxMode = password ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
  p.title = ime_title;
  p.maxTextLength = 120;
  p.initialText = ime_init;
  p.inputTextBuffer = ime_text;
  if (sceImeDialogInit(&p) >= 0) { ime_open = 1; ime_for = what; }
}
// 1 and the text when the person pressed Enter, -1 cancelled, 0 still open
static int ime_poll(char *out, int cap) {
  if (!ime_open) return -1;
  SceCommonDialogStatus s = sceImeDialogGetStatus();
  if (s != SCE_COMMON_DIALOG_STATUS_FINISHED) return 0;
  SceImeDialogResult r;
  memset(&r, 0, sizeof r);
  sceImeDialogGetResult(&r);
  sceImeDialogTerm();
  ime_open = 0;
  if (r.button != SCE_IME_DIALOG_BUTTON_ENTER) return -1;
  to8(ime_text, out, cap);
  return 1;
}

// ---------- screens ----------
enum { S_PICK, S_LOCKED, S_UNLOCKING, S_LIBRARY, S_VIEWER, S_SETTINGS, S_COVER, S_WEB, S_WEBVIEW, S_SHOW, S_DRIVE, S_PIN, S_STATS, S_WIFI, S_CALC, S_CLEAN, S_HEALTH };
static int screen = S_PICK, cover_return = S_LIBRARY;
static char found[16][512];
static int nfound, pick_sel;
static char message[256];
static unsigned message_until;
static void say(const char *m) { snprintf(message, sizeof message, "%s", m); message_until = frame_no + 150; }

static void find_vaults(void) {
  nfound = 0;
  static const char *bases[] = {"ux0:data/LewdVault", "uma0:data/LewdVault", "imc0:data/LewdVault", "ux0:LewdVault", "ux0:data/Image Vault data"};
  for (size_t b = 0; b < sizeof bases / sizeof *bases && nfound < 16; b++) {
    if (lv_is_vault(bases[b])) { snprintf(found[nfound++], 512, "%s", bases[b]); continue; }
    DIR *d = opendir(bases[b]);
    if (!d) continue;
    struct dirent *e;
    while ((e = readdir(d)) && nfound < 16) {
      if (e->d_name[0] == '.') continue;
      char p[512];
      snprintf(p, sizeof p, "%s/%s", bases[b], e->d_name);
      if (lv_is_vault(p)) snprintf(found[nfound++], 512, "%s", p);
    }
    closedir(d);
  }
}

static void apply_theme(void) {
  for (size_t k = 0; k < sizeof THEMES / sizeof *THEMES; k++)
    if (!strcmp(THEMES[k].name, vault.theme)) { ACC = THEMES[k].a; ACC2 = THEMES[k].b; return; }
  ACC = THEMES[0].a; ACC2 = THEMES[0].b;
}

static int sel;
static float scroll, scroll_v;
static void close_vault(void) {
  if (!loaded_vault) return;
  pthread_mutex_lock(&mx);
  vgen++; // (the thumbnail thread's work for this vault is dropped from now on)
  nwant = 0;
  big_ask.item = -1;
  pthread_mutex_unlock(&mx);
  pthread_mutex_lock(&vmx);
  lv_vault_save_views(&vault);
  drop_textures();
  loaded_vault = 0;
  lv_vault_free(&vault);
  pthread_mutex_unlock(&vmx);
}
static int decoy_session; // (opened with the decoy PIN: nothing about the lock, or which vault, changes)
static void open_vault(const char *path_in) {
  char path[512];
  snprintf(path, sizeof path, "%s", path_in);
  close_vault();
  pthread_mutex_lock(&vmx);
  lv_vault_init(&vault, path);
  pthread_mutex_unlock(&vmx);
  if (!decoy_session) { snprintf(cfg.vault, sizeof cfg.vault, "%s", path); cfg_save(); }
  screen = vault.untraceable ? S_LOCKED : S_UNLOCKING;
}
// Unlocking (two seconds of key making) and reading the vault (a big one takes a while) happen on a
// thread of their own, so the screen keeps moving: the main loop looks at load_state.
static volatile int load_state; // 0 idle, 1 working, 2 done, 3 failed, 4 wrong password, 5 no check file
static const char *load_why;
static char password[128];
static void *load_main(void *arg) {
  int unlock = (int)(intptr_t)arg;
  pthread_mutex_lock(&vmx);
  int st = 2;
  if (unlock) {
    int ok = lv_vault_unlock(&vault, password);
    memset(password, 0, sizeof password);
    if (ok != 1) st = ok == 0 ? 4 : 5;
  }
  if (st == 2 && lv_vault_load(&vault, &load_why) != 0) st = 3;
  pthread_mutex_unlock(&vmx);
  load_state = st;
  return NULL;
}
static void start_load(int unlock) {
  load_state = 1;
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 256 * 1024);
  if (pthread_create(&t, &at, load_main, (void *)(intptr_t)unlock) != 0) { load_main((void *)(intptr_t)unlock); return; }
  pthread_detach(t);
}
static void show_opened(void); // (show.c: Continue watching read, the session's stages made again)
static void finish_open(void) {
  const char *why = load_why;
  if (load_state != 2) { say(why ? why : "The vault could not be opened"); screen = S_PICK; load_state = 0; return; }
  load_state = 0;
  pthread_mutex_lock(&mx);
  free(tstate);
  tstate = calloc(vault.n + 1, 1);
  free(slot_of);
  slot_of = malloc((vault.n + 1) * sizeof *slot_of);
  if (!tstate || !slot_of) {
    pthread_mutex_unlock(&mx);
    say("Not enough memory for this vault");
    pthread_mutex_lock(&vmx);
    lv_vault_free(&vault);
    pthread_mutex_unlock(&vmx);
    screen = S_PICK;
    return;
  }
  for (int i = 0; i <= vault.n; i++) slot_of[i] = -1;
  loaded_vault = 1;
  pthread_mutex_unlock(&mx);
  for (int k = 0; k < SLOTS; k++) slots[k].item = -1;
  snprintf(cache_dir, sizeof cache_dir, DATA "/cache/%08lx", fnv(vault.root));
  sceIoMkdir(DATA "/cache", 0777);
  sceIoMkdir(cache_dir, 0777);
  apply_theme();
  cat = -1;
  query[0] = 0;
  refilter();
  show_opened();
  scroll = scroll_v = 0;
  sel = 0;
  screen = S_LIBRARY;
}

// ----- the picker: which vault, or how to put one on the Vita -----
static void open_drive(int from); // (drive_ui.c)
static void draw_pick(void) {
  text(40, 70, TEXT, 34, "Lewd Vault");
  text(40, 100, DIM, 18, "for PS Vita");
  if (nfound == 0) {
    rrect(40, 140, W - 80, 300, 18, SURF);
    text(70, 190, TEXT, 22, "No vault on this Vita yet");
    const char *lines[] = {
      "1. On your PC, find the vault folder (\"Image Vault data\", or data.temp for Untraceable).",
      "2. Copy it to the memory card, into  ux0:data/LewdVault/",
      "    (USB with Content Manager's file mode, or VitaShell: SELECT starts its FTP).",
      "3. Come back here and press ✕ to look again.",
      "An Untraceable vault stays encrypted on the card and opens with its password."};
    for (int k = 0; k < 5; k++) text(70, 232 + k * 34, k == 4 ? FAINT : DIM, 17, lines[k]);
    rrect(40, H - 80, 320, 46, 23, SURF2);
    text_center(200, H - 50, TEXT, 18, "\u25b3  Or from Google Drive");
    if ((in.pressed & SCE_CTRL_TRIANGLE) || hit(40, H - 80, 320, 46)) { sfx(SFX_OPEN); open_drive(S_PICK); return; }
    if (in.pressed & SCE_CTRL_CROSS || hit(40, 140, W - 80, 300)) find_vaults();
    return;
  }
  text(40, 150, DIM, 18, "Choose a vault");
  for (int k = 0; k < nfound; k++) {
    int y = 170 + k * 64;
    int on = k == pick_sel;
    rrect(40, y, W - 80, 54, 14, on ? SURF2 : SURF);
    if (on) rrect(40, y, 6, 54, 3, ACC);
    const char *nm = strrchr(found[k], '/');
    text_fit(64, y + 24, TEXT, 20, nm ? nm + 1 : found[k], W - 200);
    text_fit(64, y + 45, FAINT, 15, found[k], W - 200);
    if (lv_is_untraceable(found[k])) text(W - 170, y + 34, ACC, 16, "Untraceable");
    if (hit(40, y, W - 80, 54)) { pick_sel = k; open_vault(found[k]); return; }
  }
  if (in.pressed & SCE_CTRL_DOWN) pick_sel = (pick_sel + 1) % nfound;
  if (in.pressed & SCE_CTRL_UP) pick_sel = (pick_sel + nfound - 1) % nfound;
  if (in.pressed & SCE_CTRL_CROSS) open_vault(found[pick_sel]);
  text(40, H - 24, FAINT, 15, "\u25b3 Google Drive");
  if ((in.pressed & SCE_CTRL_TRIANGLE) || hit(40, H - 48, 200, 40)) { sfx(SFX_OPEN); open_drive(S_PICK); }
}

// ----- an Untraceable vault: the password -----
static void draw_locked(void) {
  float cx = W / 2.0f;
  circle(cx, 170, 48, with_alpha(ACC, 60));
  rrect(cx - 22, 160, 44, 36, 6, ACC);
  circle(cx, 158, 16, ACC);
  circle(cx, 158, 9, BG);
  vita2d_draw_rectangle(cx - 9, 158, 18, 4, ACC);
  text_center(cx, 260, TEXT, 26, "This vault is Untraceable");
  text_center(cx, 292, DIM, 17, "Its files stay encrypted on the card. Enter its password to open it.");
  rrect(cx - 140, 330, 280, 52, 26, ACC);
  text_center(cx, 363, RGBA(20, 12, 16, 255), 20, "Enter the password");
  rrect(cx - 140, 396, 280, 44, 22, SURF);
  text_center(cx, 424, DIM, 17, "Another vault");
#ifdef LV_DEBUG
  // (debug builds only: the emulator's keyboard can't be typed into by a script)
  FILE *dp = fopen(DATA "/debug_password.txt", "r");
  if (dp) {
    if (fgets(password, sizeof password, dp)) { password[strcspn(password, "\r\n")] = 0; screen = S_UNLOCKING; }
    fclose(dp);
    if (screen == S_UNLOCKING) return;
  }
#endif
  if (!ime_open && ((in.pressed & SCE_CTRL_CROSS) || hit(cx - 140, 330, 280, 52))) ime_start(1, "Vault password", "", 1);
  if (!ime_open && ((in.pressed & SCE_CTRL_CIRCLE) || hit(cx - 140, 396, 280, 44))) { find_vaults(); screen = S_PICK; }
}
// ----- the loading screen: at start (while the vault opens behind it) and while a vault unlocks -----
static vita2d_texture *splash_tex;
static unsigned splash_until = 96; // (frames: at least about a second and a half at start)
static void draw_splash(const char *line, int alpha) {
  float t = frame_no / 60.0f;
  vita2d_draw_rectangle(0, 0, W, H, with_alpha(BG, alpha));
  // a slow rose glow, breathing, and a few soft flecks of light drifting up
  float br = 0.85f + 0.15f * sinf(t * 1.6f);
  circle(W / 2.0f, 220, 230 * br, with_alpha(ACC, alpha * 22 / 255));
  circle(W / 2.0f, 220, 150 * br, with_alpha(ACC, alpha * 30 / 255));
  for (int k = 0; k < 14; k++) {
    float x = fmodf(k * 173.0f + 40, W), y = H - fmodf(t * (18 + k * 3) + k * 61.0f, H + 40) + 20;
    circle(x, y, 3 + (k % 4) * 2, with_alpha(ACC2, alpha * (30 + (k % 3) * 15) / 255));
  }
  // the heart springs in, then breathes
  float in_t = fminf(1.0f, frame_no / 36.0f);
  float spring = 1.0f - expf(-6.0f * in_t) * cosf(9.0f * in_t);
  float scale = (0.55f + 0.45f * spring) * (1.0f + 0.025f * sinf(t * 3.2f));
  if (splash_tex) {
    float sz = 200 * scale;
    vita2d_draw_texture_tint_scale(splash_tex, W / 2.0f - sz / 2, 214 - sz / 2, sz / 256.0f, sz / 256.0f, with_alpha(RGBA(255, 255, 255, 255), alpha));
  } else heart(W / 2.0f, 214, 60 * scale, with_alpha(ACC, alpha));
  int ta = (int)(alpha * fminf(1.0f, fmaxf(0.0f, (frame_no - 14) / 24.0f)));
  text_center(W / 2, 372, with_alpha(TEXT, ta), 34, "Lewd Vault");
  text_center(W / 2, 404, with_alpha(DIM, ta), 16, line);
  // a thin bar with light running along it
  float bw = 220, bx = W / 2.0f - bw / 2, by = 440;
  rrect(bx, by, bw, 4, 2, with_alpha(TEXT, ta * 40 / 255));
  float run = fmodf(t * 0.9f, 1.4f) - 0.2f;
  float x0 = bx + bw * fmaxf(0, run), x1 = bx + bw * fminf(1, run + 0.25f);
  if (x1 > x0) rrect(x0, by, x1 - x0, 4, 2, with_alpha(ACC, ta));
}
static void draw_unlocking(int stage) {
  draw_splash(stage ? "Opening the vault\u2026" : "Checking the password\u2026", 255);
}

static int set_sel, plan_sel;
static int wf_return;      // (wifi.c: the screen to go back to,
static char wf_last[160];  //  and the last file, or why it stopped)
// ----- the library -----
static void open_viewer(int k, int slideshow);
static void draw_bar(void) {
  vita2d_draw_rectangle(0, H - BAR, W, BAR, with_alpha(SURF, 245));
  vita2d_draw_line(0, H - BAR, W, H - BAR, with_alpha(TEXT, 20));
  static const char *labels[] = {"Library", "Browser", "Slideshow", "Settings"};
  for (int k = 0; k < 4; k++) {
    int cx = W / 8 + k * W / 4;
    int on = (k == 0 && screen == S_LIBRARY) || (k == 1 && screen == S_WEB) || (k == 2 && screen == S_SHOW) || (k == 3 && screen == S_SETTINGS);
    unsigned c = on ? ACC : DIM;
    int y = H - BAR + 20;
    switch (k) {
      case 0: for (int i = 0; i < 4; i++) rrect(cx - 11 + (i % 2) * 12, y - 10 + (i / 2) * 12, 10, 10, 2, c); break;
      case 1: // a globe
        circle(cx, y, 11, c); circle(cx, y, 9, with_alpha(SURF, 255));
        vita2d_draw_rectangle(cx - 9, y - 1, 18, 2, c); vita2d_draw_rectangle(cx - 1, y - 9, 2, 18, c);
        break;
      case 2: rrect(cx - 13, y - 9, 26, 18, 3, c); vita2d_draw_rectangle(cx - 3, y - 4, 2, 8, BG); vita2d_draw_rectangle(cx + 1, y - 4, 2, 8, BG); break;
      case 3: circle(cx, y, 10, c); circle(cx, y, 4, with_alpha(SURF, 255));
              for (int i = 0; i < 6; i++) circle(cx + cosf(i * 1.047f) * 11, y + sinf(i * 1.047f) * 11, 3, c);
              break;
    }
    text_center(cx, H - 10, c, 13, labels[k]);
    // (the button that leads here: ○ goes round Library, Browser, Slideshow, Settings)
    int next = (screen == S_LIBRARY && k == 1) || (screen == S_WEB && k == 2) || (screen == S_SHOW && k == 3) || (screen == S_SETTINGS && k == 0);
    if (next) { circle(cx + 30, y - 6, 8, with_alpha(TEXT, 40)); text_center(cx + 30, y - 1, DIM, 12, "\u25cb"); }
    if (hit(cx - W / 8, H - BAR, W / 4, BAR)) {
      sfx(SFX_TAB);
      if (k == 0) { screen = S_LIBRARY; if (query[0]) { query[0] = 0; refilter(); } }
      if (k == 1) screen = S_WEB;
      if (k == 2) { screen = S_SHOW; plan_sel = 0; }
      if (k == 3) screen = S_SETTINGS;
    }
  }
}

static int chip(int x, int y, const char *label, int on) {
  int w = text_w(16, label) + 28;
  rrect(x, y, w, 30, 15, on ? ACC : SURF2);
  text(x + 14, y + 21, on ? RGBA(20, 12, 16, 255) : TEXT, 16, label);
  return w;
}

static float chips_scroll;
#include "lib.c"
#include "show.c"
#include "drive_ui.c"
#include "pin.c"
#include "privacy.c"
#include "clean.c"

static int last_top_row = -1;
static void draw_library(void) {
  float content = lgrid.height + 12;
  float view_h = H - TOP - BAR;
  float max_scroll = fmaxf(0, content - view_h);
  // a sheet open: the grid stays drawn but doesn't take the buttons or touch
  Input saved_in = in;
  int sheet_up = lsheet != LS_NONE;
  if (sheet_up) { in.pressed = 0; in.tap = 0; in.touching = 0; in.flicked = 0; in.ly = 0; in.dbl = 0; }
  int open_now = sheet_up ? 0 : lib_cross();
  int fast = sheet_up ? 0 : fast_scroll(max_scroll, view_h);
  if (fast) { in.touching = 0; in.tap = 0; }

  // --- moving: touch drag and flick, the stick, the D-pad selection ---
  if (in.touching && in.ty > TOP && in.ty < H - BAR) { scroll -= in.drag_dy; scroll_v = 0; }
  if (in.flicked && in.down_y > TOP) scroll_v = -in.fvy * 1.6f;
  scroll += scroll_v;
  scroll_v *= 0.94f;
  if (fabsf(scroll_v) < 0.2f) scroll_v = 0;
  scroll += in.ly * 14 - in.bdy * 1.5f;
  int before = sel;
  if (in.pressed & SCE_CTRL_RIGHT) sel++;
  if (in.pressed & SCE_CTRL_LEFT) sel--;
  if (sel >= nshown) sel = nshown - 1;
  if (sel < 0) sel = 0;
  if (in.pressed & SCE_CTRL_DOWN) sel = grid_move(&lgrid, sel, 1);
  if (in.pressed & SCE_CTRL_UP) sel = grid_move(&lgrid, sel, -1);
  if (sel != before && sel >= 0 && sel < lgrid.n) {
    sfx(SFX_TICK);
    Rect r = lgrid.r[sel];
    if (r.y < scroll) scroll += (r.y - scroll) * 0.6f + (r.y < scroll - 1 ? -1 : 0);
    else if (r.y + r.h > scroll + view_h) scroll += (r.y + r.h - view_h + 10 - scroll) * 0.6f;
    if (r.y < scroll) scroll = r.y;
    if (r.y + r.h > scroll + view_h) scroll = r.y + r.h - view_h + 10;
  }
  if (scroll < 0) { scroll = in.touching ? scroll : scroll * 0.7f; if (!in.touching) scroll_v = 0; }
  if (scroll > max_scroll) { scroll = in.touching ? scroll : max_scroll + (scroll - max_scroll) * 0.7f; if (!in.touching) scroll_v = 0; }

  // --- the grid ---
  int first = grid_first(&lgrid, scroll - 8);
  // (a soft tick each time a row of pictures passes the top, as the Vita's own lists do)
  if (first < lgrid.n && lgrid.row[first] != last_top_row) { if (last_top_row >= 0 && (in.touching || scroll_v != 0 || in.ly)) sfx(SFX_TICK); last_top_row = lgrid.row[first]; }
  int wanted[64], nw = 0, last = first;
  for (int k = first; k < nshown && k < lgrid.n; k++) {
    int i = shown[k];
    Rect rc = lgrid.r[k];
    float x = rc.x, y = TOP + 6 + rc.y - scroll, cw = rc.w, ch = rc.h;
    if (y > H) break;
    last = k + 1;
    if (y + ch < TOP - 4) continue;
    LvItem *it = &vault.items[i];
    vita2d_texture *t = thumb_of(i);
    int on = k == sel && !in.touching;
    float grow = on ? 4 : 0;
    if (on) rrect(x - grow - 3, y - grow - 3, cw + 2 * grow + 6, ch + 2 * grow + 6, 12, ACC);
    if (t) draw_cover(t, x - grow, y - grow, cw + 2 * grow, ch + 2 * grow, RGBA(255, 255, 255, 255));
    else {
      rrect(x - grow, y - grow, cw + 2 * grow, ch + 2 * grow, 8, SURF);
      if (tstate[i] == 3) text_center(x + cw / 2, y + ch / 2, FAINT, 14, "No picture");
      else if (nw < 64) wanted[nw++] = i;
    }
    if (is_video(it) || is_gif(it)) {
      char d[24];
      if (is_gif(it)) snprintf(d, sizeof d, "GIF");
      else snprintf(d, sizeof d, "%d:%02d", (int)it->duration / 60, (int)it->duration % 60);
      int tw = text_w(13, d);
      rrect(x + cw - tw - 16, y + ch - 26, tw + 12, 20, 10, RGBA(0, 0, 0, 150));
      text(x + cw - tw - 10, y + ch - 11, TEXT, 13, d);
    }
    if (lv_likes_of(&vault, it) > 0) heart(x + 16, y + ch - 16, 6, ACC);
    if (is_locked(i)) { // (above the level the slideshow session has reached)
      vita2d_draw_rectangle(x, y, cw, ch, RGBA(14, 10, 14, 225));
      rrect(x + cw / 2 - 7, y + ch / 2 - 10, 14, 12, 3, DIM);
      circle(x + cw / 2, y + ch / 2 - 10, 5, DIM);
      circle(x + cw / 2, y + ch / 2 - 10, 3, RGBA(14, 10, 14, 255));
      textf(x + cw / 2 - text_w(13, "Level 10") / 2, y + ch / 2 + 18, DIM, 13, "Level %d", it->nsfw);
    }
    if (!it->path) { rrect(x + 6, y + 6, 20, 20, 10, RGBA(0, 0, 0, 160)); text_center(x + 16, y + 21, ACC2, 14, "!"); }
    if (selecting && picked && picked[i]) { circle(x + cw - 16, y + 16, 11, ACC); text_center((int)(x + cw - 16), (int)y + 22, RGBA(20, 12, 16, 255), 14, "\u2713"); }
    else if (selecting) circle(x + cw - 16, y + 16, 10, with_alpha(TEXT, 90));
    if (in.touching && in.tx >= x && in.tx < x + cw && in.ty >= y && in.ty < y + ch && lib_long_press_card(k)) { sel = k; }
    if (hit((int)x, (int)y, (int)cw, (int)ch) && in.tap_y > TOP && in.tap_y < H - BAR) {
      sel = k;
      if (selecting) { picked[i] ^= 1; npicked += picked[i] ? 1 : -1; sfx(SFX_TOGGLE); }
      else { sfx(SFX_SELECT); open_viewer(k, 0); in = saved_in; return; }
    }
  }
  // (and the next screenful, ready before it's needed)
  for (int k = last; k < last + 18 && k < nshown && nw < 64; k++) if (!thumb_of(shown[k]) && tstate[shown[k]] != 3) wanted[nw++] = shown[k];
  ask_thumbs(wanted, nw);
  if (!nshown) {
    text_center(W / 2, TOP + 150, DIM, 20, query[0] ? "Nothing matches that search" : "Nothing here");
    if (query[0]) text_center(W / 2, TOP + 180, FAINT, 16, "Tap Library below to see everything again");
  }
  // the scroll bar
  if (max_scroll > 0) {
    float bh = fmaxf(30, view_h * view_h / content);
    float by = TOP + (view_h - bh) * fminf(1, fmaxf(0, scroll / max_scroll));
    rrect(W - 6, by, 4, bh, 2, with_alpha(TEXT, 70));
  }

  // --- the top: title, then the category chips ---
  vita2d_draw_rectangle(0, 0, W, TOP, with_alpha(BG, 250));
  text(16, 28, TEXT, 22, "Library");
  char info[96];
  snprintf(info, sizeof info, "%d %s", nshown, nshown == 1 ? "item" : "items");
  text(16 + text_w(22, "Library") + 12, 28, FAINT, 15, info);
  int sx = W - 16;
  char kl[64];
  int nf = (cfg.kind != 0) + (lf.nsfw_max != 0) + (lf.stars_min != 0) + (lf.album != 0) + (lf.coll[0] != 0);
  if (nf) snprintf(kl, sizeof kl, "Filters \u00b7 %d", nf); else snprintf(kl, sizeof kl, "Filters");
  int ws = text_w(15, SORTS[cfg.sort]) + 26, wk = text_w(15, kl) + 26;
  sx -= ws;
  rrect(sx, 8, ws, 26, 13, SURF2);
  text(sx + 13, 26, TEXT, 15, SORTS[cfg.sort]);
  if (hit(sx, 0, ws, 36) || (!selecting && (in.pressed & SCE_CTRL_TRIANGLE))) { sfx(SFX_TOGGLE); cfg.sort = (cfg.sort + 1) % NSORTS; refilter(); cfg_save(); sel = 0; scroll = 0; }
  sx -= wk + 8;
  rrect(sx, 8, wk, 26, 13, nf ? with_alpha(ACC, 90) : SURF2);
  text(sx + 13, 26, TEXT, 15, kl);
  if (hit(sx, 0, wk, 36) || (!selecting && (in.pressed & SCE_CTRL_SQUARE))) { sfx(SFX_OPEN); lsheet = LS_FILTERS; lsheet_sel = 0; }
  if (query[0]) {
    char q[160];
    snprintf(q, sizeof q, "“%s”  ✕", query);
    int wq = text_w(15, q) + 26;
    sx -= wq + 8;
    rrect(sx, 8, wq, 26, 13, with_alpha(ACC, 70));
    text(sx + 13, 26, TEXT, 15, q);
    if (hit(sx, 0, wq, 36)) { query[0] = 0; refilter(); }
  }
  // categories: L / R, or tap
  if (in.pressed & (SCE_CTRL_RTRIGGER | SCE_CTRL_LTRIGGER)) sfx(SFX_TAB);
  if (in.pressed & SCE_CTRL_RTRIGGER) { cat = cat + 1 >= vault.ncats ? -1 : cat + 1; refilter(); sel = 0; scroll = 0; }
  if (in.pressed & SCE_CTRL_LTRIGGER) { cat = cat - 1 < -1 ? vault.ncats - 1 : cat - 1; refilter(); sel = 0; scroll = 0; }
  if (in.touching && in.ty < TOP && in.ty > 36) chips_scroll -= in.drag_dx;
  if (chips_scroll < 0) chips_scroll = 0;
  float cxp = 12 - chips_scroll;
  for (int k = -1; k < vault.ncats; k++) {
    const char *nm = k < 0 ? "All" : vault.cats[k];
    int w = text_w(16, nm) + 28;
    if (cxp + w > 0 && cxp < W) {
      chip((int)cxp, 34, nm, k == cat);
      if (hit((int)cxp, 34, w, 30)) { sfx(SFX_TAB); cat = k; refilter(); sel = 0; scroll = 0; }
    }
    // keep the chosen chip in view
    if (k == cat && (in.pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {
      if (cxp < 0) chips_scroll += cxp - 12;
      if (cxp + w > W) chips_scroll += cxp + w - W + 12;
    }
    cxp += w + 6;
  }

  if (selecting) {
    // choosing many: ✕ (or a tap) picks, △ what to do with them, ○ done
    char t[80];
    snprintf(t, sizeof t, "%d chosen   \u2715 pick   \u25b3 actions   \u25cb done", npicked);
    int w = text_w(15, t) + 30;
    rrect(W / 2 - w / 2, H - BAR - 46, w, 34, 17, with_alpha(ACC, 235));
    text_center(W / 2, H - BAR - 23, RGBA(20, 12, 16, 255), 15, t);
    if (open_now && nshown) { int i = shown[sel]; picked[i] ^= 1; npicked += picked[i] ? 1 : -1; sfx(SFX_TOGGLE); }
    if ((in.pressed & SCE_CTRL_TRIANGLE) && npicked) { sfx(SFX_OPEN); lsheet = LS_BATCH; lsheet_sel = 0; delete_armed = 0; }
    if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_BACK); selecting = 0; if (picked) memset(picked, 0, vault.n + 1); npicked = 0; in.pressed &= ~SCE_CTRL_CIRCLE; }
  } else {
    if (open_now && nshown) { sfx(SFX_SELECT); open_viewer(sel, 0); }
    if ((in.pressed & SCE_CTRL_START) && nshown) { sfx(SFX_SLIDESHOW); open_viewer(sel, 1); }
    if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_TAB); screen = S_WEB; } // (○ goes round the tabs: Library, Browser, Settings)
  }
  draw_bar();
  in = saved_in;
  // (a sheet opened this frame doesn't also take the press that opened it: □ would close it again)
  if (!sheet_up) { in.pressed = 0; in.tap = 0; in.dbl = 0; }
  if (lsheet == LS_FILTERS) draw_filters_sheet();
  else if (lsheet == LS_TAGS) draw_tags_sheet();
  else if (lsheet == LS_QUICK) draw_quick_sheet();
  else if (lsheet == LS_BATCH) draw_batch_sheet();
}

// ----- the full view -----
static int vk;           // position in shown
static int slideshow;
static unsigned slide_at;
static float zoom = 1, pan_x, pan_y, swipe_x;
static int details;      // the details sheet, pulled up like the phone's
static float details_y;  // 0 hidden .. 1 up
static unsigned chrome_until;
static LvVideo *vid;
static int vid_item = -1, vid_clip;
static int64_t ab_a = -1, ab_b = -1; // the A-B loop (ms; -1: not set)
static int64_t resume_to = -1;       // Continue watching: where to go once the video is open
static float mo_x, mo_y;             // the slide's motion: which way it drifts (-1..1)
static unsigned heart_at;            // (a double tap's heart, shown big for a moment)
static unsigned view_started;
static float pinch_d0, zoom0;

// A video is closed at the start of the next frame, not in the middle of one: the frame being put
// together may already draw its picture, which lives in the player's own memory.
static LvVideo *closing[8];
static int nclosing;
static void video_close_later(LvVideo *v) {
  if (!v) return;
  if (nclosing < 8) closing[nclosing++] = v;
  // (eight in one frame can't happen; if it did, a leak is better than a crash)
}
static void videos_close_now(void) {
  if (!nclosing) return;
  vita2d_wait_rendering_done();
  for (int k = 0; k < nclosing; k++) lv_video_close(closing[k]);
  nclosing = 0;
}
static void stop_video(void) {
  if (vid) {
    // Continue watching: a real video left part-way (as the PC: after 20 s; forgotten near the end)
    if (!vid_clip && vid_item >= 0 && vid_item < vault.n && resume_to < 0) {
      uint64_t pos = lv_video_pos(vid), dur = lv_video_duration(vid);
      if (dur > 0 && pos > dur * 95 / 100) resume_set(&vault.items[vid_item], 0);
      else if (pos > 20000) resume_set(&vault.items[vid_item], (int)(pos / 1000));
    }
    video_close_later(vid);
    vid = NULL;
  }
  vid_item = -1;
  ab_a = ab_b = -1;
  resume_to = -1;
}
static void account_time(void) {
  if (view_started && vk < nshown) {
    double secs = (frame_no - view_started) / 60.0;
    lv_add_seconds(&vault, &vault.items[shown[vk]], secs);
  }
  view_started = 0;
}
static void show_item(int k) {
  account_time();
  stop_video();
  if (nshown == 0) return;
  int dir = k < vk ? -1 : 1;
  k = (k % nshown + nshown) % nshown;
  for (int n = 0; n < nshown && is_locked(shown[k]); n++) k = ((k + dir) % nshown + nshown) % nshown;
  if (is_locked(shown[k])) return;
  vk = k;
  LvItem *it = &vault.items[shown[vk]];
  zoom = 1; pan_x = pan_y = 0;
  mo_x = (rnd() % 2001) / 1000.0f - 1; mo_y = (rnd() % 2001) / 1000.0f - 1;
  char now[40];
  now_iso(now, sizeof now);
  lv_viewed(&vault, it, now);
  view_started = frame_no;
  slide_at = frame_no;
  // a video: its own file if the Vita can play it (MP4), else its preview clip; a GIF: its clip
  if (is_video(it) || is_gif(it)) {
    char clip[800];
    int can = is_video(it) && it->path && (ext_is(it->path, "mp4", "m4v", "mov") || ext_is(it->path, "webm", "mkv", NULL));
    if (can) { vid = lv_video_open(it->path, lv_key(&vault), slideshow); vid_clip = 0; }
    if (!vid && lv_clip_path(&vault, it, clip, sizeof clip)) { vid = lv_video_open(clip, lv_key(&vault), 1); vid_clip = 1; }
    if (vid) { vid_item = shown[vk]; lv_video_set_volume(vid, cfg.volume * 32768 / 100); }
    if (vid && !vid_clip && !slideshow) { int t = resume_of(it); if (t > 0) resume_to = (int64_t)t * 1000; }
  }
  ask_big(shown[vk]);
}
static void open_viewer(int k, int show) {
  LOG("open_viewer %d show=%d\n", k, show);
  if (k >= 0 && k < nshown && is_locked(shown[k]) && !show) {
    sfx(SFX_ERROR);
    char m[80];
    snprintf(m, sizeof m, "Locked until the slideshow reaches level %d", vault.items[shown[k]].nsfw);
    say(m);
    return;
  }
  slideshow = show;
  details = 0;
  details_y = 0;
  swipe_x = 0;
  chrome_until = frame_no + 180;
  screen = S_VIEWER;
  scePowerSetArmClockFrequency(444);
  show_item(k);
}
static void close_viewer(void) {
  sfx(SFX_BACK);
  save_edits_now();
  LOG("close_viewer screen=%d\n", screen);
  account_time();
  stop_video();
  resume_save();
  if (lv_vault_save_views(&vault) != 0) LOG("views.csv could not be saved\n");
  screen = S_LIBRARY;
  sel = vk;
  if (sel >= 0 && sel < lgrid.n) {
    Rect r = lgrid.r[sel];
    if (r.y < scroll || r.y + r.h > scroll + H - TOP - BAR) scroll = r.y - 40;
  }
}

static void fmt_time(uint64_t ms, char *o, size_t n) { int s = (int)(ms / 1000); snprintf(o, n, "%d:%02d", s / 60, s % 60); }

static void draw_viewer(void) {
  if (nshown == 0) { screen = S_LIBRARY; return; }
  LvItem *it = &vault.items[shown[vk]];
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 255));
  xfade_wanted = slideshow && so.fade;
  // the thumbnails wanted here: this one (its glow) and the filmstrip's
  {
    int want_t[16], nwt = 0;
    for (int d = 0; d < 11 && nwt < 16; d++) {
      int k = vk + (d % 2 ? (d + 1) / 2 : -(d / 2));
      if (k >= 0 && k < nshown && !thumb_of(shown[k]) && tstate[shown[k]] != 3) want_t[nwt++] = shown[k];
    }
    if (nwt) ask_thumbs(want_t, nwt);
  }
  // --- the glow: the picture's own colours, dim, filling the screen around it ---
  vita2d_texture *th_now = thumb_of(shown[vk]);
  if (cfg.glow && th_now && zoom < 1.01f) draw_cover(th_now, 0, 0, W, H, RGBA(255, 255, 255, 60));
  // --- what's shown: the video's frame, else the picture (the thumbnail while it loads; in a
  //     fading slideshow, the one before until this one is ready) ---
  vita2d_texture *t = NULL;
  int is_frame = 0;
  if (vid) { t = lv_video_frame(vid); is_frame = t != NULL; }
  if (!t) t = big_item == shown[vk] ? big_tex : NULL;
  if (!t && xfade_wanted && big_tex && big_item >= 0 && !vid) t = big_tex;
  if (!t) t = th_now;
  float avail_h = H - details_y * 300;
  // the slide's motion (pictures in a slideshow): a slow pan, a slow zoom, or breathing
  float ms = 1, mdx = 0, mdy = 0;
  if (slideshow && so.motion && !is_frame && t != th_now) {
    float p = (frame_no - slide_at) / (pace_now() * 60.0f);
    p = p < 0 ? 0 : p > 1 ? 1 : p;
    float e = p * p * (3 - 2 * p);
    if (so.motion == 1) { ms = 1.08f; mdx = mo_x * W * 0.03f * (1 - 2 * e); mdy = mo_y * H * 0.02f * (1 - 2 * e); }
    else if (so.motion == 2) { ms = 1 + 0.12f * (1 - (1 - p) * (1 - p)); mdx = mo_x * W * 0.02f * p; mdy = mo_y * H * 0.02f * p; }
    else { ms = 1 + 0.015f * (1 - cosf((frame_no - slide_at) * 6.2832f / 252)); }
  }
  // (the picture before, fading out under this one)
  float fade = 1;
  if (fade_tex) {
    unsigned age = frame_no - fade_at;
    if (age >= 36 || !slideshow) { free_later(fade_tex); fade_tex = NULL; }
    else {
      fade = age / 36.0f;
      float fw = vita2d_texture_get_width(fade_tex), fh = vita2d_texture_get_height(fade_tex);
      float fs = fminf(W / fw, avail_h / fh) * (so.motion == 1 ? 1.08f : 1);
      vita2d_draw_texture_tint_scale(fade_tex, (W - fw * fs) / 2, (avail_h - fh * fs) / 2, fs, fs, RGBA(255, 255, 255, (int)(255 * (1 - fade))));
    }
  }
  if (t) {
    float tw = is_frame ? lv_video_width(vid) : vita2d_texture_get_width(t);
    float th = is_frame ? lv_video_height(vid) : vita2d_texture_get_height(t);
    float s = fminf(W / tw, avail_h / th) * zoom * ms;
    float dw = tw * s, dh = th * s;
    // keep a zoomed picture's edges on screen
    float mx = fmaxf(0, (dw - W) / 2), my = fmaxf(0, (dh - avail_h) / 2);
    pan_x = fminf(mx, fmaxf(-mx, pan_x));
    pan_y = fminf(my, fmaxf(-my, pan_y));
    float x = (W - dw) / 2 + pan_x + swipe_x + mdx, y = (avail_h - dh) / 2 + pan_y + mdy;
    if (fade < 1) vita2d_draw_texture_tint_scale(t, x, y, s, s, RGBA(255, 255, 255, (int)(255 * fade)));
    else vita2d_draw_texture_scale(t, x, y, s, s);
  } else {
    text_center(W / 2, H / 2, DIM, 18, big_failed ? "This file can't be shown on the Vita" : "Loading…");
  }
  if (!it->path) text_center(W / 2, (int)avail_h - 30, ACC2, 16, "This file isn't on the memory card (only its thumbnail)");
  if (vid && !vid_clip && resume_to >= 0 && lv_video_duration(vid) > 0) {
    lv_video_seek(vid, resume_to);
    char a1[16], m[64];
    fmt_time((uint64_t)resume_to, a1, sizeof a1);
    snprintf(m, sizeof m, "Carrying on from %s", a1);
    say(m);
    resume_to = -1;
  }
  if (vid && !vid_clip && ab_a >= 0 && ab_b > ab_a && (int64_t)lv_video_pos(vid) >= ab_b) lv_video_seek(vid, ab_a);
  if (vid && vid_clip && is_video(it)) text_center(W / 2, 30, with_alpha(TEXT, 200), 15, "Preview clip: the Vita can't play this video's own file");

  // --- touch: swipe for the next one, pinch to zoom, drag when zoomed, tap for the controls ---
  if (in.touching && details_y < 0.5f) {
    if (in.tcount >= 2) {
      float d = hypotf(in.t2x - in.tx, in.t2y - in.ty);
      if (pinch_d0 == 0) { pinch_d0 = d; zoom0 = zoom; }
      else zoom = fminf(6, fmaxf(1, zoom0 * d / pinch_d0));
    } else {
      pinch_d0 = 0;
      if (zoom > 1.01f) { pan_x += in.drag_dx; pan_y += in.drag_dy; }
      else if (fabsf((float)(in.tx - in.down_x)) > fabsf((float)(in.ty - in.down_y))) swipe_x += in.drag_dx;
      else if (in.ty - in.down_y < -60) { details = 1; }
    }
  } else {
    pinch_d0 = 0;
    if (swipe_x > 120) { swipe_x = 0; sfx(SFX_TICK); show_item(vk - 1); return; }
    if (swipe_x < -120) { swipe_x = 0; sfx(SFX_TICK); show_item(vk + 1); return; }
    swipe_x *= 0.7f;
    if (zoom < 1.01f) zoom = 1;
  }
  if (in.dbl && cfg.dbltap && details_y < 0.5f) { sfx(SFX_DONE); char now[40]; now_iso(now, sizeof now); lv_liked(&vault, it, now); heart_at = frame_no ? frame_no : 1; }
  else if (in.dbl) { zoom = zoom > 1.5f ? 1 : 2.5f; pan_x = (W / 2.0f - in.tap_x) * (zoom - 1); pan_y = (H / 2.0f - in.tap_y) * (zoom - 1); }
  else if (in.tap && details_y < 0.5f) {
    if (vid && !vid_clip && in.tap_y > H - 70) {
      uint64_t d = lv_video_duration(vid);
      if (d) lv_video_seek(vid, (int64_t)(d * fminf(1, fmaxf(0, (in.tap_x - 40) / (float)(W - 80)))));
    } else if (frame_no < chrome_until && in.tap_x < 70 && in.tap_y < 60) { close_viewer(); return; }
    else if (frame_no < chrome_until && in.tap_x > W - 70 && in.tap_y < 60) { details = 1; }
    else if (frame_no < chrome_until && in.tap_x > W - 140 && in.tap_x < W - 70 && in.tap_y < 60) { char now[40]; now_iso(now, sizeof now); lv_liked(&vault, it, now); }
    else if (frame_no < chrome_until && in.tap_y > H - 90 && !(vid && !vid_clip) && !slideshow && nshown > 1) {} // (the filmstrip's)
    else chrome_until = frame_no < chrome_until ? frame_no : frame_no + 240;
  }

  // (a swipe across the rear touchpad: the next or the one before)
  {
    static float back_sw;
    if (in.back_on) back_sw += in.bdx; else back_sw = 0;
    if (back_sw < -160 || back_sw > 160) { int dir = back_sw < 0 ? 1 : -1; back_sw = 0; sfx(SFX_TICK); show_item(dir > 0 && ss.on && slideshow ? session_pick(vk) : vk + dir); return; }
  }
  // --- buttons ---
  int playing_real = vid && !vid_clip;
  unsigned next_b = details || playing_real ? SCE_CTRL_RTRIGGER : (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER);
  unsigned prev_b = details || playing_real ? SCE_CTRL_LTRIGGER : (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER);
  if ((in.pressed & next_b) && !(in.now & SCE_CTRL_LTRIGGER)) { sfx(SFX_TICK); show_item(ss.on && slideshow ? session_pick(vk) : vk + 1); return; }
  if ((in.pressed & prev_b) && !(in.now & SCE_CTRL_RTRIGGER)) { sfx(SFX_TICK); show_item(vk - 1); return; }
  if (in.pressed & SCE_CTRL_CIRCLE) { if (details) details = 0; else { close_viewer(); return; } }
  if (in.pressed & SCE_CTRL_TRIANGLE) { sfx(details ? SFX_CLOSE : SFX_OPEN); details = !details; }
  if (in.pressed & SCE_CTRL_SQUARE) { sfx(SFX_DONE); char now[40]; now_iso(now, sizeof now); lv_liked(&vault, it, now); chrome_until = frame_no + 120; }
  if (in.pressed & SCE_CTRL_START) { sfx(SFX_SLIDESHOW); slideshow = !slideshow; slide_at = frame_no; say(slideshow ? "Slideshow" : "Slideshow stopped"); }
  if ((in.pressed & SCE_CTRL_CROSS) && !details) {
    if (vid && !vid_clip) lv_video_pause(vid, !lv_video_paused(vid));
    else { zoom = zoom > 1.5f ? 1 : 2.5f; pan_x = pan_y = 0; }
  }
  if (vid && !vid_clip && !details) {
    // ←→ ten seconds; ↑ faster (2×, 4×, back); ↓ the A-B loop (A here, B here, off)
    if (in.pressed & SCE_CTRL_RIGHT) { lv_video_seek(vid, (int64_t)lv_video_pos(vid) + 10000); chrome_until = frame_no + 120; }
    if (in.pressed & SCE_CTRL_LEFT) { lv_video_seek(vid, (int64_t)lv_video_pos(vid) - 10000); chrome_until = frame_no + 120; }
    if (in.pressed & SCE_CTRL_UP) {
      int sp = lv_video_speed(vid) == 100 ? 200 : lv_video_speed(vid) == 200 ? 400 : 100;
      lv_video_set_speed(vid, sp);
      sfx(SFX_TOGGLE);
      say(sp == 100 ? "Normal speed" : sp == 200 ? "2\u00d7 (no sound)" : "4\u00d7 (no sound)");
    }
    if (in.pressed & SCE_CTRL_DOWN) {
      int64_t pos = (int64_t)lv_video_pos(vid);
      sfx(SFX_TOGGLE);
      if (ab_a < 0) { ab_a = pos; say("Loop: A set \u00b7 \u2193 again for B"); }
      else if (ab_b < 0 && pos > ab_a + 500) { ab_b = pos; lv_video_seek(vid, ab_a); say("Looping A to B \u00b7 \u2193 to stop"); }
      else { ab_a = ab_b = -1; say("Loop off"); }
      chrome_until = frame_no + 180;
    }
    // the left stick scrubs: further for a harder push
    if (fabsf(in.lx) > 0.4f && frame_no % 6 == 0) { lv_video_seek(vid, (int64_t)lv_video_pos(vid) + (int64_t)(in.lx * 4000)); chrome_until = frame_no + 120; }
  } else if (zoom > 1.01f) {
    pan_x -= in.lx * 12; pan_y -= in.ly * 12;
  }
  if (in.now & SCE_CTRL_RTRIGGER && in.ly) zoom = fminf(6, fmaxf(1, zoom - in.ly * 0.05f));

  // --- the slideshow moves on by itself: pictures after the interval, videos when they end ---
  if (slideshow) {
    int is_moving = vid && !vid_clip;
    // (in a session, videos and GIFs stay half as long again as a picture, as the PC's 1.5x)
    float secs = ss.on ? pace_now() * (is_moving || vid ? 1.5f : 1) : (float)cfg.interval;
    int over = ss.on ? (is_moving && lv_video_ended(vid)) || frame_no - slide_at > (unsigned)(secs * 60)
                     : (is_moving && lv_video_ended(vid)) || (!is_moving && frame_no - slide_at > (unsigned)(secs * 60));
    if (over) {
      if (ss.on) {
        int n = session_pick(vk);
        if (n < 0) { slideshow = 0; say("Nothing more at this level"); }
        else show_item(n);
      } else if (cfg.shuffle && nshown > 1) { int n; do n = rnd() % nshown; while (n == vk); show_item(n); }
      else show_item(vk + 1);
      return;
    }
    float p = is_moving && !ss.on ? 0 : (frame_no - slide_at) / (secs * 60);
    vita2d_draw_rectangle(0, H - 3, W * fminf(1, p), 3, with_alpha(ACC, 200));
    // the session: how far it's come, a heart at the tip
    if (ss.on && ss.nst) {
      float q = (float)(ss.ms / (ss.nst * level_ms()));
      q = q > 1 ? 1 : q;
      vita2d_draw_rectangle(0, H - 9, W, 2, with_alpha(TEXT, 30));
      vita2d_draw_rectangle(0, H - 9, W * q, 2, with_alpha(ACC, 150));
      heart(fmaxf(8, W * q), H - 9, 5, ACC);
    }
  }
  // (a double tap's heart)
  if (heart_at && frame_no - heart_at < 40) {
    float a = 1 - (frame_no - heart_at) / 40.0f, sz = 20 + (frame_no - heart_at) * 0.6f;
    heart(W / 2, H / 2, sz, with_alpha(ACC, (int)(230 * a)));
  } else heart_at = 0;

  // --- the controls on top, fading away ---
  int chrome = frame_no < chrome_until || details || (vid && !vid_clip && lv_video_paused(vid));
  if (chrome) {
    int a = frame_no + 30 > chrome_until && !details ? (int)((chrome_until - frame_no) * 8) : 240;
    if (a > 240 || a < 0) a = 240;
    for (int k = 0; k < 64; k++) vita2d_draw_rectangle(0, k, W, 1, RGBA(0, 0, 0, (64 - k) * a / 400));
    // back
    vita2d_draw_line(30, 30, 42, 18, with_alpha(TEXT, a));
    vita2d_draw_line(30, 30, 42, 42, with_alpha(TEXT, a));
    vita2d_draw_line(31, 30, 43, 18, with_alpha(TEXT, a));
    vita2d_draw_line(31, 30, 43, 42, with_alpha(TEXT, a));
    char pos[48];
    snprintf(pos, sizeof pos, "%d of %d", vk + 1, nshown);
    text_center(W / 2, 36, with_alpha(TEXT, a), 17, pos);
    // heart and details
    heart(W - 104, 30, 8, lv_likes_of(&vault, it) ? with_alpha(ACC, a) : with_alpha(TEXT, a / 2));
    circle(W - 38, 30, 13, with_alpha(TEXT, a));
    circle(W - 38, 30, 11, RGBA(0, 0, 0, a));
    text_center(W - 38, 36, with_alpha(TEXT, a), 15, "i");
    if (vid && !vid_clip) {
      for (int k = 0; k < 70; k++) vita2d_draw_rectangle(0, H - 70 + k, W, 1, RGBA(0, 0, 0, k * a / 400));
      uint64_t d = lv_video_duration(vid), p = lv_video_pos(vid);
      float f = d ? fminf(1, p / (float)d) : 0;
      rrect(40, H - 34, W - 80, 6, 3, with_alpha(TEXT, a / 3));
      rrect(40, H - 34, (W - 80) * f, 6, 3, with_alpha(ACC, a));
      circle(40 + (W - 80) * f, H - 31, 8, with_alpha(TEXT, a));
      char a1[16], a2[16];
      fmt_time(p, a1, sizeof a1);
      fmt_time(d, a2, sizeof a2);
      text(40, H - 44, with_alpha(TEXT, a), 14, a1);
      text(W - 40 - text_w(14, a2), H - 44, with_alpha(TEXT, a), 14, a2);
      // the loop's ends, the speed, and what the buttons do
      if (d && ab_a >= 0) rrect(40 + (W - 80) * fminf(1, ab_a / (float)d) - 2, H - 40, 4, 18, 2, with_alpha(ACC2, a));
      if (d && ab_b >= 0) rrect(40 + (W - 80) * fminf(1, ab_b / (float)d) - 2, H - 40, 4, 18, 2, with_alpha(ACC2, a));
      char hint[96];
      snprintf(hint, sizeof hint, "%s\u2190\u2192 10 s  \u00b7  \u2191 speed  \u00b7  \u2193 A-B loop  \u00b7  L R next",
               lv_video_speed(vid) == 200 ? "2\u00d7  \u00b7  " : lv_video_speed(vid) == 400 ? "4\u00d7  \u00b7  " : "");
      text_center(W / 2, H - 44, with_alpha(DIM, a), 13, hint);
      if (lv_video_paused(vid)) {
        circle(W / 2, H / 2, 40, RGBA(0, 0, 0, 140));
        for (int k = 0; k < 30; k++) vita2d_draw_rectangle(W / 2 - 10 + k, H / 2 - 15 + k / 2, 1, 30 - k, with_alpha(TEXT, a));
      }
    }
  }

  // --- the filmstrip: the ones either side, to jump to (with the controls; not over a video's bar) ---
  if (chrome && !(vid && !vid_clip) && details_y < 0.05f && nshown > 1 && !slideshow) {
    int a = frame_no + 30 > chrome_until ? (int)((chrome_until - frame_no) * 8) : 240;
    if (a > 240 || a < 0) a = 240;
    for (int k = 0; k < 90; k++) vita2d_draw_rectangle(0, H - 90 + k, W, 1, RGBA(0, 0, 0, k * a / 300));
    for (int d = -6; d <= 6; d++) {
      int k = vk + d;
      if (k < 0 || k >= nshown) continue;
      float cx = W / 2.0f + d * 70, sz = d ? 58 : 66;
      float x = cx - sz / 2, y = H - 14 - sz;
      if (!d) rrect(x - 3, y - 3, sz + 6, sz + 6, 9, with_alpha(ACC, a));
      vita2d_texture *ft = thumb_of(shown[k]);
      if (ft) draw_cover(ft, x, y, sz, sz, RGBA(255, 255, 255, d ? a * 3 / 4 : a));
      else rrect(x, y, sz, sz, 6, with_alpha(SURF2, a));
      if (is_locked(shown[k])) vita2d_draw_rectangle(x, y, sz, sz, RGBA(14, 10, 14, 220));
      if (d && in.tap && in.tap_x >= x && in.tap_x < x + sz && in.tap_y >= y && in.tap_y < y + sz) { sfx(SFX_TICK); show_item(k); return; }
    }
  }

  // --- the details, pulled up from below (as in the phone's full view) ---
  float target = details ? 1 : 0;
  details_y += (target - details_y) * 0.25f;
  if (details_y > 0.01f) {
    float top = H - details_y * 300;
    rrect(0, top, W, 330, 20, with_alpha(SURF, 250));
    rrect(W / 2 - 24, top + 8, 48, 5, 3, with_alpha(TEXT, 60));
    int ii = shown[vk];
    text_fit(30, top + 46, TEXT, 22, it->name[0] ? it->name : it->file, W - 420);
    // editing, as the phone does: the stars, the level dial, the category, the tags, delete
    // (↑↓ choose, ←→ change, ✕ for tags and delete; or touch)
    enum { E_STARS, E_LEVEL, E_CAT, E_TAGS, E_DELETE, E_N };
    static int efocus;
    if (efocus == E_STARS) rrect(W - 400, top + 16, 110, 40, 10, SURF2);
    stars(ii, W - 390, top + 46, efocus == E_STARS);
    if (efocus == E_LEVEL) rrect(W - 190, top + 14, 170, 92, 10, SURF2);
    level_dial(ii, W - 105, top + 92, 62, efocus == E_LEVEL);
    char cl[96];
    snprintf(cl, sizeof cl, "\u2039 %s \u203a", it->category);
    int cw2 = text_w(15, cl) + 24;
    rrect(W - 400, top + 62, cw2, 28, 14, efocus == E_CAT ? ACC : SURF2);
    text(W - 388, top + 82, efocus == E_CAT ? RGBA(20, 12, 16, 255) : TEXT, 15, cl);
    if (hit(W - 400, (int)top + 62, cw2 / 2, 28)) { sfx(SFX_TOGGLE); set_category(ii, -1); }
    else if (hit(W - 400 + cw2 / 2, (int)top + 62, cw2 / 2, 28)) { sfx(SFX_TOGGLE); set_category(ii, 1); }
    rrect(W - 400, top + 96, 100, 28, 14, efocus == E_TAGS ? ACC : SURF2);
    text_center(W - 350, (int)top + 116, efocus == E_TAGS ? RGBA(20, 12, 16, 255) : TEXT, 14, "Edit tags");
    rrect(W - 292, top + 96, 90, 28, 14, efocus == E_DELETE ? RGBA(220, 70, 80, 255) : SURF2);
    text_center(W - 247, (int)top + 116, TEXT, 14, delete_armed ? "Again" : "Delete");
    int act = 0;
    if (hit(W - 400, (int)top + 96, 100, 28)) { efocus = E_TAGS; act = 1; }
    if (hit(W - 292, (int)top + 96, 90, 28)) { efocus = E_DELETE; act = 1; }
    if (details) {
      if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
      if (in.pressed & SCE_CTRL_DOWN) efocus = (efocus + 1) % E_N;
      if (in.pressed & SCE_CTRL_UP) efocus = (efocus + E_N - 1) % E_N;
      int d = (in.pressed & SCE_CTRL_RIGHT) ? 1 : (in.pressed & SCE_CTRL_LEFT) ? -1 : 0;
      if (d && efocus == E_STARS) { sfx(SFX_TOGGLE); int q = it->quality + d; set_int(ii, "quality", q < 1 ? 1 : q > 3 ? 3 : q); }
      if (d && efocus == E_LEVEL) { sfx(SFX_TICK); int l = it->nsfw + d; set_int(ii, "nsfw", l < 1 ? 1 : l > 10 ? 10 : l); }
      if (d && efocus == E_CAT) { sfx(SFX_TOGGLE); set_category(ii, d); }
      if ((in.pressed & SCE_CTRL_CROSS) || act) {
        if (efocus == E_TAGS && !ime_open) ime_start(4, "Tags (separated by spaces)", it->tags, 0);
        else if (efocus == E_DELETE && delete_confirmed()) {
          save_edits_now();
          int k = vk;
          stop_video();
          details = 0;
          if (delete_item(ii) == 0) {
            if (!nshown) { screen = S_LIBRARY; return; }
            show_item(k < nshown ? k : nshown - 1);
          }
          return;
        }
      }
    }
    char line[256];
    snprintf(line, sizeof line, "%s  ·  %s%s", it->category, is_video(it) ? "Video" : is_gif(it) ? "GIF" : "Picture", it->nsfw ? "  ·  NSFW" : "");
    text_fit(30, top + 78, DIM, 16, line, W - 60);
    if (it->width) {
      if (is_video(it)) snprintf(line, sizeof line, "%d × %d  ·  %d:%02d", it->width, it->height, (int)it->duration / 60, (int)it->duration % 60);
      else snprintf(line, sizeof line, "%d × %d", it->width, it->height);
      text(30, top + 104, FAINT, 15, line);
    }
    snprintf(line, sizeof line, "Seen %lld %s  ·  %lld %s  ·  added %.10s", lv_views_of(&vault, it), lv_views_of(&vault, it) == 1 ? "time" : "times",
             lv_likes_of(&vault, it), lv_likes_of(&vault, it) == 1 ? "heart" : "hearts", it->added);
    text(30, top + 130, FAINT, 15, line);
    if (it->collection[0]) { snprintf(line, sizeof line, "In “%s”", it->collection); text_fit(30, top + 156, DIM, 15, line, W - 60); }
    // tags as chips
    float tx = 30, ty = top + 172;
    char tags[1024];
    snprintf(tags, sizeof tags, "%s", it->tags);
    for (char *s = strtok(tags, " "); s; s = strtok(NULL, " ")) {
      int w = text_w(14, s) + 22;
      if (tx + w > W - 30) { tx = 30; ty += 32; }
      if (ty > H - 30) break;
      rrect(tx, ty, w, 26, 13, SURF2);
      text(tx + 11, ty + 18, TEXT, 14, s);
      if (hit((int)tx, (int)ty, w, 26)) {
        snprintf(query, sizeof query, "%s", s);
        close_viewer();
        cat = -1;
        refilter();
        sel = 0; scroll = 0;
        return;
      }
      tx += w + 6;
    }
    if (it->source[0] && ty < H - 40) text_fit(30, ty + 52, FAINT, 13, it->source, W - 60);
    if (in.touching && in.down_y > top && in.ty - in.down_y > 50) details = 0;
  }
}

// ----- settings -----
static float set_scroll;
static void draw_settings(void) {
  char v3[64];
  snprintf(v3, sizeof v3, "%d%%", cfg.volume);
  char v4[16];
  if (cfg.sounds) snprintf(v4, sizeof v4, "%d%%", cfg.sounds); else snprintf(v4, sizeof v4, "Off");
  char vd[96] = "Off";
  if (pin.decoy_hash[0] && !decoy_session) { const char *nm = strrchr(pin.decoy_vault, '/'); snprintf(vd, sizeof vd, "→ %s", pin.decoy_vault[0] ? (nm ? nm + 1 : pin.decoy_vault) : "the vault list"); }
  enum { ST_SEARCH, ST_VOL, ST_SOUNDS, ST_DBL, ST_GLOW, ST_DRIVE, ST_WIFI, ST_PIN, ST_DECOY, ST_COVER, ST_TLOCK, ST_STATS, ST_CLEAN, ST_HEALTH, ST_VAULT, ST_CACHE, ST_N };
  struct { const char *name, *value, *hint; } rows[ST_N] = {
    {"Search", query[0] ? query : "", "Tags, names, categories and collections"},
    {"Video volume", v3, "← → to change"},
    {"Interface sounds", v4, "The Vita's own clicks and chimes"},
    {"Double tap in the full view", cfg.dbltap ? "Heart" : "Zoom", "Zooms in, or gives it a heart"},
    {"Glow behind pictures", cfg.glow ? "On" : "Off", "The picture's own colours fill the screen around it"},
    {"Google Drive", drive_signed_in() ? "Signed in" : "", "Keep this vault in step with your PC's, both ways"},
    {"Wi-Fi transfer", "", "Send pictures and videos from a phone or PC on the same Wi-Fi"},
    {"Lock with a PIN", !pin.on || decoy_session ? "Off" : pin.on_wake ? "On, and when it wakes" : "On, when it starts", pin.on ? "✕ a new PIN · ←→ when · □ turns it off" : "A PIN pad before anything shows"},
    {"Decoy PIN", vd, pin.on ? "Another PIN that opens a harmless vault (←→ which, □ off)" : "Needs the PIN lock on"},
    {"SELECT hides the app behind", cfg.hide ? "A calculator" : "A clock", cfg.hide ? "It calculates; your PIN and = gets back in (no PIN: hold the display)" : "A plain clock; SELECT again comes back"},
    {"Touch lock", TL_NAMES[cfg.tlock & 3], "Hold L and R for a second: touch and buttons do nothing (again to unlock)"},
    {"Your stats", "", "What's here and how it's been looked at"},
    {"Clean up", "", "One at a time: swipe away what you don't want any more"},
    {"Health check", "", "What's wrong in the vault, and what can be put right"},
    {"Open another vault", "", "The vaults on the memory card"},
    {"Clear the thumbnail cache", "", "They're made again as you scroll"},
  };
  int n = ST_N, rh = 47, top = 58, room = H - BAR - top - 4;
  // (the chosen row kept in sight)
  float want = set_sel * rh;
  if (want + rh - set_scroll > room) set_scroll = want + rh - room;
  if (want < set_scroll) set_scroll = want;
  for (int k = 0; k < n; k++) {
    int y = top + k * rh - (int)set_scroll;
    if (y + rh < top - 10 || y > H - BAR) continue;
    rrect(30, y, W - 60, 43, 12, k == set_sel ? SURF2 : SURF);
    if (k == set_sel) rrect(30, y, 5, 43, 3, ACC);
    text(52, y + 19, TEXT, 17, rows[k].name);
    text(52, y + 37, FAINT, 13, rows[k].hint);
    if (rows[k].value[0]) text(W - 52 - text_w(17, rows[k].value), y + 28, ACC, 17, rows[k].value);
    if (hit(30, y, W - 60, 43) && in.tap_y > top && in.tap_y < H - BAR) { set_sel = k; in.pressed |= SCE_CTRL_CROSS; }
  }
  vita2d_draw_rectangle(0, 0, W, top - 4, BG);
  text(30, 44, TEXT, 26, "Settings");
  textf(30 + text_w(26, "Settings") + 16, 44, FAINT, 13, "%s  ·  %d items%s", vault.root, vault.n, vault.untraceable ? "  ·  Untraceable" : "");
  if (in.touching && in.down_y > top) set_scroll -= in.drag_dy;
  if (set_scroll < 0) set_scroll = 0;
  if (set_scroll > n * rh - room) set_scroll = n * rh - room > 0 ? n * rh - room : 0;
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) set_sel = (set_sel + 1) % n;
  if (in.pressed & SCE_CTRL_UP) set_sel = (set_sel + n - 1) % n;
  int d = (in.pressed & SCE_CTRL_RIGHT) ? 1 : (in.pressed & SCE_CTRL_LEFT) ? -1 : 0;
  int cross = (in.pressed & SCE_CTRL_CROSS) != 0, square = (in.pressed & SCE_CTRL_SQUARE) != 0;
  if (cross) d = d ? d : 1;
  if (decoy_session && (set_sel == ST_PIN || set_sel == ST_DECOY) && (d || square || cross)) { sfx(SFX_ERROR); say("Not available"); d = square = cross = 0; }
  if (square && set_sel == ST_PIN && pin.on) { sfx(SFX_CLOSE); pin.on = 0; pin.decoy_hash[0] = 0; pin_save(); say("PIN lock off"); }
  if (square && set_sel == ST_DECOY && pin.decoy_hash[0]) { sfx(SFX_CLOSE); pin.decoy_hash[0] = 0; pin_save(); say("Decoy PIN off"); }
  if (d) {
    switch (set_sel) {
      case ST_SEARCH: if (cross && !ime_open) ime_start(2, "Search tags, names, categories", query, 0); break;
      case ST_VOL: cfg.volume += d * 10; if (cfg.volume > 100) cfg.volume = 0; if (cfg.volume < 0) cfg.volume = 100; break;
      case ST_DBL: cfg.dbltap = !cfg.dbltap; sfx(SFX_TOGGLE); break;
      case ST_GLOW: cfg.glow = !cfg.glow; sfx(SFX_TOGGLE); break;
      case ST_SOUNDS: cfg.sounds += d * 10; if (cfg.sounds > 100) cfg.sounds = 0; if (cfg.sounds < 0) cfg.sounds = 100; sfx_volume(cfg.sounds); sfx(SFX_TOGGLE); break;
      case ST_DRIVE: if (cross) { sfx(SFX_OPEN); open_drive(S_SETTINGS); return; } break;
      case ST_PIN:
        if (cross && !ime_open) { ime_number = 1; ime_start(12, "A new PIN (4 to 6 digits)", "", 1); }
        else if (!cross && pin.on) { sfx(SFX_TOGGLE); pin.on_wake = !pin.on_wake; pin_save(); }
        break;
      case ST_DECOY:
        if (!pin.on) { sfx(SFX_ERROR); say("Turn the PIN lock on first"); break; }
        if (cross && !ime_open) { ime_number = 1; ime_start(13, "The decoy PIN (not the real one)", "", 1); }
        else if (!cross) {
          // which vault it opens: the others on the card, in turn
          find_vaults();
          int cur = -1;
          for (int k = 0; k < nfound; k++) if (!strcmp(found[k], pin.decoy_vault)) cur = k;
          for (int t = 0; t < nfound; t++) {
            cur = (cur + d + nfound) % nfound;
            if (strcmp(found[cur], vault.root)) break;
          }
          if (nfound && strcmp(found[cur], vault.root)) { snprintf(pin.decoy_vault, sizeof pin.decoy_vault, "%s", found[cur]); pin_save(); sfx(SFX_TOGGLE); }
          else say("Put a second vault on the card for the decoy to open");
        }
        break;
      case ST_STATS: if (cross) { sfx(SFX_OPEN); screen = S_STATS; return; } break;
      case ST_WIFI: if (cross) { sfx(SFX_OPEN); wf_return = S_SETTINGS; wf_last[0] = 0; screen = S_WIFI; return; } break;
      case ST_VAULT: if (cross) { lv_vault_save_views(&vault); resume_save(); find_vaults(); screen = S_PICK; return; } break;
      case ST_CACHE:
        if (cross) {
          DIR *dd = opendir(cache_dir);
          struct dirent *e;
          char p[900];
          while (dd && (e = readdir(dd))) { if (e->d_name[0] == '.') continue; snprintf(p, sizeof p, "%s/%s", cache_dir, e->d_name); sceIoRemove(p); }
          if (dd) closedir(dd);
          say("Thumbnail cache cleared");
        }
        break;
      case ST_COVER: cfg.hide = !cfg.hide; sfx(SFX_TOGGLE); break;
      case ST_TLOCK: cfg.tlock = (cfg.tlock + d + 4) & 3; sfx(SFX_TOGGLE); break;
      case ST_CLEAN: if (cross) { sfx(SFX_OPEN); clean_open(); return; } break;
      case ST_HEALTH: if (cross) { sfx(SFX_OPEN); hc.done = 0; screen = S_HEALTH; return; } break;
    }
    cfg_save();
  }
  if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_TAB); screen = S_LIBRARY; }
  draw_bar();
}

// ----- the cover: a plain clock, nothing of the vault (SELECT, from anywhere) -----
static void draw_cover_screen(void) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(10, 12, 16, 255));
  time_t t = time(NULL);
  struct tm l;
  lv_local_tm(t, &l);
  char b[16];
  strftime(b, sizeof b, "%H:%M", &l);
  text_center(W / 2, H / 2 + 20, RGBA(220, 225, 235, 255), 72, b);
  strftime(b, sizeof b, "%A", &l);
  text_center(W / 2, H / 2 + 64, RGBA(120, 128, 140, 255), 20, b);
}

// The Vita going to sleep and waking: afterwards the network's connections and the video player's
// open file may be dead, so the videos close and the connections start afresh.
static volatile int woke;
static int power_cb(int notify_id, int count, int arg, void *user) {
  if (arg & (SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_APP_RESUME)) woke = 1;
  return 0;
}
static int power_main(SceSize args, void *argp) {
  SceUID cb = sceKernelCreateCallback("lv_power", 0, power_cb, NULL);
  scePowerRegisterCallback(cb);
  for (;;) sceKernelDelayThreadCB(1000000);
  return 0;
}

// The vault got an item (a save from the Browser): the Library's per-item arrays grow with it.
static void library_grew(int before) {
  pthread_mutex_lock(&mx);
  signed char *t = realloc(tstate, vault.n + 1);
  short *so = realloc(slot_of, (vault.n + 1) * sizeof *so);
  if (t) tstate = t;
  if (so) slot_of = so;
  if (t && so) for (int i = before; i <= vault.n; i++) { tstate[i] = 0; slot_of[i] = -1; }
  pthread_mutex_unlock(&mx);
  if (picked) { unsigned char *pk = realloc(picked, vault.n + 64); if (pk) { picked = pk; memset(picked + before, 0, vault.n + 64 - before); } }
  refilter();
}

#include "web.c"
#include "wifi.c"

int main(void) {
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir(DATA, 0777);
  sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
  SceAppUtilInitParam ip;
  SceAppUtilBootParam bp;
  memset(&ip, 0, sizeof ip);
  memset(&bp, 0, sizeof bp);
  sceAppUtilInit(&ip, &bp);
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  // (a bigger per-frame vertex pool than vita2d's 1 MB: room to spare on the busiest screen)
  vita2d_init_advanced(4 * 1024 * 1024);
  make_disc();
  splash_tex = vita2d_load_PNG_file("app0:splash.png");
  sfx_init();
  sfx_volume(70);
  lv_video_release_texture = free_later;
  vita2d_set_clear_color(BG);
  vita2d_set_vblank_wait(1);
  font = vita2d_load_font_file("app0:font.ttf");
  pgf = vita2d_load_default_pgf();
  rnd_state ^= (unsigned)time(NULL);
  cfg_load();
  show_load();
  drive_load();
  sfx_volume(cfg.sounds);
  sfx(SFX_START);

  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 256 * 1024);
  pthread_create(&th, &at, worker, NULL);
  SceUID pt = sceKernelCreateThread("lv_power", power_main, 0x10000100, 0x10000, 0, 0, NULL);
  if (pt >= 0) sceKernelStartThread(pt, 0, NULL);
  atlas_init();

  find_vaults();
  pin_load();
  if (pin.on) pin_lock(-1);
  else if (cfg.vault[0] && lv_is_vault(cfg.vault)) open_vault(cfg.vault);
  else if (nfound == 1) open_vault(found[0]);

  unsigned idle_save = 0;
  for (;;) {
    frame_no++;
    read_input();
    if (frame_no < splash_until) { in.pressed = 0; in.tap = 0; in.touching = 0; } // (nothing happens under the loading screen)
    // touch lock: nothing gets through but SELECT (the disguise still works)
    if (screen != S_PIN && screen != S_CALC && touch_lock_filter()) {
      in.pressed &= SCE_CTRL_SELECT;
      in.now = 0;
      in.tap = in.dbl = in.touching = in.flicked = 0;
      in.lx = in.ly = in.bdx = in.bdy = in.drag_dx = in.drag_dy = 0;
      in.back_on = 0;
    }
    if (npending) { vita2d_wait_rendering_done(); for (int k = 0; k < npending; k++) vita2d_free_texture(pending_free[k]); npending = 0; }
    videos_close_now();
    if (woke) {
      woke = 0;
      lv_net_reset();
      if (vid) { stop_video(); say("Paused while the Vita slept"); }
      if (wv_vid) { video_close_later(wv_vid); wv_vid = NULL; }
      if (pin.on && pin.on_wake && screen != S_PIN) { slideshow = 0; pin_lock(screen == S_COVER ? cover_return : screen); }
    }
    // (a video playing, or a slideshow: the Vita doesn't dim or sleep)
    if ((vid && !lv_video_paused(vid)) || (wv_vid && !lv_video_paused(wv_vid)) || (screen == S_VIEWER && slideshow) || drive.state == DRV_SYNCING || drive.state == DRV_CODE || wf_run)
      sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    if (loaded_vault) take_results();
    if (web_started) web_take_results();
    if (loaded_vault) wf_take();
    if (wf_run && screen != S_WIFI && screen != S_COVER) wf_stop(); // (the server only while its screen is open)

    // the keyboard's answer
    if (ime_open) {
      char got[120];
      int r = ime_poll(got, sizeof got);
      if (r == 1 && ime_for == 1) { snprintf(password, sizeof password, "%s", got); load_state = 0; screen = S_UNLOCKING; }
      if (r == 1 && ime_for == 3) { char clean[128]; snprintf(cfg.wquery, sizeof cfg.wquery, "%s", lv_booru_clean(got, clean, sizeof clean)); sheet = SH_NONE; screen = S_WEB; web_search(); }
      if (r == 1 && ime_for == 4 && screen == S_VIEWER && vk < nshown) { if (lv_item_set(&vault, shown[vk], "tags", got) == 0) { edited(); sfx(SFX_DONE); } }
      if (r == 1 && ime_for == 5 && got[0]) saved_add_current(got);
      if (r == 1 && ime_for == 6) batch_add_tag(got);
      if (r == 1 && ime_for == 7 && got[0]) { snprintf(drive_new_id, sizeof drive_new_id, "%s", got); drive_ask_secret = 1; }
      if (r == 1 && ime_for == 9) { snprintf(whidden, sizeof whidden, "%s", got); hidden_save(); web_search(); }
      if (r == 1 && ime_for == 10 && got[0]) { snprintf(wkey_user, sizeof wkey_user, "%s", got); wkey_ask = 1; }
      if (r == 1 && ime_for == 11 && got[0]) { snprintf(wkeys[cfg.site], sizeof wkeys[cfg.site], "%s %s", wkey_user, got); wkeys_save(); sfx(SFX_DONE); web_search(); }
      if (r == 1 && (ime_for == 12 || ime_for == 13)) {
        int ok = strlen(got) >= 4 && strlen(got) <= 6 && strspn(got, "0123456789") == strlen(got);
        if (!ok) { sfx(SFX_ERROR); say("A PIN is 4 to 6 digits"); }
        else { pin_set(got, ime_for == 13); sfx(SFX_DONE); say(ime_for == 12 ? "PIN lock on" : "Decoy PIN set: \u2190\u2192 on its row picks the vault it opens"); }
        memset(got, 0, sizeof got);
      }
      if (r == 1 && ime_for == 8 && got[0]) { drive_set_key(drive_new_id, got); sfx(SFX_DONE); say("Key saved: now sign in"); }
      if (r == 1 && ime_for == 2) { lsheet = LS_NONE; snprintf(query, sizeof query, "%s", got); cat = -1; refilter(); sel = 0; scroll = 0; screen = S_LIBRARY; }
      if (r != 0) memset(&in, 0, sizeof in);
      else in.pressed = 0, in.tap = 0;
    }
    // SELECT: the cover, from anywhere (and back)
    if (in.pressed & SCE_CTRL_SELECT) {
      if (screen == S_COVER) screen = cover_return;
      else if (screen != S_UNLOCKING && screen != S_CALC) {
        if (vid) lv_video_pause(vid, 1);
        if (wv_vid) lv_video_pause(wv_vid, 1);
        slideshow = 0;
        tl_on = 0;
        if (cfg.hide) calc_open(screen);
        else { cover_return = screen; screen = S_COVER; }
      }
      in.pressed = 0;
    }

    vita2d_start_drawing();
    vita2d_clear_screen();
    switch (screen) {
      case S_PICK: draw_pick(); break;
      case S_LOCKED: draw_locked(); break;
      case S_UNLOCKING:
        draw_unlocking(!vault.untraceable || vault.has_key);
        break;
      case S_LIBRARY: draw_library(); break;
      case S_VIEWER: draw_viewer(); break;
      case S_SETTINGS: draw_settings(); break;
      case S_WEB: draw_web(); break;
      case S_SHOW: draw_show_plan(); break;
      case S_DRIVE: draw_drive(); break;
      case S_PIN: draw_pin(); break;
      case S_STATS: draw_stats(); break;
      case S_WIFI: draw_wifi(); break;
      case S_CALC: draw_calc(); break;
      case S_CLEAN: draw_clean(); break;
      case S_HEALTH: draw_health(); break;
      case S_WEBVIEW: draw_webview(); break;
      case S_COVER: draw_cover_screen(); if (in.tap) { screen = cover_return; } break;
    }
    // the loading screen at start, fading into whatever is behind it once the vault is open
    if (frame_no < splash_until + 24) {
      if (frame_no >= splash_until && (screen == S_UNLOCKING || load_state == 1)) splash_until = frame_no + 1; // (still opening)
      int a = frame_no < splash_until ? 255 : (int)(255 * (splash_until + 24 - frame_no) / 24.0f);
      if (screen != S_LOCKED || frame_no < splash_until) draw_splash(screen == S_LOCKED ? "Untraceable vault" : "Opening the vault\u2026", a);
    }
    if (frame_no < message_until && screen != S_COVER && screen != S_CALC) {
      int w = text_w(16, message) + 40;
      int a = message_until - frame_no < 20 ? (message_until - frame_no) * 12 : 240;
      rrect(W / 2 - w / 2, H - BAR - 56, w, 38, 19, RGBA(0, 0, 0, a * 3 / 4));
      text_center(W / 2, H - BAR - 31, with_alpha(TEXT, a), 16, message);
    }
    if (screen != S_CALC && screen != S_COVER) draw_touch_lock();
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();

    // the vault opens on its own thread (start_load); its end is picked up here
    if (screen == S_UNLOCKING) {
      if (load_state == 0) start_load(vault.untraceable && !vault.has_key);
      else if (load_state == 4 || load_state == 5) {
        say(load_state == 4 ? "That password isn't right" : "The vault's password check is missing");
        load_state = 0;
        screen = S_LOCKED;
      } else if (load_state == 2 || load_state == 3) finish_open();
    }
    if (edits_at && frame_no - edits_at > 150 && lsheet == LS_NONE) save_edits_now();
    // the slideshow session's clock, and the sleep timer
    if (loaded_vault && show_tick(screen == S_VIEWER && slideshow)) {
      slideshow = 0;
      if (vid) lv_video_pause(vid, 1);
      sfx(SFX_CLOSE);
      say("Sleep timer: the slideshow has stopped");
    }
    static unsigned cfg_tick;
    if ((screen == S_WEB || screen == S_WEBVIEW) && ++cfg_tick % 600 == 0) cfg_save();
    // views reach the card now and then while looking (and always when leaving the full view)
    if (loaded_vault && vault.views_dirty && ++idle_save > 60 * 30) { lv_vault_save_views(&vault); idle_save = 0; }
  }
  return 0;
}
