// Lewd Vault for the PS Vita: the phone app's library, full view and slideshow, native (vita2d), on
// the same vault folder as the PC and the phones (vault.c reads it the way src-tauri does).
//
// The vault is copied onto the memory card (USB or VitaShell's FTP) into ux0:data/LewdVault/; an
// Untraceable vault stays encrypted there and opens with its password. Views, hearts and time go
// into its views.csv, like on the PC, so copying the folder back carries them home.
//
// Touch works as on the phone (tap, drag, swipe, pinch); the buttons do it all too:
//   Library  D-pad/stick move · ✕ open · L/R category · △ sort · □ kind · START slideshow · SELECT cover
//   Viewer   ←/→ or L/R previous/next · ✕ play/pause or zoom · △ details · □ heart · ○ back
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
#include "lvcrypt.h"
#include "vault.h"
#include "video.h"
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
#define SLOTS 110

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

// ---------- the app's own settings (ux0:data/LWDV00001/config.txt) ----------
static struct {
  char vault[512];
  int sort, kind, interval, shuffle, volume;
  int site, ratings, wsort; // the Browser's site, ratings (LV_R_* bits) and order
  char wquery[128], save_cat[64];
} cfg = {"", 0, 0, 5, 0, 80, 0, 15, 0, "", ""};
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
  }
  fclose(f);
  if (cfg.interval < 2 || cfg.interval > 60) cfg.interval = 5;
  if (cfg.volume < 0 || cfg.volume > 100) cfg.volume = 80;
  if (cfg.site < 0 || cfg.site >= 64) cfg.site = 0;
  if (!(cfg.ratings & 15)) cfg.ratings = 15;
  if (cfg.wsort < 0 || cfg.wsort > 3) cfg.wsort = 0;
}
static void cfg_save(void) {
  FILE *f = fopen(DATA "/config.txt.part", "w");
  if (!f) return;
  fprintf(f, "vault=%s\nsort=%d\nkind=%d\ninterval=%d\nshuffle=%d\nvolume=%d\nsite=%d\nratings=%d\nwsort=%d\nwquery=%s\nsave_cat=%s\n",
          cfg.vault, cfg.sort, cfg.kind, cfg.interval, cfg.shuffle, cfg.volume, cfg.site, cfg.ratings, cfg.wsort, cfg.wquery, cfg.save_cat);
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
static const char *SORTS[] = {"Newest", "Oldest", "Most viewed", "Top rated", "Most hearted", "Shuffle"};
static const char *KINDS[] = {"Everything", "Pictures", "Videos"};

static int is_video(const LvItem *it) { return !strcmp(it->kind, "video"); }
static int is_gif(const LvItem *it) { return !strcmp(it->kind, "gif"); }
static int ext_is(const char *path, const char *e1, const char *e2, const char *e3) {
  const char *d = path ? strrchr(path, '.') : NULL;
  if (!d) return 0;
  return !strcasecmp(d + 1, e1) || (e2 && !strcasecmp(d + 1, e2)) || (e3 && !strcasecmp(d + 1, e3));
}
static int contains_ci(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  if (!n) return 1;
  for (; *hay; hay++)
    if (!strncasecmp(hay, needle, n)) return 1;
  return 0;
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
static void refilter(void) {
  free(shown);
  shown = malloc((vault.n + 1) * sizeof *shown);
  nshown = 0;
  for (int i = 0; i < vault.n; i++) {
    const LvHot *h = &vault.hot[i];
    if (cat >= 0 && h->cat != cat) continue;
    if (cfg.kind == 1 && h->kind == LV_VIDEO) continue;
    if (cfg.kind == 2 && h->kind == LV_IMAGE) continue;
    LvItem *it = &vault.items[i];
    if (query[0] && !contains_ci(it->tags, query) && !contains_ci(it->name, query) && !contains_ci(it->category, query) &&
        !contains_ci(it->collection, query))
      continue;
    shown[nshown++] = i;
  }
  sort_mode = cfg.sort;
  if (cfg.sort == 5) {
    for (int k = nshown - 1; k > 0; k--) { int j = rnd() % (k + 1), t = shown[k]; shown[k] = shown[j]; shown[j] = t; }
  } else {
    qsort(shown, nshown, sizeof *shown, cmp_items);
  }
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
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int want[64], nwant;
static struct { int item, gen, max_w, max_h; } big_ask = {-1, 0, 0, 0};
typedef struct { int item, big, gen; LvPixels px; } Done;
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

// The size to decode a picture at for a card: just big enough to cover it (a landscape picture as
// tall as the card, a portrait one as wide), never more.
static void cover_size(const uint8_t *b, size_t n, int *mw, int *mh) {
  int w = 0, h = 0;
  *mw = CARD_W; *mh = CARD_H * 2;
  if (!lv_image_size(b, n, &w, &h) || w <= 0 || h <= 0) return;
  if (w * CARD_H >= h * CARD_W) { *mh = CARD_H; *mw = (int)((long long)CARD_H * w / h) + 1; if (*mw > CARD_W * 4) *mw = CARD_W * 4; }
  else { *mw = CARD_W; *mh = (int)((long long)CARD_W * h / w) + 1; if (*mh > CARD_H * 3) *mh = CARD_H * 3; }
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
  snprintf(cp, sizeof cp, "%s/%08lx%08lx.jpg", cache_dir, fnv(it->thumb), fnv(it->id));
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
  int sw = 0, shh = 0, small = lv_image_size(b, n, &sw, &shh) && sw <= CARD_W * 4 && shh <= CARD_H * 3 && (sw <= CARD_W * 3 / 2 || shh <= CARD_H * 3 / 2);
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
    pthread_mutex_unlock(&mx);
    if (full) { sceKernelDelayThread(4000); if (!big) { pthread_mutex_lock(&mx); tstate[item] = 0; pthread_mutex_unlock(&mx); } continue; }
    LvPixels px;
    int r = -1;
    if (big) {
      LvItem *it = &vault.items[item];
      size_t n = 0;
      uint8_t *b = it->path ? lv_read_all(it->path, lv_key(&vault), &n) : NULL;
      if (b) { r = lv_decode(b, n, mw, mh, &px); free(b); }
      if (r != 0) { b = lv_thumb_bytes(&vault, it, &n); if (b) { r = lv_decode(b, n, mw, mh, &px); free(b); } }
    } else {
      r = thumb_pixels(item, &px);
    }
    pthread_mutex_lock(&mx);
    if (r == 0) done[ndone++] = (Done){item, big, gen, px};
    else if (!big) tstate[item] = 3;
    else done[ndone++] = (Done){item, big, gen, {NULL, 0, 0}};
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

static vita2d_texture *big_tex;
static int big_item = -1, big_gen, big_failed;

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
    if (d->big) {
      if (d->gen == big_gen) {
        free_later(big_tex);
        big_tex = d->px.rgba ? upload(&d->px) : NULL;
        big_item = d->item;
        LOG("big %d: %dx%d tex=%p\n", d->item, d->px.w, d->px.h, (void *)big_tex);
        big_failed = !big_tex;
      }
    } else {
      // the slot used longest ago
      int best = 0;
      for (int s = 1; s < SLOTS; s++) if (slots[s].used < slots[best].used) best = s;
      if (slots[best].tex) { free_later(slots[best].tex); tstate[slots[best].item] = 0; slot_of[slots[best].item] = -1; }
      slots[best].tex = upload(&d->px);
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
  for (int k = 0; k < SLOTS; k++) { if (slots[k].tex) vita2d_free_texture(slots[k].tex); if (slots[k].item >= 0 && slot_of) slot_of[slots[k].item] = -1; slots[k].tex = NULL; slots[k].item = -1; slots[k].used = 0; }
  if (big_tex) vita2d_free_texture(big_tex);
  big_tex = NULL;
  big_item = -1;
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
  p.type = SCE_IME_TYPE_DEFAULT;
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
enum { S_PICK, S_LOCKED, S_UNLOCKING, S_LIBRARY, S_VIEWER, S_SETTINGS, S_COVER, S_WEB, S_WEBVIEW };
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

static int scroll_target, sel;
static float scroll, scroll_v;
static void open_vault(const char *path) {
  if (loaded_vault) { lv_vault_save_views(&vault); drop_textures(); lv_vault_free(&vault); loaded_vault = 0; }
  lv_vault_init(&vault, path);
  snprintf(cfg.vault, sizeof cfg.vault, "%s", path);
  cfg_save();
  screen = vault.untraceable ? S_LOCKED : S_UNLOCKING;
}
static void finish_open(void) {
  const char *why = NULL;
  if (lv_vault_load(&vault, &why) != 0) { say(why ? why : "The vault could not be opened"); screen = S_PICK; return; }
  loaded_vault = 1;
  free(tstate);
  tstate = calloc(vault.n + 1, 1);
  free(slot_of);
  slot_of = malloc((vault.n + 1) * sizeof *slot_of);
  for (int i = 0; i <= vault.n; i++) slot_of[i] = -1;
  for (int k = 0; k < SLOTS; k++) slots[k].item = -1;
  snprintf(cache_dir, sizeof cache_dir, DATA "/cache/%08lx", fnv(vault.root));
  sceIoMkdir(DATA "/cache", 0777);
  sceIoMkdir(cache_dir, 0777);
  apply_theme();
  cat = -1;
  query[0] = 0;
  refilter();
  scroll = scroll_v = 0;
  sel = 0;
  screen = S_LIBRARY;
}

// ----- the picker: which vault, or how to put one on the Vita -----
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
}

// ----- an Untraceable vault: the password -----
static char password[128];
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
static void draw_unlocking(int stage) {
  float cx = W / 2.0f;
  float a = frame_no * 0.12f;
  for (int k = 0; k < 8; k++) {
    float t = a + k * 0.785f;
    circle(cx + cosf(t) * 26, 230 + sinf(t) * 26, 4 + k * 0.6f, with_alpha(ACC, 80 + k * 20));
  }
  text_center(cx, 310, TEXT, 22, stage ? "Opening the vault…" : "Checking the password…");
}

static int set_sel;
// ----- the library -----
static void open_viewer(int k, int slideshow);
static void draw_bar(void) {
  vita2d_draw_rectangle(0, H - BAR, W, BAR, with_alpha(SURF, 245));
  vita2d_draw_line(0, H - BAR, W, H - BAR, with_alpha(TEXT, 20));
  static const char *labels[] = {"Library", "Browser", "Slideshow", "Settings"};
  for (int k = 0; k < 4; k++) {
    int cx = W / 8 + k * W / 4;
    int on = (k == 0 && screen == S_LIBRARY) || (k == 1 && screen == S_WEB) || (k == 3 && screen == S_SETTINGS);
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
    // (the button that leads here: ○ goes round Library, Browser, Settings)
    int next = (screen == S_LIBRARY && k == 1) || (screen == S_WEB && k == 3) || (screen == S_SETTINGS && k == 0);
    if (next) { circle(cx + 30, y - 6, 8, with_alpha(TEXT, 40)); text_center(cx + 30, y - 1, DIM, 12, "\u25cb"); }
    if (hit(cx - W / 8, H - BAR, W / 4, BAR)) {
      if (k == 0) { screen = S_LIBRARY; if (query[0]) { query[0] = 0; refilter(); } }
      if (k == 1) screen = S_WEB;
      if (k == 2 && nshown) open_viewer(sel < nshown ? sel : 0, 1);
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
static void draw_library(void) {
  int rows = (nshown + COLS - 1) / COLS;
  float content = rows * (CARD_H + GAP) + 12;
  float view_h = H - TOP - BAR;
  float max_scroll = fmaxf(0, content - view_h);

  // --- moving: touch drag and flick, the stick, the D-pad selection ---
  if (in.touching && in.ty > TOP && in.ty < H - BAR) { scroll -= in.drag_dy; scroll_v = 0; }
  if (in.flicked && in.down_y > TOP) scroll_v = -in.fvy * 1.6f;
  scroll += scroll_v;
  scroll_v *= 0.94f;
  if (fabsf(scroll_v) < 0.2f) scroll_v = 0;
  scroll += in.ly * 14;
  int moved_sel = 0;
  if (in.pressed & SCE_CTRL_RIGHT) { sel++; moved_sel = 1; }
  if (in.pressed & SCE_CTRL_LEFT) { sel--; moved_sel = 1; }
  if (in.pressed & SCE_CTRL_DOWN) { sel += COLS; moved_sel = 1; }
  if (in.pressed & SCE_CTRL_UP) { sel -= COLS; moved_sel = 1; }
  if (sel >= nshown) sel = nshown - 1;
  if (sel < 0) sel = 0;
  if (moved_sel) {
    float top = (sel / COLS) * (CARD_H + GAP);
    if (top < scroll) scroll_target = (int)top;
    else if (top + CARD_H > scroll + view_h) scroll_target = (int)(top + CARD_H - view_h + 10);
    else scroll_target = (int)scroll;
    scroll += (scroll_target - scroll) * 0.6f;
  }
  if (scroll < 0) { scroll = in.touching ? scroll : scroll * 0.7f; if (!in.touching) scroll_v = 0; }
  if (scroll > max_scroll) { scroll = in.touching ? scroll : max_scroll + (scroll - max_scroll) * 0.7f; if (!in.touching) scroll_v = 0; }

  // --- the grid ---
  int first = (int)fmaxf(0, scroll / (CARD_H + GAP)) * COLS;
  int last = (int)((scroll + view_h) / (CARD_H + GAP) + 1) * COLS + COLS;
  int wanted[64], nw = 0;
  for (int k = first; k < last && k < nshown; k++) {
    int i = shown[k];
    float x = MARGIN + (k % COLS) * (CARD_W + GAP);
    float y = TOP + 6 + (k / COLS) * (CARD_H + GAP) - scroll;
    if (y > H || y + CARD_H < TOP - 4) continue;
    LvItem *it = &vault.items[i];
    vita2d_texture *t = thumb_of(i);
    int on = k == sel && !in.touching;
    float grow = on ? 4 : 0;
    if (on) rrect(x - grow - 3, y - grow - 3, CARD_W + 2 * grow + 6, CARD_H + 2 * grow + 6, 14, ACC);
    if (t) draw_cover(t, x - grow, y - grow, CARD_W + 2 * grow, CARD_H + 2 * grow, RGBA(255, 255, 255, 255));
    else {
      rrect(x - grow, y - grow, CARD_W + 2 * grow, CARD_H + 2 * grow, 10, SURF);
      if (tstate[i] == 3) text_center(x + CARD_W / 2, y + CARD_H / 2, FAINT, 14, "No picture");
      else if (nw < 64) wanted[nw++] = i;
    }
    if (is_video(it) || is_gif(it)) {
      char d[24];
      if (is_gif(it)) snprintf(d, sizeof d, "GIF");
      else snprintf(d, sizeof d, "%d:%02d", (int)it->duration / 60, (int)it->duration % 60);
      int tw = text_w(13, d);
      rrect(x + CARD_W - tw - 16, y + CARD_H - 26, tw + 12, 20, 10, RGBA(0, 0, 0, 150));
      text(x + CARD_W - tw - 10, y + CARD_H - 11, TEXT, 13, d);
    }
    if (lv_likes_of(&vault, it) > 0) heart(x + 16, y + CARD_H - 16, 6, ACC);
    if (!it->path) { rrect(x + 6, y + 6, 20, 20, 10, RGBA(0, 0, 0, 160)); text_center(x + 16, y + 21, ACC2, 14, "!"); }
    if (hit((int)x, (int)y, CARD_W, CARD_H) && in.tap_y > TOP && in.tap_y < H - BAR) { sel = k; open_viewer(k, 0); return; }
  }
  // (and the next screenful, ready before it's needed)
  for (int k = last; k < last + COLS * 3 && k < nshown && nw < 64; k++) if (!thumb_of(shown[k]) && tstate[shown[k]] != 3) wanted[nw++] = shown[k];
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
  int ws = text_w(15, SORTS[cfg.sort]) + 26, wk = text_w(15, KINDS[cfg.kind]) + 26;
  sx -= ws;
  rrect(sx, 8, ws, 26, 13, SURF2);
  text(sx + 13, 26, TEXT, 15, SORTS[cfg.sort]);
  if (hit(sx, 0, ws, 36) || (in.pressed & SCE_CTRL_TRIANGLE)) { cfg.sort = (cfg.sort + 1) % 6; refilter(); cfg_save(); sel = 0; scroll = 0; }
  sx -= wk + 8;
  rrect(sx, 8, wk, 26, 13, SURF2);
  text(sx + 13, 26, TEXT, 15, KINDS[cfg.kind]);
  if (hit(sx, 0, wk, 36) || (in.pressed & SCE_CTRL_SQUARE)) { cfg.kind = (cfg.kind + 1) % 3; refilter(); cfg_save(); sel = 0; scroll = 0; }
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
      if (hit((int)cxp, 34, w, 30)) { cat = k; refilter(); sel = 0; scroll = 0; }
    }
    // keep the chosen chip in view
    if (k == cat && (in.pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {
      if (cxp < 0) chips_scroll += cxp - 12;
      if (cxp + w > W) chips_scroll += cxp + w - W + 12;
    }
    cxp += w + 6;
  }

  if ((in.pressed & SCE_CTRL_CROSS) && nshown) open_viewer(sel, 0);
  if ((in.pressed & SCE_CTRL_START) && nshown) open_viewer(sel, 1);
  if (in.pressed & SCE_CTRL_CIRCLE) screen = S_WEB; // (○ goes round the tabs: Library, Browser, Settings)
  draw_bar();
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
static unsigned view_started;
static float pinch_d0, zoom0;

static void stop_video(void) {
  if (vid) { lv_video_close(vid); vid = NULL; }
  vid_item = -1;
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
  vk = (k % nshown + nshown) % nshown;
  LvItem *it = &vault.items[shown[vk]];
  zoom = 1; pan_x = pan_y = 0;
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
  }
  ask_big(shown[vk]);
}
static void open_viewer(int k, int show) {
  LOG("open_viewer %d show=%d\n", k, show);
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
  LOG("close_viewer screen=%d\n", screen);
  account_time();
  stop_video();
  if (lv_vault_save_views(&vault) != 0) LOG("views.csv could not be saved\n");
  screen = S_LIBRARY;
  sel = vk;
  float top = (sel / COLS) * (CARD_H + GAP);
  if (top < scroll || top + CARD_H > scroll + H - TOP - BAR) scroll = top - 40;
}

static void fmt_time(uint64_t ms, char *o, size_t n) { int s = (int)(ms / 1000); snprintf(o, n, "%d:%02d", s / 60, s % 60); }

static void draw_viewer(void) {
  if (nshown == 0) { screen = S_LIBRARY; return; }
  LvItem *it = &vault.items[shown[vk]];
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 255));
  // --- what's shown: the video's frame, else the picture (the thumbnail while it loads) ---
  vita2d_texture *t = NULL;
  int is_frame = 0;
  if (vid) { t = lv_video_frame(vid); is_frame = t != NULL; }
  if (!t) t = big_item == shown[vk] ? big_tex : NULL;
  if (!t) t = thumb_of(shown[vk]);
  float avail_h = H - details_y * 300;
  if (t) {
    float tw = is_frame ? lv_video_width(vid) : vita2d_texture_get_width(t);
    float th = is_frame ? lv_video_height(vid) : vita2d_texture_get_height(t);
    float s = fminf(W / tw, avail_h / th) * zoom;
    float dw = tw * s, dh = th * s;
    // keep a zoomed picture's edges on screen
    float mx = fmaxf(0, (dw - W) / 2), my = fmaxf(0, (dh - avail_h) / 2);
    pan_x = fminf(mx, fmaxf(-mx, pan_x));
    pan_y = fminf(my, fmaxf(-my, pan_y));
    float x = (W - dw) / 2 + pan_x + swipe_x, y = (avail_h - dh) / 2 + pan_y;
    vita2d_draw_texture_scale(t, x, y, s, s);
  } else {
    text_center(W / 2, H / 2, DIM, 18, big_failed ? "This file can't be shown on the Vita" : "Loading…");
  }
  if (!it->path) text_center(W / 2, (int)avail_h - 30, ACC2, 16, "This file isn't on the memory card (only its thumbnail)");
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
    if (swipe_x > 120) { swipe_x = 0; show_item(vk - 1); return; }
    if (swipe_x < -120) { swipe_x = 0; show_item(vk + 1); return; }
    swipe_x *= 0.7f;
    if (zoom < 1.01f) zoom = 1;
  }
  if (in.dbl) { zoom = zoom > 1.5f ? 1 : 2.5f; pan_x = (W / 2.0f - in.tap_x) * (zoom - 1); pan_y = (H / 2.0f - in.tap_y) * (zoom - 1); }
  else if (in.tap && details_y < 0.5f) {
    if (vid && !vid_clip && in.tap_y > H - 70) {
      uint64_t d = lv_video_duration(vid);
      if (d) lv_video_seek(vid, (int64_t)(d * fminf(1, fmaxf(0, (in.tap_x - 40) / (float)(W - 80)))));
    } else if (frame_no < chrome_until && in.tap_x < 70 && in.tap_y < 60) { close_viewer(); return; }
    else if (frame_no < chrome_until && in.tap_x > W - 70 && in.tap_y < 60) { details = 1; }
    else if (frame_no < chrome_until && in.tap_x > W - 140 && in.tap_x < W - 70 && in.tap_y < 60) { char now[40]; now_iso(now, sizeof now); lv_liked(&vault, it, now); }
    else chrome_until = frame_no < chrome_until ? frame_no : frame_no + 240;
  }

  // --- buttons ---
  if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER)) { show_item(vk + 1); return; }
  if (in.pressed & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER)) { show_item(vk - 1); return; }
  if (in.pressed & SCE_CTRL_CIRCLE) { if (details) details = 0; else { close_viewer(); return; } }
  if (in.pressed & SCE_CTRL_TRIANGLE) details = !details;
  if (in.pressed & SCE_CTRL_SQUARE) { char now[40]; now_iso(now, sizeof now); lv_liked(&vault, it, now); chrome_until = frame_no + 120; }
  if (in.pressed & SCE_CTRL_START) { slideshow = !slideshow; slide_at = frame_no; say(slideshow ? "Slideshow" : "Slideshow stopped"); }
  if (in.pressed & SCE_CTRL_CROSS) {
    if (vid && !vid_clip) lv_video_pause(vid, !lv_video_paused(vid));
    else { zoom = zoom > 1.5f ? 1 : 2.5f; pan_x = pan_y = 0; }
  }
  if (vid && !vid_clip) {
    if (in.pressed & SCE_CTRL_UP) lv_video_seek(vid, (int64_t)lv_video_pos(vid) + 10000);
    if (in.pressed & SCE_CTRL_DOWN) lv_video_seek(vid, (int64_t)lv_video_pos(vid) - 10000);
  } else if (zoom > 1.01f) {
    pan_x -= in.lx * 12; pan_y -= in.ly * 12;
  }
  if (in.now & SCE_CTRL_RTRIGGER && in.ly) zoom = fminf(6, fmaxf(1, zoom - in.ly * 0.05f));

  // --- the slideshow moves on by itself: pictures after the interval, videos when they end ---
  if (slideshow) {
    int is_moving = vid && !vid_clip;
    if ((is_moving && lv_video_ended(vid)) || (!is_moving && frame_no - slide_at > (unsigned)cfg.interval * 60)) {
      if (cfg.shuffle && nshown > 1) { int n; do n = rnd() % nshown; while (n == vk); show_item(n); }
      else show_item(vk + 1);
      return;
    }
    float p = is_moving ? 0 : (frame_no - slide_at) / (float)(cfg.interval * 60);
    vita2d_draw_rectangle(0, H - 3, W * p, 3, with_alpha(ACC, 200));
  }

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
      if (lv_video_paused(vid)) {
        circle(W / 2, H / 2, 40, RGBA(0, 0, 0, 140));
        for (int k = 0; k < 30; k++) vita2d_draw_rectangle(W / 2 - 10 + k, H / 2 - 15 + k / 2, 1, 30 - k, with_alpha(TEXT, a));
      }
    }
  }

  // --- the details, pulled up from below (as in the phone's full view) ---
  float target = details ? 1 : 0;
  details_y += (target - details_y) * 0.25f;
  if (details_y > 0.01f) {
    float top = H - details_y * 300;
    rrect(0, top, W, 330, 20, with_alpha(SURF, 250));
    rrect(W / 2 - 24, top + 8, 48, 5, 3, with_alpha(TEXT, 60));
    text_fit(30, top + 46, TEXT, 22, it->name[0] ? it->name : it->file, W - 260);
    // its score, as dots out of ten
    for (int k = 0; k < 10; k++) circle(W - 220 + k * 19, top + 38, 6, k < it->quality ? ACC : with_alpha(TEXT, 40));
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
static void draw_settings(void) {
  text(30, 44, TEXT, 26, "Settings");
  char v1[64], v2[64], v3[64];
  snprintf(v1, sizeof v1, "%d seconds", cfg.interval);
  snprintf(v2, sizeof v2, "%s", cfg.shuffle ? "On" : "Off");
  snprintf(v3, sizeof v3, "%d%%", cfg.volume);
  struct { const char *name, *value, *hint; } rows[] = {
    {"Search", query[0] ? query : "", "Tags, names, categories and collections"},
    {"Slideshow: each picture for", v1, "← → to change"},
    {"Slideshow: shuffle", v2, "Plays the items in a random order"},
    {"Video volume", v3, "← → to change"},
    {"Open another vault", "", "The vaults on the memory card"},
    {"Clear the thumbnail cache", "", "They're made again as you scroll"},
    {"Cover the screen", "SELECT", "A plain screen, at a press, anywhere"},
  };
  int n = sizeof rows / sizeof *rows;
  for (int k = 0; k < n; k++) {
    int y = 58 + k * 55;
    rrect(30, y, W - 60, 50, 14, k == set_sel ? SURF2 : SURF);
    if (k == set_sel) rrect(30, y, 5, 50, 3, ACC);
    text(52, y + 22, TEXT, 18, rows[k].name);
    text(52, y + 42, FAINT, 13, rows[k].hint);
    if (rows[k].value[0]) text(W - 52 - text_w(18, rows[k].value), y + 32, ACC, 18, rows[k].value);
    if (hit(30, y, W - 60, 50)) { set_sel = k; in.pressed |= SCE_CTRL_CROSS; }
  }
  textf(30, H - BAR - 8, FAINT, 13, "%s  ·  %d items%s", vault.root, vault.n, vault.untraceable ? "  ·  Untraceable" : "");
  if (in.pressed & SCE_CTRL_DOWN) set_sel = (set_sel + 1) % n;
  if (in.pressed & SCE_CTRL_UP) set_sel = (set_sel + n - 1) % n;
  int d = (in.pressed & SCE_CTRL_RIGHT) ? 1 : (in.pressed & SCE_CTRL_LEFT) ? -1 : 0;
  if (in.pressed & SCE_CTRL_CROSS) d = d ? d : 1;
  if (d) {
    switch (set_sel - 1) {
      case -1: if (in.pressed & SCE_CTRL_CROSS && !ime_open) ime_start(2, "Search tags, names, categories", query, 0); break;
      case 0: cfg.interval = cfg.interval + d * (cfg.interval >= 10 ? 5 : 1); if (cfg.interval > 60) cfg.interval = 2; if (cfg.interval < 2) cfg.interval = 60; break;
      case 1: cfg.shuffle = !cfg.shuffle; break;
      case 2: cfg.volume += d * 10; if (cfg.volume > 100) cfg.volume = 0; if (cfg.volume < 0) cfg.volume = 100; break;
      case 3: if (in.pressed & SCE_CTRL_CROSS) { lv_vault_save_views(&vault); find_vaults(); screen = S_PICK; return; } break;
      case 4:
        if (in.pressed & SCE_CTRL_CROSS) {
          DIR *dd = opendir(cache_dir);
          struct dirent *e;
          char p[900];
          while (dd && (e = readdir(dd))) { if (e->d_name[0] == '.') continue; snprintf(p, sizeof p, "%s/%s", cache_dir, e->d_name); sceIoRemove(p); }
          if (dd) closedir(dd);
          say("Thumbnail cache cleared");
        }
        break;
      case 5: if (in.pressed & SCE_CTRL_CROSS) { cover_return = S_SETTINGS; screen = S_COVER; } break;
    }
    cfg_save();
  }
  if (in.pressed & SCE_CTRL_CIRCLE) screen = S_LIBRARY;
  draw_bar();
}

// ----- the cover: a plain clock, nothing of the vault (SELECT, from anywhere) -----
static void draw_cover_screen(void) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(10, 12, 16, 255));
  time_t t = time(NULL);
  struct tm l;
  localtime_r(&t, &l);
  char b[16];
  strftime(b, sizeof b, "%H:%M", &l);
  text_center(W / 2, H / 2 + 20, RGBA(220, 225, 235, 255), 72, b);
  strftime(b, sizeof b, "%A", &l);
  text_center(W / 2, H / 2 + 64, RGBA(120, 128, 140, 255), 20, b);
}

// The vault got an item (a save from the Browser): the Library's per-item arrays grow with it.
static void library_grew(void) {
  signed char *t = realloc(tstate, vault.n + 1);
  short *so = realloc(slot_of, (vault.n + 1) * sizeof *so);
  if (t) tstate = t;
  if (so) slot_of = so;
  if (t && so) { tstate[vault.n - 1] = 0; slot_of[vault.n - 1] = -1; tstate[vault.n] = 0; slot_of[vault.n] = -1; }
  refilter();
}

#include "web.c"

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
  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  // (a bigger per-frame vertex pool than vita2d's 1 MB: room to spare on the busiest screen)
  vita2d_init_advanced(4 * 1024 * 1024);
  make_disc();
  lv_video_release_texture = free_later;
  vita2d_set_clear_color(BG);
  vita2d_set_vblank_wait(1);
  font = vita2d_load_font_file("app0:font.ttf");
  pgf = vita2d_load_default_pgf();
  rnd_state ^= (unsigned)time(NULL);
  cfg_load();

  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 256 * 1024);
  pthread_create(&th, &at, worker, NULL);

  find_vaults();
  if (cfg.vault[0] && lv_is_vault(cfg.vault)) open_vault(cfg.vault);
  else if (nfound == 1) open_vault(found[0]);

  int unlock_stage = 0;
  unsigned idle_save = 0;
  for (;;) {
    frame_no++;
    read_input();
    if (npending) { vita2d_wait_rendering_done(); for (int k = 0; k < npending; k++) vita2d_free_texture(pending_free[k]); npending = 0; }
    if (loaded_vault) take_results();
    if (web_started) web_take_results();

    // the keyboard's answer
    if (ime_open) {
      char got[120];
      int r = ime_poll(got, sizeof got);
      if (r == 1 && ime_for == 1) { snprintf(password, sizeof password, "%s", got); screen = S_UNLOCKING; unlock_stage = 0; }
      if (r == 1 && ime_for == 3) { char clean[128]; snprintf(cfg.wquery, sizeof cfg.wquery, "%s", lv_booru_clean(got, clean, sizeof clean)); sheet = SH_NONE; screen = S_WEB; web_search(); }
      if (r == 1 && ime_for == 2) { snprintf(query, sizeof query, "%s", got); cat = -1; refilter(); sel = 0; scroll = 0; screen = S_LIBRARY; }
      if (r != 0) memset(&in, 0, sizeof in);
      else in.pressed = 0, in.tap = 0;
    }
    // SELECT: the cover, from anywhere (and back)
    if (in.pressed & SCE_CTRL_SELECT) {
      if (screen == S_COVER) screen = cover_return;
      else if (screen != S_UNLOCKING) {
        cover_return = screen;
        if (vid) lv_video_pause(vid, 1);
        if (wv_vid) lv_video_pause(wv_vid, 1);
        screen = S_COVER;
      }
      in.pressed = 0;
    }

    vita2d_start_drawing();
    vita2d_clear_screen();
    switch (screen) {
      case S_PICK: draw_pick(); break;
      case S_LOCKED: draw_locked(); break;
      case S_UNLOCKING:
        draw_unlocking(unlock_stage >= 2 || !vault.untraceable);
        break;
      case S_LIBRARY: draw_library(); break;
      case S_VIEWER: draw_viewer(); break;
      case S_SETTINGS: draw_settings(); break;
      case S_WEB: draw_web(); break;
      case S_WEBVIEW: draw_webview(); break;
      case S_COVER: draw_cover_screen(); if (in.tap) { screen = cover_return; } break;
    }
    if (frame_no < message_until && screen != S_COVER) {
      int w = text_w(16, message) + 40;
      int a = message_until - frame_no < 20 ? (message_until - frame_no) * 12 : 240;
      rrect(W / 2 - w / 2, H - BAR - 56, w, 38, 19, RGBA(0, 0, 0, a * 3 / 4));
      text_center(W / 2, H - BAR - 31, with_alpha(TEXT, a), 16, message);
    }
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();

    // the slow steps run between frames, after a frame that says what's happening has been shown
    if (screen == S_UNLOCKING) {
      if (++unlock_stage == 2) {
        if (vault.untraceable) {
          int ok = lv_vault_unlock(&vault, password);
          memset(password, 0, sizeof password);
          if (ok != 1) { say(ok == 0 ? "That password isn't right" : "The vault's password check is missing"); screen = S_LOCKED; continue; }
        }
      } else if (unlock_stage == 4) {
        finish_open();
        unlock_stage = 0;
      }
    }
    // views reach the card now and then while looking (and always when leaving the full view)
    if (loaded_vault && vault.views_dirty && ++idle_save > 60 * 30) { lv_vault_save_views(&vault); idle_save = 0; }
  }
  return 0;
}
