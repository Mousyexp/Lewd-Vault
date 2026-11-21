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
#include <psp2/shellutil.h>
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>
#include <psp2/kernel/threadmgr/callback.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <pthread.h>
#include <malloc.h>
#include <ctype.h>
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
#include "favmerge.h"
#include "net.h"
#include "discover.h"
#include "conv.h"
#include "manga.h"
#include <psp2/kernel/clib.h>
#ifdef LV_DEBUG
// (debug builds: PT("label") logs the time since the last mark when it's over 4 ms)
static uint64_t pt_last;
#define PT0() (pt_last = sceKernelGetProcessTimeWide())
#define PT(label) do { uint64_t n_ = sceKernelGetProcessTimeWide(); if (pt_last && n_ - pt_last > 4000) { LOG("pt %s %.1fms\n", label, (n_ - pt_last) / 1000.0); n_ = sceKernelGetProcessTimeWide(); } pt_last = n_; } while (0)
#else
#define PT(label) do { } while (0)
#define PT0() do { } while (0)
#endif
#ifdef LV_DEBUG
#define LOG(...) do { char lb_[256]; int ln_ = snprintf(lb_, sizeof lb_, __VA_ARGS__); SceUID lf_ = sceIoOpen("ux0:data/LWDV00001/log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666); if (lf_ >= 0) { sceIoWrite(lf_, lb_, ln_ > 255 ? 255 : ln_); sceIoClose(lf_); } } while (0)
#else
#define LOG(...) do { } while (0)
#endif

// A worker thread onto one of the user cores (0: the drawing thread's; 1 and 2 for the work), as
// the Vita wants it: the drawing thread is never pre-empted by decoding or the network.
// The drawing thread has core 0 to itself; every worker may run on core 1 or 2, the system putting it
// on the freer one (pinned to one core each, most of them landed on core 2 while core 1 idled).
static void pin_to_core(int core) {
  int mask = core <= 0 ? SCE_KERNEL_CPU_MASK_USER_0 : (SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2);
  sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
}
// The C heap: 128 MB. Measured in use: a few MB browsing, ~30 MB at most while a big picture is
// decoded (a vault of 10,000 items adds a few MB of strings). What it doesn't take is left to the
// textures that don't fit in video memory (tex_new), the video player and the system: a Vita app has
// about 256 MB in all, and a heap that asked for most of it left them short.
int _newlib_heap_size_user = 128 * 1024 * 1024;
// A texture: in video memory (vita2d's default) and, when that's full, in main memory instead of
// failing (the GPU reads either; video memory is only 128 MB, shared with the screen's buffers).
// Never right up to the limit: a little over it is a crash, so each memory keeps a reserve free (about
// 6% of video memory's 128 MB, and of an app's ~256 MB of main memory). A texture that would eat into
// a reserve goes to the other memory, or isn't made at all (its card shows a ring, or "can't be shown").
#define RESERVE_CDRAM (8u << 20)
#define RESERVE_USER (16u << 20)
static int mem_free(int *user, int *cdram) {
  SceKernelFreeMemorySizeInfo i;
  memset(&i, 0, sizeof i);
  i.size = sizeof i;
  if (sceKernelGetFreeMemorySize(&i) < 0) return -1;
  *user = i.size_user; *cdram = i.size_cdram;
  return 0;
}
// (main memory comes in 4 KiB pages; a texture this small goes there, not into a 256 KiB granule)
#define SMALL_TEX (128u << 10)
static vita2d_texture *in_main_memory(vita2d_texture *(*make)(unsigned, unsigned, const char *), unsigned w, unsigned h, const char *path) {
  SceKernelMemBlockType was = vita2d_texture_get_alloc_memblock_type();
  vita2d_texture_set_alloc_memblock_type(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
  vita2d_texture *t = make(w, h, path);
  vita2d_texture_set_alloc_memblock_type(was);
  return t;
}
static vita2d_texture *make_empty(unsigned w, unsigned h, const char *path) { (void)path; return vita2d_create_empty_texture(w, h); }
static vita2d_texture *make_png(unsigned w, unsigned h, const char *path) { (void)w; (void)h; return vita2d_load_PNG_file(path); }
// An icon's PNG (a few KB of pixels): in main memory, not a 256 KiB granule of video memory each
static vita2d_texture *load_png_small(const char *path) { return in_main_memory(make_png, 0, 0, path); }
static void make_room(size_t need); // (the parts not on screen let go, if memory is short: below)
static int (*tex_no_room)(void); // (main.c's release_hidden_now, once there is one; main thread only)
vita2d_texture *tex_new(unsigned w, unsigned h) {
  // (video memory comes in 256 KiB granules: a small texture still takes a whole one)
  long need = ((long)((w + 7) & ~7u) * h * 4 + 4096 + 0x3FFFF) & ~0x3FFFFL;
  int user = 0, cdram = 0, known = mem_free(&user, &cdram) == 0;
  vita2d_texture *t = NULL;
  if ((size_t)((w + 7) & ~7u) * h * 4 <= SMALL_TEX && (!known || user > (int)RESERVE_USER + (1 << 20)) && (t = in_main_memory(make_empty, w, h, NULL))) return t;
  if (!known || cdram - need >= (long)RESERVE_CDRAM) t = vita2d_create_empty_texture(w, h);
  if (t) return t;
  if (known && user - need < (long)RESERVE_USER) {
    // (no room: the parts not on screen give theirs back at once, and it's tried once more)
    static int again;
    if (!again && tex_no_room && tex_no_room()) { again = 1; t = tex_new(w, h); again = 0; return t; }
    LOG("tex_new %ux%u: no room (user %d, cdram %d)\n", w, h, user, cdram);
    return NULL;
  }
  SceKernelMemBlockType was = vita2d_texture_get_alloc_memblock_type();
  vita2d_texture_set_alloc_memblock_type(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
  t = vita2d_create_empty_texture(w, h);
  vita2d_texture_set_alloc_memblock_type(was);
  return t;
}
// The C heap the same way: room for `need` more bytes and still under 95% of it.
// A worker's wait for work (its mutex held, as pthread_cond_wait). With a connection open and nothing
// to do for 20 s, the connection is closed: a part of the app left behind keeps no sockets.
static void wait_work(pthread_cond_t *cv, pthread_mutex_t *mx, LvHttp *h) {
  if (!lv_http_has_conn(h)) { pthread_cond_wait(cv, mx); return; }
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += 20;
  if (pthread_cond_timedwait(cv, mx, &until) == 0) return;
  pthread_mutex_unlock(mx); // (closing may say goodbye to the server: not with the lock held)
  lv_http_drop(h);
  pthread_mutex_lock(mx);
}
static int heap_room(size_t need) {
  struct mallinfo mi = mallinfo();
  size_t heap = (size_t)_newlib_heap_size_user, used = (size_t)mi.uordblks;
  return used + need <= heap / 100 * 95;
}

#define W 960
#define H 544
#define POOL_SIZE (4 * 1024 * 1024) // (vita2d's per-frame vertex pool: three frames' thirds, main())
#define DATA "ux0:data/LWDV00001"
// The header (one row of large buttons) and the floating tab bar tuck away while the Library or the
// Browser is scrolled down, and come back on the way up: chrome_hide goes 0 (shown) to 1 (tucked).
static float chrome_hide, chrome_target;
#define TOP_FULL 56
#define TOP ((int)(TOP_FULL * (1 - chrome_hide) + 0.5f)) // (where the content starts)
#define HDR_Y (TOP - TOP_FULL)                            // (the header's own top: 0, or above the screen)
#define BAR ((int)(80 - 36 * chrome_hide))                // (the tab bar's room)
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
#define NTHEMES ((int)(sizeof THEMES / sizeof *THEMES)) // (and theme NTHEMES + 1: Custom, cfg.acc_*)
// A colour from hue (degrees), saturation and value (0-1)
static unsigned hsv(float h, float s, float v) {
  h = fmodf(h, 360); if (h < 0) h += 360;
  float c = v * s, x = c * (1 - fabsf(fmodf(h / 60, 2) - 1)), m = v - c, r, g, b;
  if (h < 60) { r = c; g = x; b = 0; } else if (h < 120) { r = x; g = c; b = 0; } else if (h < 180) { r = 0; g = c; b = x; }
  else if (h < 240) { r = 0; g = x; b = c; } else if (h < 300) { r = x; g = 0; b = c; } else { r = c; g = 0; b = x; }
  return RGBA((int)((r + m) * 255 + .5f), (int)((g + m) * 255 + .5f), (int)((b + m) * 255 + .5f), 255);
}
static unsigned with_alpha(unsigned c, int a) { return (c & 0x00FFFFFF) | (unsigned)a << 24; }
// The backgrounds: the page, its cards, their raised parts, and the text's three greys
static const struct { const char *name; unsigned bg, surf, surf2, text, dim, faint; } BGS[] = {
  {"Dark", RGBA(18, 14, 17, 255), RGBA(34, 27, 32, 255), RGBA(48, 39, 45, 255), RGBA(247, 232, 236, 255), RGBA(190, 170, 180, 255), RGBA(120, 105, 114, 255)},
  {"Black (OLED)", RGBA(0, 0, 0, 255), RGBA(18, 16, 18, 255), RGBA(32, 29, 32, 255), RGBA(245, 240, 242, 255), RGBA(180, 172, 176, 255), RGBA(110, 104, 108, 255)},
  {"Midnight", RGBA(12, 16, 28, 255), RGBA(24, 30, 48, 255), RGBA(36, 44, 68, 255), RGBA(232, 238, 250, 255), RGBA(168, 180, 205, 255), RGBA(104, 114, 140, 255)},
  {"Plum", RGBA(26, 14, 30, 255), RGBA(42, 24, 48, 255), RGBA(60, 36, 68, 255), RGBA(248, 232, 250, 255), RGBA(200, 172, 208, 255), RGBA(130, 104, 140, 255)},
};
static const char *SND_PACKS[4] = {"", "soft", "retro", "bubbly"};
static const char *SND_NAMES[4] = {"The Vita's own", "Soft", "Retro (8-bit)", "Bubbly"};

// ---------- text ----------
// A font per size, all reading the one copy of font.ttf in memory: vita2d's font keeps each letter
// once, at the first size it was drawn, and scales that for every other size (blurry big text,
// ragged small text, its neighbours in the atlas bleeding in round the edges). One per size, each
// letter is drawn at its own size, pixel for pixel. Made the first time a size is used.
static vita2d_font *font; // (the first one made: for anything that wants "the" font)
static uint8_t *font_mem;
static size_t font_mem_n;
#define FONT_SIZES 80
static vita2d_font *font_sz[FONT_SIZES];
static vita2d_font *font_at(int size) {
  if (size < 6) size = 6;
  if (size >= FONT_SIZES) size = FONT_SIZES - 1;
  if (!font_sz[size]) {
    if (!font_mem) font_mem = lv_read_all("app0:font.ttf", NULL, &font_mem_n);
    font_sz[size] = font_mem ? vita2d_load_font_mem(font_mem, (unsigned)font_mem_n) : vita2d_load_font_file("app0:font.ttf");
    if (!font_sz[size]) return font; // (out of memory: the one there is, scaled)
    if (!font) font = font_sz[size];
  }
  return font_sz[size];
}
static vita2d_pgf *pgf; // the system's font (Japanese and more), where the firmware has it
static int has_wide(const char *s) {
  for (; *s; s++) if ((unsigned char)*s >= 0xE3) return 1;
  return 0;
}
// Widths measured once: the same labels are measured every frame (the headers, the buttons, the
// chips), and measuring walks the font's glyphs. A small table by the text and its size.
#define TW_CACHE 512
static struct { unsigned h; short size, w; char s[160]; } tw_cache[TW_CACHE]; // (hints are long: kept too)
// Text size (Settings: 90% to 125%): every size the screens ask for, scaled here, where it's measured
// and drawn
static int text_pct = 100;
#define TS(size) (((size) * text_pct + 50) / 100)
static int text_w_raw(int size, const char *s) {
  size = TS(size);
  if (pgf && has_wide(s)) return vita2d_pgf_text_width(pgf, size / 17.0f, s);
  return vita2d_font_text_width(font_at(size), size, s);
}
static int text_w(int size, const char *s) {
  size_t n = strlen(s);
  if (n >= 160) return text_w_raw(size, s);
  unsigned h = 2166136261u ^ (unsigned)size ^ ((unsigned)text_pct << 20); // (another text size: measured anew)
  for (size_t k = 0; k < n; k++) h = (h ^ (unsigned char)s[k]) * 16777619u;
  int at = h & (TW_CACHE - 1);
  if (tw_cache[at].h == h && tw_cache[at].size == size && !strcmp(tw_cache[at].s, s)) return tw_cache[at].w;
  int w = text_w_raw(size, s);
  tw_cache[at].h = h; tw_cache[at].size = (short)size; tw_cache[at].w = (short)w;
  memcpy(tw_cache[at].s, s, n + 1);
  return w;
}
// A drawing given a coordinate that isn't a number, or is absurdly far off: not drawn (NaN or a huge
// value reaching the GPU draws garbage over the whole screen). Debug builds say who asked.
static unsigned bad_draws;
#define BAD1(v) (!isfinite(v) || fabsf(v) > 20000.0f)
#define DRAW_GUARD(...) do { \
    float g_[] = {__VA_ARGS__}; \
    for (unsigned g_i = 0; g_i < sizeof g_ / sizeof *g_; g_i++) { \
      if (!BAD1(g_[g_i])) continue; \
      if (bad_draws++ < 40) { LOG("bad draw in %s from %p: arg %u = %f\n", __func__, __builtin_return_address(0), g_i, (double)g_[g_i]); } \
      return; \
    } \
  } while (0)
static void text(int x, int y, unsigned col, int size, const char *s) {
  if (x < -4000 || x > 4000 || y < -4000 || y > 4000 || !s) return; // (off anywhere sensible: a bad number)
  size = TS(size);
  if (pgf && has_wide(s)) vita2d_pgf_draw_text(pgf, x, y, col, size / 17.0f, s);
  else vita2d_font_draw_text(font_at(size), x, y, col, size, s);
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
  // (where it's cut, remembered: the same line cut the same way every frame was measured anew)
  static struct { unsigned h; short size, maxw, cut; } fit_cache[64];
  unsigned h = 2166136261u ^ (unsigned)size * 31u ^ (unsigned)max_w * 131u ^ ((unsigned)text_pct << 22);
  for (size_t k = 0; k < n; k++) h = (h ^ (unsigned char)s[k]) * 16777619u;
  int slot = h & 63;
  if (fit_cache[slot].h == h && fit_cache[slot].size == size && fit_cache[slot].maxw == max_w && (size_t)fit_cache[slot].cut <= n) {
    size_t lo = (size_t)fit_cache[slot].cut;
    if (lo > sizeof b - 4) lo = sizeof b - 4;
    memcpy(b, s, lo);
    memcpy(b + lo, "\u2026", 4);
    text(x, y, col, size, b);
    return;
  }
  if (n > sizeof b - 4) n = sizeof b - 4;
  // (the longest start that fits with "…" after it: found by halving, a few measures, not one per
  //  letter; whole UTF-8 characters)
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = (lo + hi + 1) / 2, m = mid;
    while (m > 0 && ((unsigned char)s[m] & 0xC0) == 0x80) m--;
    memcpy(b, s, m);
    memcpy(b + m, "…", 4);
    if (text_w_raw(size, b) <= max_w) lo = mid; else hi = mid - 1;
  }
  while (lo > 0 && ((unsigned char)s[lo] & 0xC0) == 0x80) lo--;
  fit_cache[slot].h = h; fit_cache[slot].size = (short)size; fit_cache[slot].maxw = (short)max_w; fit_cache[slot].cut = (short)lo;
  memcpy(b, s, lo);
  memcpy(b + lo, "…", 4);
  text(x, y, col, size, b);
}
static void text_center(int cx, int y, unsigned col, int size, const char *s) { text(cx - text_w(size, s) / 2, y, col, size, s); }
// the ○ button, as a hint: one ring (the font's own ○, nothing behind it)
static void btn_hint_circle(int cx, int cy, unsigned col) { text_center(cx, cy + 5, col, 14, "\u25cb"); }

// Circles and round corners from one small anti-aliased disc texture: a single quad each, where
// vita2d's own filled circle is a fan of 100 triangles out of its per-frame vertex pool (a busy
// screen of cards and chips would ask for hundreds of those every frame).
static vita2d_texture *disc;
#define DISC 64
static void make_disc(void) {
  disc = tex_new(DISC, DISC);
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
  DRAW_GUARD(cx, cy, r);
  if (r <= 0) return;
  if (!disc) { vita2d_draw_fill_circle(cx, cy, r, c); return; }
  float s = r * 2 / DISC;
  vita2d_draw_texture_tint_scale(disc, cx - r, cy - r, s, s, c);
}
// The squircle's corner: a superellipse (|x|^3.5 + |y|^3.5 = 1) quarter, the continuous curve of iOS's
// buttons, where a circle's quarter meets the straight edge with a corner you can see.
static vita2d_texture *sqdisc;
static void make_sqdisc(void) {
  sqdisc = tex_new(DISC, DISC);
  if (!sqdisc) return;
  uint8_t *d = vita2d_texture_get_datap(sqdisc);
  unsigned stride = vita2d_texture_get_stride(sqdisc);
  const float R = DISC / 2.0f;
  for (int y = 0; y < DISC; y++)
    for (int x = 0; x < DISC; x++) {
      // (4 x 4 samples a pixel: a smooth edge)
      int in = 0;
      for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
          float dx = fabsf(x + (sx + 0.5f) / 4 - R) / R, dy = fabsf(y + (sy + 0.5f) / 4 - R) / R;
          in += powf(dx, 3.5f) + powf(dy, 3.5f) <= 1; // (3.5: rounder than iOS's 5, still not a circle's 2)
        }
      uint8_t *p = d + y * stride + x * 4;
      p[0] = p[1] = p[2] = 255;
      p[3] = (uint8_t)(in * 255 / 16);
    }
  vita2d_texture_set_filters(sqdisc, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
}
// A rounded rectangle with circular corners: the tab bar's pill and its bubble, which are round
static void rrect_round(float x, float y, float w, float h, float r, unsigned c);
// A rounded rectangle, its corners squircles. A pill (a chip, a button rounded right round) is made a
// squircle-cornered box: it reads as the box you can press, its corners no longer cut away.
static void rrect(float x, float y, float w, float h, float r, unsigned c) {
  if (r * 2 >= h - 0.5f && w > h * 1.25f && h >= 16) r = h * 0.42f;
  else r *= 1.25f; // (rounder corners all round)
  if (!sqdisc) { rrect_round(x, y, w, h, r, c); return; }
  DRAW_GUARD(x, y, w, h, r);
  if (r * 2 > h) r = h / 2;
  if (r * 2 > w) r = w / 2;
  if (r < 1) { vita2d_draw_rectangle(x, y, w, h, c); return; }
  vita2d_draw_rectangle(x + r, y, w - 2 * r, h, c);
  vita2d_draw_rectangle(x, y + r, r, h - 2 * r, c);
  vita2d_draw_rectangle(x + w - r, y + r, r, h - 2 * r, c);
  float s = r / (DISC / 2.0f), q = DISC / 2.0f;
  vita2d_draw_texture_tint_part_scale(sqdisc, x, y, 0, 0, q, q, s, s, c);
  vita2d_draw_texture_tint_part_scale(sqdisc, x + w - r, y, q, 0, q, q, s, s, c);
  vita2d_draw_texture_tint_part_scale(sqdisc, x, y + h - r, 0, q, q, q, s, s, c);
  vita2d_draw_texture_tint_part_scale(sqdisc, x + w - r, y + h - r, q, q, q, q, s, s, c);
}
static void rrect_round(float x, float y, float w, float h, float r, unsigned c) {
  DRAW_GUARD(x, y, w, h, r);
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
// A rounded outline (a ring 64 px across, 3 px thick, radius 16), drawn 9-sliced to any size: the
// focus ring, on top of whatever it marks (it never covers the cards next to it while it moves).
static unsigned frame_no;
// How long the last frame took, in 60ths of a second (1 at 60 fps; 2 for a frame that took two): the
// motion goes by the clock, not by frames, so a slow frame moves things as far as two quick ones
// would, never a stutter backwards or a slowed animation.
static float ft = 1;
static float frame_len_ms = 16.667f; // (the last frame's length, uncapped)
static uint64_t frame_t0; // (when this frame's work began)
// Whether this frame has done enough: pictures that came in wait for the next one, so a batch of
// them never makes one frame late (the first each frame always goes in: they keep coming)
static int frame_busy(void) { return sceKernelGetProcessTimeWide() - frame_t0 > 5000; }
static float ease(float k) { return ft == 1 ? k : 1 - powf(1 - k, ft); }  // (x += (target - x) * ease(k))
static float decay(float d) { return ft == 1 ? d : powf(d, ft); }         // (v *= decay(d))
static vita2d_texture *ring_tex;
static void make_ring(void) {
  const int S = 64, R = 16, T = 3;
  ring_tex = tex_new(S, S);
  if (!ring_tex) return;
  uint8_t *d = vita2d_texture_get_datap(ring_tex);
  unsigned stride = vita2d_texture_get_stride(ring_tex);
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      // the distance to the rounded rectangle's edge, inside it
      float px = x + 0.5f, py = y + 0.5f;
      float cx = px < R ? R : px > S - R ? S - R : px, cy = py < R ? R : py > S - R ? S - R : py;
      float dist = R - sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)); // (0 at the edge, R inside)
      if (px >= R && px <= S - R) dist = fminf(py, S - py);
      if (py >= R && py <= S - R) dist = fminf(dist, fminf(px, S - px));
      float a = fminf(fminf(dist + 0.5f, 1.0f), fminf(T + 0.5f - dist, 1.0f));
      uint8_t *p = d + y * stride + x * 4;
      p[0] = p[1] = p[2] = 255;
      p[3] = a <= 0 ? 0 : (uint8_t)(a * 255);
    }
  vita2d_texture_set_filters(ring_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
}
static void ring9(float x, float y, float w, float h, unsigned c) {
  DRAW_GUARD(x, y, w, h);
  if (!ring_tex || w < 34 || h < 34) { rrect(x, y, w, 3, 1, c); rrect(x, y + h - 3, w, 3, 1, c); return; }
  const float q = 16, S = 64;
  vita2d_draw_texture_tint_part_scale(ring_tex, x, y, 0, 0, q, q, 1, 1, c);
  vita2d_draw_texture_tint_part_scale(ring_tex, x + w - q, y, S - q, 0, q, q, 1, 1, c);
  vita2d_draw_texture_tint_part_scale(ring_tex, x, y + h - q, 0, S - q, q, q, 1, 1, c);
  vita2d_draw_texture_tint_part_scale(ring_tex, x + w - q, y + h - q, S - q, S - q, q, q, 1, 1, c);
  vita2d_draw_texture_tint_part_scale(ring_tex, x + q, y, q, 0, 1, q, w - 2 * q, 1, c);
  vita2d_draw_texture_tint_part_scale(ring_tex, x + q, y + h - q, q, S - q, 1, q, w - 2 * q, 1, c);
  vita2d_draw_texture_tint_part_scale(ring_tex, x, y + q, 0, q, q, 1, 1, h - 2 * q, c);
  vita2d_draw_texture_tint_part_scale(ring_tex, x + w - q, y + q, S - q, q, q, 1, 1, h - 2 * q, c);
}
// The focus: one ring, the same on every screen, gliding to whatever is chosen (with the D-pad) and
// breathing a little. Called after drawing, with the chosen thing's rectangle.
static struct { float x, y, w, h; int scr; unsigned at; } fring = {0, 0, 0, 0, -1, 0};
static int screen_now(void);
// (the screen touched: the ring hides until a button or the stick is used again, as the PS Vita's
//  own menus do: it's for finding your place with the buttons, not in the way of a finger)
static int touch_mode;
static void focus_ring(float x, float y, float w, float h) {
  if (touch_mode) return;
  DRAW_GUARD(x, y, w, h);
  int scr = screen_now();
  if (fring.scr != scr || frame_no - fring.at > 3) { fring.x = x; fring.y = y; fring.w = w; fring.h = h; fring.scr = scr; }
  else { fring.x += (x - fring.x) * ease(0.35f); fring.y += (y - fring.y) * ease(0.35f); fring.w += (w - fring.w) * ease(0.35f); fring.h += (h - fring.h) * ease(0.35f); }
  fring.at = frame_no;
  float br = 0.5f + 0.5f * sinf(frame_no * 0.1f);
  ring9(fring.x - 6, fring.y - 6, fring.w + 12, fring.h + 12, with_alpha(ACC, (int)(60 + 50 * br)));
  ring9(fring.x - 3, fring.y - 3, fring.w + 6, fring.h + 6, ACC);
}
// A card still loading: its shape, with a light passing over it (and its ring, small, in the middle)
static void grad(float x, float y, float w, float h, unsigned col, int a0, int a1, int across);
static void skeleton(float x, float y, float w, float h, float r) {
  DRAW_GUARD(x, y, w, h);
  rrect(x, y, w, h, r, SURF);
  float band = fmaxf(40, w * 0.4f), span = w + band;
  float p = fmodf(frame_no * 6.0f + x * 0.7f + y * 0.3f, span * 1.6f) - band;
  float b0 = fmaxf(x, x + p), mid = x + p + band / 2, b1 = fminf(x + w, x + p + band);
  if (mid > b0 && mid < x + w) grad(b0, y + 2, fminf(mid, x + w) - b0, h - 4, TEXT, 0, 18, 1);
  if (b1 > mid && mid > x) grad(fmaxf(mid, x), y + 2, b1 - fmaxf(mid, x), h - 4, TEXT, 18, 0, 1);
}
static vita2d_texture *ui_icon(const char *name);
static void heart(float cx, float cy, float s, unsigned c) {
  DRAW_GUARD(cx, cy, s);
  // (the icon's filled heart, scaled: one quad, not two circles and a dozen one-pixel lines)
  vita2d_texture *hf = s > 9 ? ui_icon("heart_big") : NULL; // (a big one: drawn from a big picture, crisp)
  if (hf) { float sc = s * 2.3f / vita2d_texture_get_width(hf); vita2d_draw_texture_tint_scale(hf, cx - s * 1.15f, cy - s * 1.15f, sc, sc, c); return; }
  hf = ui_icon("heart_fill");
  if (hf) { float sc = s * 2.3f / vita2d_texture_get_width(hf); vita2d_draw_texture_tint_scale(hf, cx - s * 1.15f, cy - s * 1.1f, sc, sc, c); return; }
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
  char local_vault[512]; // the last vault opened that isn't the Google Drive one (livearea.c)
  int lmode;             // the Library's mode: 0 everything, 1 pictures, 2 videos, 3 comics (lib.c)
  int theme, bgstyle;    // the accent (0: the vault's own, else THEMES[theme - 1]) and the background (BGS)
  int sndpack;           // the interface sounds: SND_PACKS
  int conv;              // offline copies made 480p by a conversion service: 0 off (as it starts), 1 on
  int acc_h, acc_s, acc_v; // the custom accent (theme 11): hue 0-359, saturation and value 0-100
  int onboarded;           // the three cards of the first start seen (an install from before them: yes)
  // reading comics (reader.c): pages or a strip, landscape or portrait, the page turn, the direction,
  // what taps do, what a double tap does, the rear touchpad
  int cmode, corient, canim, cdir, ctap, cdbl, cback;
  // the full views (viewopt.c): pictures' size and orientation, videos' size and orientation
  int ifit, iorient, vfit, vorient;
  int tscale;  // text size, in percent: 90, 100, 112 or 125 (0: 100)
  int accpic;  // the accent follows the picture shown
  int wsize;   // the Browser's card size: 1-3 (small, medium, large); 0 not set (medium)
  int rear_l, rear_r; // the rear touchpad's halves: REAR_* (0 not set: their defaults)
  int autorot; // portrait pictures turned to fill the screen held on its side
  int nodecor; // the little anime girls (decor.c) hidden
  int notags;  // a save doesn't ask for your own tags
} cfg = {"", 0, 0, 5, 0, 100, 1, 15, 2, "", "", 0, 0, 70, 0, 1, 0, 0, 0, 0, "", 0, 0, 0, 0, 0, 330, 65, 100, 0, 0, 0, 0, 0, 0, 0, 0};
static void cfg_load(void) {
  cfg.sndpack = 3; // (Bubbly, from the start)
  FILE *f = fopen(DATA "/config.txt", "r");
  if (!f) return;
  int snd_chosen = 0; // (a sound pack picked since Bubbly became the default: kept; one from before: Bubbly)
  cfg.onboarded = 1; // (it was used before: unless it says otherwise, nothing to show)
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
    else if (!strcmp(line, "local_vault")) snprintf(cfg.local_vault, sizeof cfg.local_vault, "%s", eq);
    else if (!strcmp(line, "lmode")) cfg.lmode = atoi(eq);
    else if (!strcmp(line, "theme")) cfg.theme = atoi(eq);
    else if (!strcmp(line, "bgstyle")) cfg.bgstyle = atoi(eq);
    else if (!strcmp(line, "sndpack")) cfg.sndpack = atoi(eq);
    else if (!strcmp(line, "sndv")) snd_chosen = atoi(eq) >= 2;
    else if (!strcmp(line, "conv")) cfg.conv = atoi(eq) != 0;
    else if (!strcmp(line, "acc_h")) cfg.acc_h = atoi(eq);
    else if (!strcmp(line, "acc_s")) cfg.acc_s = atoi(eq);
    else if (!strcmp(line, "acc_v")) cfg.acc_v = atoi(eq);
    else if (!strcmp(line, "onboarded")) cfg.onboarded = atoi(eq) != 0;
    else if (!strcmp(line, "tscale")) cfg.tscale = atoi(eq);
    else if (!strcmp(line, "accpic")) cfg.accpic = atoi(eq) != 0;
    else if (!strcmp(line, "wsize")) cfg.wsize = atoi(eq);
    else if (!strcmp(line, "rear")) sscanf(eq, "%d,%d", &cfg.rear_l, &cfg.rear_r);
    else if (!strcmp(line, "autorot")) cfg.autorot = atoi(eq) != 0;
    else if (!strcmp(line, "nodecor")) cfg.nodecor = atoi(eq) != 0;
    else if (!strcmp(line, "notags")) cfg.notags = atoi(eq) != 0;
    else if (!strcmp(line, "view")) sscanf(eq, "%d,%d,%d,%d", &cfg.ifit, &cfg.iorient, &cfg.vfit, &cfg.vorient);
    else if (!strcmp(line, "comic")) sscanf(eq, "%d,%d,%d,%d,%d,%d,%d", &cfg.cmode, &cfg.corient, &cfg.canim, &cfg.cdir, &cfg.ctap, &cfg.cdbl, &cfg.cback);
  }
  fclose(f);
  if (cfg.interval < 2 || cfg.interval > 60) cfg.interval = 5;
  if (cfg.volume < 0 || cfg.volume > 150) cfg.volume = 100;
  if (cfg.site < 0 || cfg.site >= 64) cfg.site = 0;
  if (!(cfg.ratings & 15)) cfg.ratings = 15;
  if (cfg.wsort < 0 || cfg.wsort > 3) cfg.wsort = 0;
  if (cfg.wpages < 0 || cfg.wpages > 50) cfg.wpages = 0;
  if (cfg.kind < 0 || cfg.kind > 3) cfg.kind = 0;
  if (cfg.lmode < 0 || cfg.lmode > 3) cfg.lmode = 0;
  if (cfg.theme < 0 || cfg.theme > NTHEMES + 1) cfg.theme = 0;
  cfg.acc_h = ((cfg.acc_h % 360) + 360) % 360;
  { int *c[] = {&cfg.cmode, &cfg.corient, &cfg.canim, &cfg.cdir, &cfg.ctap, &cfg.cdbl, &cfg.cback}; static const int top[] = {1, 1, 3, 1, 2, 2, 1};
    for (int k = 0; k < 7; k++) if (*c[k] < 0 || *c[k] > top[k]) *c[k] = 0; }
  { int *c[] = {&cfg.ifit, &cfg.iorient, &cfg.vfit, &cfg.vorient}; static const int top[] = {3, 1, 4, 1};
    for (int k = 0; k < 4; k++) if (*c[k] < 0 || *c[k] > top[k]) *c[k] = 0; }
  if (cfg.acc_s < 0 || cfg.acc_s > 100) cfg.acc_s = 65;
  if (cfg.tscale != 90 && cfg.tscale != 112 && cfg.tscale != 125) cfg.tscale = 100;
  if (cfg.acc_v < 25 || cfg.acc_v > 100) cfg.acc_v = 100;
  if (cfg.bgstyle < 0 || cfg.bgstyle > 3) cfg.bgstyle = 0;
  if (!snd_chosen || cfg.sndpack < 0 || cfg.sndpack > 3) cfg.sndpack = 3;
  if (cfg.sort < 0 || cfg.sort >= 10) cfg.sort = 0;
  if (cfg.wsel < 0) cfg.wsel = 0;
  if (cfg.sounds < 0 || cfg.sounds > 100) cfg.sounds = 70;
  if (cfg.wspan < 0 || cfg.wspan >= LV_NSPANS) cfg.wspan = 0;
}
static unsigned cfg_dirty_at; // (a change waiting to be written: when the last one was)
static void cfg_write(void);
// (every change marks it; it's written a second after the last one, not on every press of a held
//  button, and at once before the app is put away: flush_settings)
static void cfg_save(void) { cfg_dirty_at = frame_no ? frame_no : 1; }
static void cfg_write(void) {
  cfg_dirty_at = 0;
  FILE *f = fopen(DATA "/config.txt.part", "w");
  if (!f) return;
  fprintf(f, "vault=%s\nsort=%d\nkind=%d\ninterval=%d\nshuffle=%d\nvolume=%d\nsite=%d\nratings=%d\nwsort=%d\nwquery=%s\nsave_cat=%s\nwpages=%d\nwsel=%d\nsounds=%d\ndbltap=%d\nglow=%d\nwspan=%d\nwfull=%d\nhide=%d\ntlock=%d\nlocal_vault=%s\nlmode=%d\ntheme=%d\nbgstyle=%d\nsndpack=%d\nconv=%d\nacc_h=%d\nacc_s=%d\nacc_v=%d\nonboarded=%d\ncomic=%d,%d,%d,%d,%d,%d,%d\nview=%d,%d,%d,%d\ntscale=%d\naccpic=%d\nsndv=2\nwsize=%d\nrear=%d,%d\nautorot=%d\nnodecor=%d\nnotags=%d\n",
          cfg.vault, cfg.sort, cfg.kind, cfg.interval, cfg.shuffle, cfg.volume, cfg.site, cfg.ratings, cfg.wsort, cfg.wquery, cfg.save_cat,
          cfg.wpages, cfg.wsel, cfg.sounds, cfg.dbltap, cfg.glow, cfg.wspan, cfg.wfull, cfg.hide, cfg.tlock, cfg.local_vault, cfg.lmode, cfg.theme, cfg.bgstyle, cfg.sndpack, cfg.conv, cfg.acc_h, cfg.acc_s, cfg.acc_v, cfg.onboarded,
          cfg.cmode, cfg.corient, cfg.canim, cfg.cdir, cfg.ctap, cfg.cdbl, cfg.cback, cfg.ifit, cfg.iorient, cfg.vfit, cfg.vorient, cfg.tscale, cfg.accpic, cfg.wsize, cfg.rear_l, cfg.rear_r, cfg.autorot, cfg.nodecor, cfg.notags);
  fclose(f);
  sceIoRemove(DATA "/config.txt");
  sceIoRename(DATA "/config.txt.part", DATA "/config.txt");
}

// ---------- the vault, and what the library shows ----------
static LvVault vault;
static int loaded_vault;
static int *shown, nshown;     // indices into vault.items
static float scroll, scroll_v; // (the Library's)
static int cat = -1;           // -1: every category
static char query[128];
// (their numbers are kept in config.txt: new ones only ever go on the end)
static const char *SORTS[] = {"Newest", "Oldest", "Most viewed", "Top rated", "Most appreciated", "Shuffle", "For you",
                              "Least viewed", "Recently viewed", "Most time spent", "Largest files"};
#define NSORTS 11
// ----- storage: what the app takes on the card (the vault's files, and its own data: caches, offline
// comics, the video being played), measured on a thread; each item's size kept for "Largest files" -----
static int64_t *item_bytes; static int item_bytes_n;
static volatile int store_state; // 0 not measured, 1 measuring, 2 done
static volatile int64_t store_vault, store_data;
static int64_t dir_bytes(const char *dir, int depth) {
  DIR *d = opendir(dir);
  if (!d) return 0;
  int64_t t = 0;
  struct dirent *e;
  char p[600];
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) { if (depth < 6) t += dir_bytes(p, depth + 1); }
    else t += st.st_size;
  }
  closedir(d);
  return t;
}
static void *store_main(void *arg) {
  (void)arg;
  int n = vault.n;
  int64_t *b = calloc(n + 1, sizeof *b);
  int64_t sum = 0;
  for (int i = 0; b && i < n && i < vault.n; i++) {
    struct stat st;
    if (vault.items[i].path && stat(vault.items[i].path, &st) == 0) { b[i] = st.st_size; sum += st.st_size; }
  }
  store_data = dir_bytes(DATA, 0);
  store_vault = sum + (vault.root[0] ? 0 : 0);
  static pthread_mutex_t sm = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&sm);
  int64_t *old = item_bytes; item_bytes = b; item_bytes_n = b ? n : 0;
  pthread_mutex_unlock(&sm);
  (void)old; // (the old table left: a sort may be reading it this moment; small, measured rarely)
  store_state = 2;
  return NULL;
}
static void store_measure(void) {
  if (store_state == 1) return;
  store_state = 1;
  pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 * 1024);
  if (pthread_create(&t, &at, store_main, NULL) == 0) pthread_detach(t); else store_state = 0;
}
static int64_t bytes_of(int i) { return item_bytes && i >= 0 && i < item_bytes_n ? item_bytes[i] : 0; }
static int lib_pass(int i);
static void lib_prepare(void);
static int mode_pass(int i);
static int lib_books(void);
static int book_shelf, book_inside; // (lib.c: the Comics mode's shelf of books, or one book's pages)
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
    case 7: d = views_at(x->view) - views_at(y->view); break; // (least viewed first)
    case 8: { // (the one looked at last first; never looked at: last)
      const char *lx = x->view >= 0 && vault.views[x->view].last ? vault.views[x->view].last : "";
      const char *ly = y->view >= 0 && vault.views[y->view].last ? vault.views[y->view].last : "";
      int c = strcmp(ly, lx);
      if (c) return c;
      break;
    }
    case 10: { int64_t bx = bytes_of(*(const int *)a), by = bytes_of(*(const int *)b); d = by > bx ? 1 : by < bx ? -1 : 0; break; } // (largest first)
    case 9: { double sx = x->view >= 0 ? vault.views[x->view].seconds : 0, sy = y->view >= 0 ? vault.views[y->view].seconds : 0; d = sy > sx ? 1 : sy < sx ? -1 : 0; break; }
  }
  if (d) return d > 0 ? 1 : -1;
  d = y->added - x->added;
  return d > 0 ? 1 : d < 0 ? -1 : 0;
}
static Grid lgrid;
static int is_cover_in_all(int i); // (lib.c)
static float lib_aspect(int k) {
  if (book_shelf || is_cover_in_all(shown[k])) return 0.7f; // (a book: a cover's shape)
  const LvItem *it = &vault.items[shown[k]];
  float a = it->width > 0 && it->height > 0 ? (float)it->width / it->height : 0.75f;
  return a < 0.5f ? 0.5f : a; // (a webtoon's strip: a card, not a sliver; the card shows its top)
}

static int cap_shown;
static void lhero_build(void); // (the hero row over All: after the grid is made, it goes below it)
static void lhero_offset(void);
static int refilter_wanted; // (items came in: the Library filtered again at the next frame)
static int smart; // (a smart collection chosen: lib.c)
static int smart_pass(int i);
static int cmp_views_desc(const void *a, const void *b);
static void refilter(void) {
  // (the list's room kept from one filter to the next: grown only when the vault has grown)
  if (!shown || cap_shown < vault.n + 1) {
    int *ns = realloc(shown, (vault.n + 1) * sizeof *shown);
    if (ns) { shown = ns; cap_shown = vault.n + 1; } else { free(shown); shown = NULL; cap_shown = 0; }
  }
  nshown = 0;
  if (!shown) { grid_build(&lgrid, 0, NULL); return; }
  lib_prepare();
  if (lib_books()) { grid_build(&lgrid, nshown, lib_aspect); return; } // (the Comics mode: books, or a book's pages in order)
  for (int i = 0; i < vault.n; i++) {
    if (cat >= 0 && vault.hot[i].cat != cat) continue;
    if (!mode_pass(i) || !lib_pass(i)) continue;
    if (smart && !smart_pass(i)) continue;
    shown[nshown++] = i;
  }
  sort_mode = cfg.sort;
  if (smart == 3) qsort(shown, nshown, sizeof *shown, cmp_views_desc); // (Most viewed: by its views)
  else if (cfg.sort == 6) sort_foryou();
  else if (cfg.sort == 5) {
    for (int k = nshown - 1; k > 0; k--) { int j = rnd() % (k + 1), t = shown[k]; shown[k] = shown[j]; shown[j] = t; }
  } else {
    qsort(shown, nshown, sizeof *shown, cmp_items);
  }
  grid_build(&lgrid, nshown, lib_aspect);
  lhero_build();
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

static int cache_broken, cache_proven; // (the encoder found wrong, or found right three times)
// The average difference of a sample of pixels, small enough for a lossy copy of the same picture.
static int pixels_close(const LvPixels *a, const LvPixels *b) {
  long long diff = 0, n = 0;
  size_t total = (size_t)a->w * a->h;
  for (size_t k = 0; k < total; k += 7, n++)
    for (int c = 0; c < 3; c++) diff += abs(a->rgba[k * 4 + c] - b->rgba[k * 4 + c]);
  return n && diff / (n * 3) < 24;
}

// A picture decoded to exactly w x h: made just big enough to cover it, then cropped (a little above
// the middle, where faces tend to be). A picture smaller than that stays its own size, cropped to the
// shape: the GPU stretches it, nothing is made up.
static int decode_exact(const uint8_t *b, size_t n, int w, int h, LvPixels *out) {
  int iw = 0, ih = 0;
  if (!lv_image_size(b, n, &iw, &ih) || iw <= 0 || ih <= 0) return -1;
  float s = fmaxf(w / (float)iw, h / (float)ih);
  int dw = s < 1 ? (int)ceilf(iw * s) : iw, dh = s < 1 ? (int)ceilf(ih * s) : ih;
  LvPixels px;
  if (lv_decode(b, n, dw, dh, &px) != 0) return -1;
  // the crop: the card's shape, at most the card's size
  float sh = fminf(px.w / (float)w, px.h / (float)h);
  int cw = (int)(w * sh), ch = (int)(h * sh);
  if (cw > px.w) cw = px.w;
  if (ch > px.h) ch = px.h;
  if (cw < 1 || ch < 1) { free(px.rgba); return -1; }
  int ox = (px.w - cw) / 2, oy = (int)((px.h - ch) * 0.3f);
  uint8_t *o = malloc((size_t)cw * ch * 4);
  if (!o) { free(px.rgba); return -1; }
  for (int y = 0; y < ch; y++) memcpy(o + (size_t)y * cw * 4, px.rgba + ((size_t)(y + oy) * px.w + ox) * 4, (size_t)cw * 4);
  free(px.rgba);
  out->rgba = o;
  out->w = cw;
  out->h = ch;
  return 0;
}

// The size each item's card wants its thumbnail at (0: a cell's), set as the screen asks for it
static unsigned short *tsize_w, *tsize_h;
static int thumb_pixels(int i, LvPixels *px, int want_w, int want_h) {
  LvItem *it = &vault.items[i];
  char cp[800];
  // (exactly the card's pixels: the card's size, the picture covering it and cropped; cached at it)
  int cw = want_w > 0 ? want_w : CELL_W, ch = want_h > 0 ? want_h : CELL_H;
  snprintf(cp, sizeof cp, "%s/%08lx%08lx-%dx%d.jpg", cache_dir, fnv(it->thumb), fnv(it->id), cw, ch);
  size_t n = 0;
  uint8_t *b = lv_read_all(cp, lv_key(&vault), &n);
  if (b) {
    int r = lv_decode(b, n, cw, ch, px);
    free(b);
    if (r == 0) return 0;
  }
  b = lv_thumb_bytes(&vault, it, &n);
  if (!b && it->path && !is_video(it) && !is_gif(it)) b = lv_read_all(it->path, lv_key(&vault), &n); // (no thumbnail: the picture itself)
  if (!b) return -1;
  int sw = 0, shh = 0, small = lv_image_size(b, n, &sw, &shh) && sw <= cw * 3 / 2 && shh <= ch * 3 / 2;
  int r = decode_exact(b, n, cw, ch, px);
  free(b);
  if (r != 0) return -1;
  if (small) return 0; // (already card-sized, like the Browser's saves: nothing to gain from a copy)
  // Kept small for next time, once the copy is known to come back right: it is decoded again and
  // compared, and if the encoder ever gets it wrong (Vita3K's does) nothing more is cached.
  if (!cache_broken) {
    size_t en = 0;
    uint8_t *enc = lv_encode_jpeg(px, 85, &en);
    if (enc && cache_proven >= 3) lv_write_all(cp, lv_key(&vault), enc, en); // (the encoder's been right: no decoding again to check)
    else {
      LvPixels back;
      if (enc && lv_decode(enc, en, px->w, px->h, &back) == 0) {
        int ok = back.w == px->w && back.h == px->h && pixels_close(px, &back);
        free(back.rgba);
        if (ok) { lv_write_all(cp, lv_key(&vault), enc, en); cache_proven++; }
        else cache_broken = 1;
      }
    }
    free(enc);
  }
  return 0;
}

static void *worker(void *arg) {
  pin_to_core(1); // (off the drawing thread's core)
  while (worker_run) {
    pthread_mutex_lock(&mx);
    // (asleep with nothing to do, or while its finished ones haven't been taken: the drawing thread
    //  wakes it when it takes some, rather than it looking every few milliseconds)
    while (worker_run && big_ask.item < 0 && (nwant == 0 || ndone >= (int)(sizeof done / sizeof *done))) pthread_cond_wait(&cv, &mx);
    if (!worker_run) { pthread_mutex_unlock(&mx); break; }
    int item = -1, big = 0, gen = 0, mw = 0, mh = 0;
    if (big_ask.item >= 0) {
      item = big_ask.item; gen = big_ask.gen; mw = big_ask.max_w; mh = big_ask.max_h; big = 1;
      big_ask.item = -1;
    } else {
      item = want[0];
      memmove(want, want + 1, --nwant * sizeof *want);
      if (tsize_w && item >= 0 && item < vault.n) { mw = tsize_w[item]; mh = tsize_h[item]; }
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
      // (a video's or a GIF's own file isn't a picture to decode: reading it whole, maybe hundreds of
      //  megabytes, only to fail was slow and could run the memory out; its thumbnail is the picture)
      int moving = !strcmp(it->kind, "video") || !strcmp(it->kind, "gif");
      uint8_t *b = it->path && !moving ? lv_read_all(it->path, lv_key(&vault), &n) : NULL;
      if (b) { r = lv_decode(b, n, mw, mh, &px); free(b); }
      if (r != 0) { b = lv_thumb_bytes(&vault, it, &n); if (b) { r = lv_decode(b, n, mw, mh, &px); free(b); } }
    } else {
      r = thumb_pixels(item, &px, mw, mh);
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
static unsigned *pending_at; // (the frame each was let go in)
static int npending, cappending;
static pthread_mutex_t fmx = PTHREAD_MUTEX_INITIALIZER; // (a video closing on its own thread frees too)
static unsigned free_backdate; // (a part off screen for 3 frames or more: its textures can go at once)
static void free_later(vita2d_texture *t) {
  if (!t) return;
  pthread_mutex_lock(&fmx);
  if (npending == cappending) {
    int cap = cappending ? cappending * 2 : 64;
    vita2d_texture **p = realloc(pending_free, cap * sizeof *p);
    if (p) pending_free = p;
    unsigned *pa = p ? realloc(pending_at, cap * sizeof *pa) : NULL;
    if (pa) pending_at = pa;
    if (!p || !pa) { pthread_mutex_unlock(&fmx); return; } // (a leak, not a crash)
    cappending = cap;
  }
  pending_at[npending] = frame_no - free_backdate;
  pending_free[npending++] = t;
  pthread_mutex_unlock(&fmx);
}
// textures let go three frames ago or more: the GPU is done with them (vita2d has two frames in
// flight at most), so they're freed without waiting for it
static void free_due(void) {
  pthread_mutex_lock(&fmx);
  int kept = 0;
  for (int k = 0; k < npending; k++) {
    if (frame_no - pending_at[k] >= 3) vita2d_free_texture(pending_free[k]);
    else { pending_free[kept] = pending_free[k]; pending_at[kept++] = pending_at[k]; }
  }
  npending = kept;
  pthread_mutex_unlock(&fmx);
}
static vita2d_texture *upload(const LvPixels *px) {
  vita2d_texture *t = tex_new(px->w, px->h);
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
#define ATLASES 8 // (cells 0-71 the Library's, 72-143 the Browser's: WSLOTS, web.c, is 72)
static vita2d_texture *atlas[ATLASES];
static vita2d_texture cells[ATLASES * CELLS_PER_ATLAS];
// (each atlas made the first time a cell of it is used: the Browser's four, 16 MB of video memory,
//  only once the Browser shows something)
static void atlas_make(int a) {
  if (a < 0 || a >= ATLASES || atlas[a]) return;
  {
    atlas[a] = tex_new(1024, 1024);
    if (!atlas[a]) return;
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
// ---------- pools: covers and cards in shared atlases, not a texture each ----------
// The comics' covers, the video browser's cards and Favourites' covers: each was a texture of its own,
// made and freed as they scrolled (a 256 KiB granule of video memory each, and the memory manager
// asked every time). A pool cuts 1024 x 1024 atlases into cells of one size; a picture goes into a free
// cell (shrunk to fit, if it's bigger), and its "texture" is a view onto that cell. A cell let go is
// used again only three frames on (the GPU may still be drawing the old picture).
#define POOL_ATL 8
#define MG_POOL_W 148 // (the comics' covers: MG_CW x MG_CH, manga_ui.c)
#define MG_POOL_H 210
#define VB_POOL_W 190 // (the video browser's cards: VB_CW x VB_CH, vbrowse.c)
#define VB_POOL_H 107
#define FAV_POOL_W 216 // (Favourites: a video's card or a comic's cover, whichever's bigger each way)
#define FAV_POOL_H 210
#define POOL_CELLS 256
typedef struct {
  int cw, ch, cols, per;                 // (a cell's size; cells across and in each atlas)
  vita2d_texture *at[POOL_ATL];
  vita2d_texture cell[POOL_CELLS];
  unsigned char used[POOL_CELLS];
  unsigned freed_at[POOL_CELLS];
} Pool;
static Pool pool_covers = {MG_POOL_W, MG_POOL_H}, pool_cards = {VB_POOL_W, VB_POOL_H}, pool_favs = {FAV_POOL_W, FAV_POOL_H};
static int pool_owns(const Pool *p, const vita2d_texture *t) { return t >= p->cell && t < p->cell + POOL_CELLS; }
static vita2d_texture *pool_put(Pool *p, const LvPixels *px) {
  if (!px->rgba || px->w <= 0 || px->h <= 0) return NULL;
  if (!p->per) { p->cols = 1024 / (p->cw + 2); p->per = p->cols * (1024 / (p->ch + 2)); }
  int n = p->per * POOL_ATL < POOL_CELLS ? p->per * POOL_ATL : POOL_CELLS, k = -1;
  for (int i = 0; i < n && k < 0; i++) if (!p->used[i] && frame_no - p->freed_at[i] >= 3) k = i;
  if (k < 0) return upload(px); // (full: a texture of its own, as before)
  int a = k / p->per, c = k % p->per;
  if (!p->at[a] && !(p->at[a] = tex_new(1024, 1024))) return upload(px);
  unsigned stride = vita2d_texture_get_stride(p->at[a]);
  uint8_t *d = (uint8_t *)vita2d_texture_get_datap(p->at[a]) + (size_t)((c / p->cols) * (p->ch + 2)) * stride + (size_t)((c % p->cols) * (p->cw + 2)) * 4;
  float s = fminf(1.0f, fminf((float)p->cw / px->w, (float)p->ch / px->h));
  int w = (int)(px->w * s), h = (int)(px->h * s);
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  for (int y = 0; y < h; y++) {
    int yy = s == 1.0f ? y : (int)((y + 0.5f) / s);
    if (yy >= px->h) yy = px->h - 1;
    const uint32_t *srow = (const uint32_t *)(px->rgba + (size_t)yy * px->w * 4);
    uint32_t *drow = (uint32_t *)(d + (size_t)y * stride);
    if (s == 1.0f) { memcpy(drow, srow, (size_t)w * 4); continue; }
    for (int x = 0; x < w; x++) { int xx = (int)((x + 0.5f) / s); drow[x] = srow[xx < px->w ? xx : px->w - 1]; }
  }
  vita2d_texture *t = &p->cell[k];
  memset(t, 0, sizeof *t);
  sceGxmTextureInitLinearStrided(&t->gxm_tex, d, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, w, h, stride);
  sceGxmTextureSetMinFilter(&t->gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
  sceGxmTextureSetMagFilter(&t->gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
  p->used[k] = 1;
  return t;
}
static int pool_free(Pool *p, vita2d_texture *t) {
  if (!pool_owns(p, t)) return 0;
  int k = (int)(t - p->cell);
  p->used[k] = 0; p->freed_at[k] = frame_no;
  return 1;
}
static int pool_free_any(vita2d_texture *t) { return pool_free(&pool_covers, t) || pool_free(&pool_cards, t) || pool_free(&pool_favs, t); }
// (a part let go: its pool's atlases too; its owner has forgotten its cells)
static void pool_release(Pool *p) {
  for (int a = 0; a < POOL_ATL; a++) if (p->at[a]) { free_later(p->at[a]); p->at[a] = NULL; }
  memset(p->used, 0, sizeof p->used);
}
static void atlas_init(void) { atlas_make(0); } // (the first of the Library's at the start; the rest as needed)
// A thumbnail into its cell, whole: it was decoded to fit CELL_W x CELL_H, so it's copied as it is
// (scaled down only if it somehow isn't), and the cell's view takes its size.
static vita2d_texture *put_cell(int cell, const LvPixels *px) {
  if (cell < 0 || cell >= ATLASES * CELLS_PER_ATLAS) return NULL; // (never past the atlases: it used to be)
  atlas_make(cell / CELLS_PER_ATLAS);
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
static vita2d_texture *upload(const LvPixels *px);
static void free_later(vita2d_texture *t);

// Screen-size pictures (the full views'): copied into one of three buffers made once, a view of exactly
// the picture's size onto it (no GPU memory made or freed per picture; three, so one is never written
// while the GPU may still draw it). Larger ones (zoomed in) get a texture of their own.
static vita2d_texture *scr_buf[6];
static vita2d_texture scr_view[6];
static int scr_next[2];
static int is_view(const vita2d_texture *t) { return t >= scr_view && t < scr_view + 6; }
static int pool_free_any(vita2d_texture *t);
static void release_tex(vita2d_texture *t) { if (t && !is_view(t) && !pool_free_any(t)) free_later(t); }
// set: 0 the Library's full view, 1 the Browser's
static vita2d_texture *screen_upload(int set, const LvPixels *px) {
  if (!px->rgba || px->w <= 0 || px->h <= 0) return NULL;
  if (px->w > W || px->h > H) return upload(px);
  int k = set * 3 + scr_next[set];
  scr_next[set] = (scr_next[set] + 1) % 3;
  if (!scr_buf[k]) {
    scr_buf[k] = tex_new(W, H);
    if (!scr_buf[k]) return upload(px);
  }
  unsigned stride = vita2d_texture_get_stride(scr_buf[k]);
  uint8_t *d = vita2d_texture_get_datap(scr_buf[k]);
  for (int y = 0; y < px->h; y++) memcpy(d + (size_t)y * stride, px->rgba + (size_t)y * px->w * 4, (size_t)px->w * 4);
  vita2d_texture *v = &scr_view[k];
  *v = *scr_buf[k];
  sceGxmTextureInitLinearStrided(&v->gxm_tex, d, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, px->w, px->h, stride);
  sceGxmTextureSetMinFilter(&v->gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
  sceGxmTextureSetMagFilter(&v->gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
  return v;
}
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
  if (!ndone) return; // (nothing decoded since: no lock taken)
  pthread_mutex_lock(&mx);
  // (at most six a frame, two while the grid is flung fast: the rest the next, so a flood of them
  //  never holds a frame up; and none more once this frame has spent 3 ms on them)
  int cap = fabsf(scroll_v) > 30 ? 2 : 6;
  int n = ndone < cap ? ndone : cap;
  Done got[16];
  memcpy(got, done, n * sizeof *got);
  memmove(done, done + n, (ndone - n) * sizeof *done);
  ndone -= n;
  if (n && nwant) pthread_cond_signal(&cv); // (room again: the worker carries on)
  pthread_mutex_unlock(&mx);
  uint64_t t0 = sceKernelGetProcessTimeWide();
  for (int k = 0; k < n; k++) {
    Done *d = &got[k];
    if (k > 0 && !d->big && sceKernelGetProcessTimeWide() - t0 > 3000) {
      // (out of time: back to the front of the queue for the next frame)
      pthread_mutex_lock(&mx);
      int rest = n - k;
      if (ndone + rest <= 16) { memmove(done + rest, done, ndone * sizeof *done); memcpy(done, got + k, rest * sizeof *got); ndone += rest; }
      else for (int j = k; j < n; j++) { if (got[j].item < vault.n && got[j].item >= 0 && !got[j].big) tstate[got[j].item] = 0; free(got[j].px.rgba); }
      pthread_mutex_unlock(&mx);
      break;
    }
    if (d->vgen != vgen || d->item >= vault.n) { free(d->px.rgba); continue; }
    if (d->big) {
      if (d->gen == big_gen) {
        if (xfade_wanted && big_tex && big_item != d->item) { release_tex(fade_tex); fade_tex = big_tex; fade_at = frame_no; }
        else release_tex(big_tex);
        big_tex = d->px.rgba ? screen_upload(0, &d->px) : NULL;
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
// The size a card is drawn at, for its thumbnail (at most a cell's; the same shape)
static void want_size(int item, float w, float h) {
  if (!tsize_w || item < 0 || item >= vault.n || w < 1 || h < 1) return;
  float s = fminf(1, fminf(CELL_W / w, CELL_H / h));
  tsize_w[item] = (unsigned short)(w * s + 0.5f);
  tsize_h[item] = (unsigned short)(h * s + 0.5f);
}
static void ask_thumbs(const int *items, int n) {
  // (the same list as last frame, the usual case: nothing to do, no lock)
  static int last[64], nlast = -1;
  if (n == nlast && !memcmp(items, last, n * sizeof *items)) {
    int unasked = 0; // (one let go meanwhile, its request dropped: asked again)
    for (int k = 0; k < n && !unasked; k++) unasked = tstate[items[k]] == 0;
    if (!unasked) return;
  }
  nlast = n < 64 ? n : 64;
  memcpy(last, items, nlast * sizeof *items);
  pthread_mutex_lock(&mx);
  for (int k = 0; k < nwant; k++) if (tstate[want[k]] == 1) tstate[want[k]] = 0;
  nwant = 0;
  int fresh = 0;
  for (int k = 0; k < n && nwant < 64; k++) {
    if (tstate[items[k]] == 0) { tstate[items[k]] = 1; want[nwant++] = items[k]; fresh = 1; }
  }
  if (fresh) pthread_cond_signal(&cv); // (the worker woken only when there's something for it)
  pthread_mutex_unlock(&mx);
}
static int big_hi, big_hi_item = -1;
static int is_tall(const LvItem *it) { return it->width > 0 && it->height > it->width * 2.2f && strcmp(it->kind, "video"); }
static void ask_big(int item) {
  pthread_mutex_lock(&mx);
  big_gen++;
  big_ask.item = item;
  big_ask.gen = big_gen;
  big_ask.max_w = big_hi ? 1920 : W; // (the screen's pixels; twice that only once zoomed in)
  big_ask.max_h = big_hi ? 1920 : H;
  // (a tall page, a webtoon's strip: read at the screen's width, scrolling down, so it's decoded to
  //  that width, as tall as a texture can be)
  if (item >= 0 && item < vault.n && is_tall(&vault.items[item])) { big_ask.max_w = W; big_ask.max_h = 4096; }
  // (the decode and its copy must leave the heap under 95%: else the screen's size, which always fits)
  if ((size_t)big_ask.max_w * big_ask.max_h * 8 > 0 && !heap_room((size_t)big_ask.max_w * big_ask.max_h * 8)) { big_ask.max_w = W; big_ask.max_h = H; }
  big_failed = 0;
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mx);
}
static void drop_textures(void) {
  vita2d_wait_rendering_done();
  for (int k = 0; k < SLOTS; k++) { slots[k].tex = NULL; slots[k].item = -1; slots[k].used = 0; } // (the cells stay: the atlases are for good)
  if (big_tex && !is_view(big_tex)) vita2d_free_texture(big_tex);
  big_tex = NULL;
  big_item = -1;
  if (fade_tex && !is_view(fade_tex)) vita2d_free_texture(fade_tex);
  fade_tex = NULL;
  pthread_mutex_lock(&fmx);
  for (int k = 0; k < npending; k++) vita2d_free_texture(pending_free[k]);
  npending = 0;
  pthread_mutex_unlock(&fmx);
}

// Draws a texture to cover a box (cropped to fill it, like object-fit: cover).
static void draw_cover(vita2d_texture *t, float x, float y, float w, float h, unsigned tint) {
  DRAW_GUARD(x, y, w, h);
  float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
  float s = fmaxf(w / tw, h / th);
  float sw = w / s, sh = h / s;
  float sx = (tw - sw) / 2, sy = (th - sh) * 0.3f; // (faces tend to be near the top)
  vita2d_draw_texture_tint_part_scale(t, x, y, sx, sy, sw, sh, s, s, tint);
}

// draw_cover with rounded (squircle) corners: the picture drawn in strips, each row of the corners cut
// in by the curve, so nothing behind it needs painting over (a picture over a picture)
static void draw_cover_round(vita2d_texture *t, float x, float y, float w, float h, float r, unsigned tint) {
  DRAW_GUARD(x, y, w, h);
  x = floorf(x + 0.5f); y = floorf(y + 0.5f); w = floorf(w + 0.5f); h = floorf(h + 0.5f); // (whole pixels: no seams between the rows)
  float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
  float s = fmaxf(w / tw, h / th);
  float sw = w / s, sh = h / s;
  float sx = (tw - sw) / 2, sy = (th - sh) * 0.3f;
  r *= 1.25f;
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  int rows = (int)ceilf(r);
  for (int i = 0; i < rows; i++) {
    float v = fmaxf(0, 1 - (i + 0.5f) / r); // (how far up the corner's curve this row is)
    float in_ = r - r * powf(1 - powf(v, 3.5f), 1 / 3.5f);
    float cw = w - in_ * 2;
    if (cw <= 0) continue;
    // (the row at the top, then its mirror at the bottom)
    in_ = floorf(in_ + 0.5f); cw = w - in_ * 2;
    vita2d_draw_texture_tint_part_scale(t, x + in_, y + i, sx + in_ / s, sy + i / s, cw / s, 1 / s, s, s, tint);
    vita2d_draw_texture_tint_part_scale(t, x + in_, y + h - i - 1, sx + in_ / s, sy + (h - i - 1) / s, cw / s, 1 / s, s, s, tint);
  }
  if (h - rows * 2 > 0)
    vita2d_draw_texture_tint_part_scale(t, x, y + rows, sx, sy + rows / s, sw, (h - rows * 2) / s, s, s, tint);
}

// ---------- input ----------
typedef struct {
  unsigned now, pressed, held_ms[32];
  float lx, ly;
  float rx; int r_flick; // the right stick across; a flick of it this frame (-1, 1)
  int touching, tx, ty, tcount, t2x, t2y;
  int tap, tap_x, tap_y, dbl;
  float drag_dx, drag_dy; // since the last frame
  int down_x, down_y, moved, flicked;
  float fvx, fvy;
  unsigned down_frame, last_tap_frame;
  int last_tap_x, last_tap_y;
  float bdx, bdy; // the rear touchpad: how far a finger on it moved since the last frame
  int back_on, back_x0;
  int panic; // (three fingers tapped together: the calculator, at once)
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
  {
    // (the right stick: a flick across, once, until it's back near the middle)
    static int rlatched;
    in.rx = (c.rx - 128) / 128.0f;
    in.r_flick = 0;
    if (!rlatched && fabsf(in.rx) > 0.65f) { in.r_flick = in.rx > 0 ? 1 : -1; rlatched = 1; }
    else if (rlatched && fabsf(in.rx) < 0.3f) rlatched = 0;
  }

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
  in.panic = 0;
  in.drag_dx = in.drag_dy = 0;
  in.tcount = t.reportNum;
  // the panic tap: three fingers, landing together (within ~150 ms), apart from each other (not a
  // palm), staying put, never a fourth, lifted within half a second. Anything else isn't it, and a
  // touch of two or more fingers is never a tap either.
  {
    static unsigned g0, tri_at;
    static int maxc, tri_ok, px[3], py[3];
    int c = t.reportNum;
    if (c > 0 && !prev_touch) { g0 = frame_no; maxc = 0; tri_ok = 1; tri_at = 0; }
    if (c > maxc) maxc = c;
    if (c > 3) tri_ok = 0;
    if (c == 3 && !tri_at) {
      if (frame_no - g0 > 9) tri_ok = 0;
      tri_at = frame_no;
      for (int k = 0; k < 3; k++) { px[k] = t.report[k].x / 2; py[k] = t.report[k].y / 2; }
      for (int a = 0; a < 3; a++)
        for (int b = a + 1; b < 3; b++)
          if (abs(px[a] - px[b]) + abs(py[a] - py[b]) < 40) tri_ok = 0;
    } else if (c == 3 && tri_at) {
      // (each finger near one of where they landed: the reports' order can change)
      for (int k = 0; k < 3; k++) {
        int near = 0;
        for (int j = 0; j < 3; j++) near |= abs(t.report[k].x / 2 - px[j]) < 24 && abs(t.report[k].y / 2 - py[j]) < 24;
        if (!near) tri_ok = 0;
      }
    }
    if (c == 0 && prev_touch) {
      if (tri_at && tri_ok && maxc == 3 && frame_no - g0 <= 30) in.panic = 1;
      if (maxc >= 2) in.moved = 1; // (no tap from it)
    }
  }
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
// (a few pixels of slack all round: a finger is wider than a cursor)
#define SLACK 5
static int hit(int x, int y, int w, int h) { return in.tap && in.tap_x >= x - SLACK && in.tap_x < x + w + SLACK && in.tap_y >= y - SLACK && in.tap_y < y + h + SLACK; }

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
enum { S_PICK, S_LOCKED, S_UNLOCKING, S_LIBRARY, S_VIEWER, S_SETTINGS, S_COVER, S_WEB, S_WEBVIEW, S_SHOW, S_DRIVE, S_PIN, S_STATS, S_WIFI, S_CALC, S_CLEAN, S_HEALTH, S_VIDEOS, S_MANGA, S_BOOK, S_READER, S_FAVS, S_TEST, S_DUPES };
static int screen = S_PICK, cover_return = S_LIBRARY;
static int screen_now(void) { return screen; }
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

static unsigned acc_theme; // (the theme's accent: where the picture's colour goes back to)
static void apply_theme_base(void);
static void apply_theme(void) { apply_theme_base(); acc_theme = ACC; }
static void apply_theme_base(void) {
  int b = cfg.bgstyle >= 0 && cfg.bgstyle < 4 ? cfg.bgstyle : 0;
  BG = BGS[b].bg; SURF = BGS[b].surf; SURF2 = BGS[b].surf2; TEXT = BGS[b].text; DIM = BGS[b].dim; FAINT = BGS[b].faint;
  vita2d_set_clear_color(BG);
  // the accent: the one chosen in Settings, else the vault's own theme (as the PC and the phones show it)
  if (cfg.theme == NTHEMES + 1) {
    // the custom colour, and a second one a little round the wheel for the gradients
    ACC = hsv(cfg.acc_h, cfg.acc_s / 100.f, cfg.acc_v / 100.f);
    ACC2 = hsv(cfg.acc_h + 40, cfg.acc_s / 100.f * 0.9f, fminf(1, cfg.acc_v / 100.f + 0.05f));
    return;
  }
  if (cfg.theme > 0 && cfg.theme <= NTHEMES) { ACC = THEMES[cfg.theme - 1].a; ACC2 = THEMES[cfg.theme - 1].b; return; }
  for (size_t k = 0; k < sizeof THEMES / sizeof *THEMES; k++)
    if (!strcmp(THEMES[k].name, vault.theme)) { ACC = THEMES[k].a; ACC2 = THEMES[k].b; return; }
  ACC = THEMES[0].a; ACC2 = THEMES[0].b;
}

static int sel;

// ---------- the writer: the vault's lists written to the card off the drawing thread ----------
// The drawing thread only makes the text (fast, in memory); this thread makes the day's backup and
// writes it (slow on a memory card, and more so encrypted). The same file asked for twice before it's
// written is written once, the newer text. saver_flush() waits for all of it (a vault closing, a sync
// starting, the vault read again).
typedef struct { char root[512], name[32]; uint8_t key[32]; int has_key; char *data; size_t n; } SaveJob;
static SaveJob sjobs[4];
static int nsjobs;
static volatile int saver_busy, saver_failed;
static pthread_mutex_t smx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t scv_done = PTHREAD_COND_INITIALIZER; // (the saver: a job finished, room or all written)
static int saver_started;
static void *saver_main(void *arg) {
  pin_to_core(2); // (off the drawing thread's core)
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&smx);
    while (!nsjobs) pthread_cond_wait(&scv, &smx);
    SaveJob j = sjobs[0];
    memmove(sjobs, sjobs + 1, --nsjobs * sizeof *sjobs);
    saver_busy = 1;
    pthread_mutex_unlock(&smx);
    lv_backup_today_root(j.root);
    char p[600];
    snprintf(p, sizeof p, "%s/%s", j.root, j.name);
    if (lv_write_all(p, j.has_key ? j.key : NULL, j.data, j.n) != 0) saver_failed = 1;
    free(j.data);
    memset(j.key, 0, sizeof j.key);
    pthread_mutex_lock(&smx);
    saver_busy = 0;
    pthread_cond_broadcast(&scv_done); // (whoever waits for room, or for everything to be written)
    pthread_mutex_unlock(&smx);
  }
  return NULL;
}
// data: malloc'd, the writer's from now on
static void saver_start(void) {
  if (saver_started) return;
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 128 * 1024);
  saver_started = pthread_create(&t, &at, saver_main, NULL) == 0;
}
static void save_async(const char *name, char *data) {
  if (!data) return;
  saver_start();
  if (!saver_started) {
    { // (no thread: written here, as before)
      lv_backup_today_root(vault.root);
      char p[600];
      snprintf(p, sizeof p, "%s/%s", vault.root, name);
      if (lv_write_all(p, lv_key(&vault), data, strlen(data)) != 0) saver_failed = 1;
      free(data);
      return;
    }
  }
  pthread_mutex_lock(&smx);
  int k = 0;
  for (; k < nsjobs; k++) if (!strcmp(sjobs[k].name, name) && !strcmp(sjobs[k].root, vault.root)) { free(sjobs[k].data); break; }
  if (k == nsjobs) {
    if (nsjobs == 4) { while (nsjobs == 4) pthread_cond_wait(&scv_done, &smx); k = nsjobs; } // (waits for room, asleep)
    nsjobs++;
  }
  SaveJob *j = &sjobs[k];
  snprintf(j->root, sizeof j->root, "%s", vault.root);
  snprintf(j->name, sizeof j->name, "%s", name);
  j->has_key = vault.has_key;
  if (vault.has_key) memcpy(j->key, vault.key, 32);
  j->data = data;
  j->n = strlen(data);
  pthread_cond_signal(&scv);
  pthread_mutex_unlock(&smx);
}
static void saver_flush(void) {
  // (asleep until everything's written, at most 4 s, rather than looking every 2 ms)
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += 4;
  pthread_mutex_lock(&smx);
  while (nsjobs || saver_busy) if (pthread_cond_timedwait(&scv_done, &smx, &until) != 0) break;
  pthread_mutex_unlock(&smx);
}
// views.csv, when it has changed
static void views_save_async(void) {
  if (!loaded_vault || !vault.views_dirty) return;
  char *t = lv_views_csv(&vault);
  if (!t) return;
  vault.views_dirty = 0;
  save_async("views.csv", t);
}
// index.csv, when it has changed (never without its columns)
static int index_save_async(void) {
  if (!loaded_vault || !vault.index_dirty) return 0;
  if (!vault.nhead) return -1;
  char *t = lv_index_csv(&vault);
  if (!t) return -1;
  vault.index_dirty = 0;
  save_async("index.csv", t);
  return 0;
}

static void close_vault(void) {
  if (!loaded_vault) return;
  pthread_mutex_lock(&mx);
  vgen++; // (the thumbnail thread's work for this vault is dropped from now on)
  nwant = 0;
  big_ask.item = -1;
  pthread_mutex_unlock(&mx);
  saver_flush();
  pthread_mutex_lock(&vmx);
  lv_vault_save_views(&vault);
  drop_textures();
  loaded_vault = 0;
  lv_vault_free(&vault);
  pthread_mutex_unlock(&vmx);
}
static int decoy_session; // (opened with the decoy PIN: nothing about the lock, or which vault, changes)
static int is_drive_vault(const char *root); // (livearea.c)
static void open_start_vault(void);
static void la_update_stats(void);
static void open_vault(const char *path_in) {
  char path[512];
  snprintf(path, sizeof path, "%s", path_in);
  close_vault();
  pthread_mutex_lock(&vmx);
  lv_vault_init(&vault, path);
  pthread_mutex_unlock(&vmx);
  if (!decoy_session) {
    snprintf(cfg.vault, sizeof cfg.vault, "%s", path);
    if (!is_drive_vault(path)) snprintf(cfg.local_vault, sizeof cfg.local_vault, "%s", path);
    cfg_save();
  }
  screen = vault.untraceable ? S_LOCKED : S_UNLOCKING;
}
// Unlocking (two seconds of key making) and reading the vault (a big one takes a while) happen on a
// thread of their own, so the screen keeps moving: the main loop looks at load_state.
static volatile int load_state; // 0 idle, 1 working, 2 done, 3 failed, 4 wrong password, 5 no check file
static const char *load_why;
static char password[128];
static void *load_main(void *arg) {
  pin_to_core(1); // (off the drawing thread's core)
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
static void web_start(void); // (web.c: its threads, started under the loading screen)
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
  free(tsize_w); free(tsize_h);
  tsize_w = calloc(vault.n + 1, sizeof *tsize_w);
  tsize_h = calloc(vault.n + 1, sizeof *tsize_h);
  if (!tstate || !slot_of || !tsize_w || !tsize_h) {
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
  // (the Browser's threads start when it's first opened, not with the vault)
  scroll = scroll_v = 0;
  sel = 0;
  la_update_stats();
  screen = S_LIBRARY;
}

// ----- the picker: which vault, or how to put one on the Vita -----
static void open_drive(int from); // (drive_ui.c)
static void icon(const char *name, float cx, float cy, unsigned col);
static void draw_pick(void) {
  text(40, 70, TEXT, 34, "Lewd Vault");
  text(40, 100, DIM, 18, "for PS Vita");
  if (nfound == 0) {
    // the first start: a new vault here (nothing else needed), or the PC's copied over, or Drive's
    text(40, 160, TEXT, 22, "No vault on this Vita yet");
    text(40, 190, DIM, 16, "A vault is where your saved pictures, videos and comics go.");
    int bx = 40, by = 214, bw = W - 80;
    rrect(bx, by, bw, 72, 20, ACC);
    icon("plus", bx + 34, by + 36, RGBA(20, 12, 16, 255));
    text(bx + 60, by + 32, RGBA(20, 12, 16, 255), 20, "Start a new vault on this Vita");
    text(bx + 60, by + 56, RGBA(20, 12, 16, 200), 14, "Empty, in ux0:data/LewdVault/My Vault  ·  fill it from the Browser and Comics");
    text(bx + bw - 40, by + 44, RGBA(20, 12, 16, 255), 18, "✕");
    int y2 = by + 86;
    rrect(bx, y2, bw, 112, 20, SURF);
    text(bx + 24, y2 + 30, TEXT, 17, "Or use the vault from the PC app (optional)");
    text(bx + 24, y2 + 56, DIM, 14, "Copy its folder (\"Image Vault data\", or data.temp for Untraceable) into  ux0:data/LewdVault/");
    text(bx + 24, y2 + 78, DIM, 14, "with VitaShell (SELECT starts its FTP) or Content Manager, then press □ to look again.");
    text(bx + 24, y2 + 100, FAINT, 13, "An Untraceable vault stays encrypted on the card and opens with its password.");
    rrect(40, H - 62, 300, 44, 22, SURF2);
    text_center(190, H - 34, TEXT, 16, "△  From Google Drive");
    if ((in.pressed & SCE_CTRL_TRIANGLE) || hit(40, H - 62, 300, 44)) { sfx(SFX_OPEN); open_drive(S_PICK); return; }
    if ((in.pressed & SCE_CTRL_SQUARE) || hit(bx, y2, bw, 112)) { sfx(SFX_TICK); find_vaults(); if (!nfound) say("Still no vault in ux0:data/LewdVault/"); return; }
    if ((in.pressed & SCE_CTRL_CROSS) || hit(bx, by, bw, 72)) {
      const char *why = NULL;
      static const char NEWV[] = "ux0:data/LewdVault/My Vault";
      if (lv_vault_create(NEWV, &why) != 0) { sfx(SFX_ERROR); say(why ? why : "It couldn't be made"); find_vaults(); return; }
      sfx(SFX_SELECT);
      say("Your vault is ready: save things into it from the Browser and Comics");
      find_vaults();
      open_vault(NEWV);
      return;
    }
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
// The loading screen: the app's heart as a glass vessel, its liquid (the accent) sloshing, a darker
// wave behind it, bubbles rising through it and drifting up inside, and a line with dots that count.
// The heart's outline is drawn once into a texture from a distance field (thick, round, smooth at any
// edge); the liquid is drawn as one-pixel columns between the wave and the heart's inside, its top
// edge softened, so the whole of it fades out with the screen.
static unsigned mix_col(unsigned a, unsigned b, float t);
static void pill_progress(float x, float y, float w, float h, float f, int a);
#define FL_W 280
#define FL_H 270
#define FL_T 12.0f   // (the outline's thickness)
#define FL_S 200.0f  // (the heart's size: its distance field's unit, in pixels)
#define FL_TIP 246.0f // (the heart's point, in the texture)
#define FL_ROUND 10.0f // (how much rounder than the bare shape: its corners and point softened)
static vita2d_texture *fl_tex;
static short fl_in_top[FL_W], fl_in_bot[FL_W]; // (each column's inside: top and bottom; -1 none)
static float fl_top_y; // (the heart's highest inside point: bubbles fade before it)
// (a heart's distance field, Inigo Quilez's: the point at the origin, the lobes above; < 0 inside)
static float sd_heart(float x, float y) {
  x = fabsf(x);
  if (y + x > 1.0f) { float dx = x - 0.25f, dy = y - 0.75f; return sqrtf(dx * dx + dy * dy) - 0.35355339f; }
  float d1 = x * x + (y - 1.0f) * (y - 1.0f);
  float m = 0.5f * fmaxf(x + y, 0.0f), d2 = (x - m) * (x - m) + (y - m) * (y - m);
  return sqrtf(fminf(d1, d2)) * (x - y > 0 ? 1.0f : -1.0f);
}
// (in the texture's pixels, the heart rounded a little: < 0 inside)
static float fl_shape(float px, float py) {
  float u = (px - FL_W / 2.0f) / FL_S, v = (FL_TIP - py) / FL_S;
  return sd_heart(u, v + FL_ROUND / FL_S * 0.6f) * FL_S - FL_ROUND;
}
static void make_flask(void) {
  if (fl_tex) return;
  fl_tex = tex_new(FL_W, FL_H);
  if (!fl_tex) return;
  uint8_t *d = vita2d_texture_get_datap(fl_tex);
  unsigned stride = vita2d_texture_get_stride(fl_tex);
  for (int y = 0; y < FL_H; y++)
    for (int x = 0; x < FL_W; x++) {
      float o = fabsf(fl_shape(x + 0.5f, y + 0.5f)) - FL_T / 2; // (the outline: a band along the edge)
      float al = fminf(fmaxf(0.5f - o, 0), 1);
      uint8_t *q = d + y * stride + x * 4;
      q[0] = q[1] = q[2] = 255;
      q[3] = (uint8_t)(al * 255);
    }
  vita2d_texture_set_filters(fl_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
  // each column's inside: where the liquid can be (just under the outline's inner edge)
  fl_top_y = FL_H;
  for (int x = 0; x < FL_W; x++) {
    int t = -1, bo = -1;
    for (int y = 0; y < FL_H; y++) {
      if (fl_shape(x + 0.5f, y + 0.5f) < -FL_T / 2 + 1.0f) { if (t < 0) t = y; bo = y + 1; }
    }
    fl_in_top[x] = (short)t; fl_in_bot[x] = (short)bo;
    if (t >= 0 && t < fl_top_y) fl_top_y = t;
  }
}
// (a band of liquid: from the wave down to the heart's inside, column by column; the wave's edge
//  softened with a part-covered pixel above it)
static void fl_liquid(float ox, float oy, float level, float amp, float k, float ph, float tilt, unsigned col) {
  float cx = FL_W / 2.0f;
  unsigned a = (col >> 24) & 255;
  for (int x = 0; x < FL_W; x++) {
    if (fl_in_top[x] < 0) continue;
    float wy = level + amp * sinf(k * x + ph) + tilt * (x - cx);
    float top = fmaxf(wy, fl_in_top[x]), bot = fl_in_bot[x];
    if (top >= bot) continue;
    float ft = floorf(top), cover = 1 - (top - ft);
    if (ft + 1 < bot) vita2d_draw_rectangle(ox + x, oy + ft + 1, 1, bot - ft - 1, col);
    if (top > fl_in_top[x]) vita2d_draw_rectangle(ox + x, oy + ft, 1, 1, with_alpha(col, (int)(a * cover)));
  }
}
static void draw_splash(const char *line, int alpha) {
  make_flask();
  float t = frame_no / 60.0f;
  vita2d_draw_rectangle(0, 0, W, H, with_alpha(BG, alpha));
  // the heart comes in (rising, a little spring), then beats now and then, softly
  float in_t = fminf(1.0f, frame_no / 30.0f);
  float spring = 1.0f - expf(-5.0f * in_t) * cosf(7.0f * in_t);
  // (the heart, the pill and the line together, centred down the screen)
  float ox = W / 2.0f - FL_W / 2.0f, oy = 74 + (1 - spring) * 30;
  int fa = (int)(alpha * fminf(1, in_t * 1.6f));
  if (fl_tex) {
    // the liquid: a darker wave behind, the accent in front; both slosh, tilting against each other
    float tilt = 0.09f * sinf(t * 1.7f), level = FL_TIP - 112 + 4 * sinf(t * 1.1f);
    fl_liquid(ox, oy, level - 7, 6, 0.040f, -t * 2.6f + 1.3f, tilt * 0.7f, with_alpha(mix_col(ACC, BG, 0.45f), fa));
    fl_liquid(ox, oy, level, 7, 0.034f, t * 3.1f, -tilt, with_alpha(ACC, fa));
    float cx = ox + FL_W / 2.0f;
    // bubbles: rising through the liquid (light) ...
    for (int k = 0; k < 7; k++) {
      float sp = 0.32f + 0.08f * k, ph = fmodf(t * sp + k * 0.37f, 1.0f);
      float bx = cx + sinf(k * 2.1f) * 40 + sinf(t * 2 + k) * 4, by = oy + FL_TIP - 40 - ph * 70;
      circle(bx, by, 3.5f + (k % 3), with_alpha(mix_col(ACC, TEXT, 0.45f), (int)(fa * 0.55f * sinf(ph * 3.14159f))));
    }
    // ... and from its top, drifting up inside the heart, fading before its top
    float rise = level - fl_top_y - 26;
    for (int k = 0; k < 5; k++) {
      float sp = 0.38f + 0.07f * k, ph = fmodf(t * sp + k * 0.23f, 1.0f);
      float bx = cx + (k - 2) * 26 + sinf(k * 1.7f + ph * 5) * 7, by = oy + level - 10 - ph * rise;
      circle(bx, by, 5.0f - (k % 2) * 1.5f - ph * 2, with_alpha(ACC, (int)(fa * (1 - ph) * (1 - ph))));
    }
    // the heart, beating gently: its outline drawn a touch bigger on each beat
    float beat = fmaxf(0, sinf(t * 2.4f)), bs = 1 + 0.015f * beat * beat * beat;
    vita2d_draw_texture_tint_scale(fl_tex, cx - FL_W * bs / 2, oy + FL_H / 2.0f - FL_H * bs / 2, bs, bs, with_alpha(TEXT, fa));
  } else heart(W / 2.0f, 200, 60, with_alpha(ACC, alpha));
  // a big striped pill under the heart, filling as the vault opens (it eases towards full while it
  //  works, and goes all the way once it's open), and the line under it, larger
  int ta = (int)(alpha * fminf(1.0f, fmaxf(0.0f, (frame_no - 12) / 20.0f)));
  {
    static float shown_f;
    float target = alpha < 255 ? 1.0f : 0.92f * (1 - expf(-t / 1.4f));
    shown_f += (target - shown_f) * (alpha < 255 ? 0.25f : 0.08f);
    float pw = 380, ph = 26, px = W / 2.0f - pw / 2, py = oy + FL_H + 18;
    pill_progress(px, py, pw, ph, shown_f, ta);
  }
  char msg[96];
  snprintf(msg, sizeof msg, "%s", line);
  size_t n = strlen(msg);
  if (n >= 3 && !strcmp(msg + n - 3, "\u2026")) msg[n - 3] = 0;
  int w = text_w(24, msg), dots = (int)(t * 2.5f) % 4;
  float lx = W / 2.0f - (w + 40) / 2.0f, ly = oy + FL_H + 88;
  text((int)lx, (int)ly, with_alpha(TEXT, ta), 24, msg);
  for (int k = 0; k < 3; k++) {
    float on = k < dots ? 1 : 0.25f;
    circle(lx + w + 12 + k * 13, ly - 7, 4.0f, with_alpha(TEXT, (int)(ta * on)));
  }
}
static void draw_unlocking(int stage) {
  draw_splash(stage ? "Opening the vault\u2026" : "Checking the password\u2026", 255);
}

static int set_sel, plan_sel;
static int wf_return;      // (wifi.c: the screen to go back to,
static char wf_last[160];  //  and the last file, or why it stopped)
// ----- the library -----
static void open_viewer(int k, int slideshow);
static int show_pool; // (the slideshow's own list is the one shown: lib.c, mode_pass)
// The slideshow's list made the one shown; returns where item (an item number) is in it, else 0
static int show_pool_on(int item) {
  show_pool = 1;
  refilter();
  for (int k = 0; k < nshown; k++) if (shown[k] == item) return k;
  return 0;
}
// ---------- the app's icons (vita/ui/*.png, tools/make-ui-icons.mjs): white glyphs, tinted when drawn ----------
// (found by a hash of the name: one or two compares, not a walk past every icon before it; room for
//  128, there are fifty-odd)
#define UI_ICONS 128
static struct { const char *name; vita2d_texture *t; int tried; } ui_icons[UI_ICONS];
static vita2d_texture *ui_icon(const char *name) {
  unsigned h = 2166136261u;
  for (const char *c = name; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
  int k = h & (UI_ICONS - 1), probes = 0;
  while (ui_icons[k].name && ui_icons[k].name != name && strcmp(ui_icons[k].name, name)) { k = (k + 1) & (UI_ICONS - 1); if (++probes == UI_ICONS) return NULL; }
  if (!ui_icons[k].name) ui_icons[k].name = name;
  if (!ui_icons[k].tried) {
    ui_icons[k].tried = 1;
    char p[96];
    snprintf(p, sizeof p, "app0:ui/%s.png", name);
    ui_icons[k].t = load_png_small(p);
  }
  return ui_icons[k].t;
}
// an icon centred on (cx, cy), at its own size (they're made at the size they're drawn)
static void icon(const char *name, float cx, float cy, unsigned col) {
  DRAW_GUARD(cx, cy);
  vita2d_texture *t = ui_icon(name);
  if (!t) return;
  float w = vita2d_texture_get_width(t), h = vita2d_texture_get_height(t);
  vita2d_draw_texture_tint(t, (int)(cx - w / 2), (int)(cy - h / 2), col);
}

// ---------- the tab bar: a pill floating over the bottom, as on iOS ----------
// The chosen tab's bubble: glass with the accent's colour in it. A soft glow round it, a light rim,
// its body, a sheen across its top half and a bright glint near the top: light on a curved surface.
static unsigned mix_col(unsigned a, unsigned b, float t) { // (a to b; alpha from a)
  int r = (int)((a & 255) * (1 - t) + (b & 255) * t), g = (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t);
  int bl = (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t);
  return RGBA(r, g, bl, (a >> 24) & 255);
}
// Liquid glass, as iOS has it: the chosen thing in a lens of glass, a breath of the accent in it, a
// faint even rim, light from above as a soft gradient, a faint shadow under it. round: a pill (the tab bar); else squircle corners (segments).
// A progress pill: an indented track (shadow inside its top, a lit lip at its foot), the done part
// the accent with diagonal stripes running along it and a glass sheen on top; f < 0: not known how
// far, a striped part sliding to and fro. The stripes are a small repeating texture.
static vita2d_texture *stripe_tex;
static void make_stripes(void) {
  if (stripe_tex) return;
  stripe_tex = tex_new(64, 64);
  if (!stripe_tex) return;
  uint8_t *d = vita2d_texture_get_datap(stripe_tex);
  unsigned stride = vita2d_texture_get_stride(stripe_tex);
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      float u = fmodf(x + y + 0.5f, 32.0f), edge = fminf(fabsf(u - 8), fabsf(u - 24)); // (a band 16 wide every 32)
      float a = u > 8 && u < 24 ? 1.0f : 0.0f;
      a = fminf(1, fmaxf(0, edge < 0.7f ? 0.5f + (a > 0 ? edge : -edge) / 1.4f : a));
      uint8_t *p = d + y * stride + x * 4;
      p[0] = p[1] = p[2] = 255; p[3] = (uint8_t)(a * 255);
    }
  vita2d_texture_set_filters(stripe_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
  sceGxmTextureSetUAddrMode(&stripe_tex->gxm_tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
  sceGxmTextureSetVAddrMode(&stripe_tex->gxm_tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
}
static void rrect_round(float x, float y, float w, float h, float r, unsigned c);
static void pill_progress(float x, float y, float w, float h, float f, int a) {
  make_stripes();
  float r = h / 2;
  // the track, pressed in
  rrect_round(x - 1, y - 1, w + 2, h + 2, r + 1, with_alpha(RGBA(0, 0, 0, 255), a * 70 / 255));
  rrect_round(x, y, w, h, r, with_alpha(mix_col(BG, RGBA(0, 0, 0, 255), 0.35f), a));
  rrect_round(x + 1, y + 1, w - 2, h * 0.45f, fminf(r - 1, h * 0.22f), with_alpha(RGBA(0, 0, 0, 255), a * 60 / 255)); // (its shadow inside)
  rrect_round(x + r * 0.6f, y + h - 2.5f, w - r * 1.2f, 1.5f, 0.75f, with_alpha(TEXT, a * 22 / 255));                  // (the lip lit below)
  float fx = x, fw;
  if (f < 0) { float run = 0.5f + 0.5f * sinf(frame_no / 60.0f * 2.4f); fw = fmaxf(h * 2, w * 0.3f); fx = x + (w - fw) * run; }
  else fw = fmaxf(f > 0 ? h : 0, w * fminf(1, f));
  if (fw <= 0) return;
  rrect_round(fx, y, fw, h, r, with_alpha(ACC, a));
  // the stripes, along the straight part (the ends stay plain), running
  if (stripe_tex && fw > h) {
    float sc = h / 64.0f, off = fmodf(frame_no * 0.6f, 32.0f);
    float sx = fx + r * 0.7f, sw = fw - r * 1.4f;
    vita2d_draw_texture_tint_part_scale(stripe_tex, sx, y, -off, 0, sw / sc, 64, sc, sc, with_alpha(mix_col(ACC, TEXT, 0.35f), a * 90 / 255));
  }
  rrect_round(fx + 2, y + 1.5f, fw - 4, h * 0.42f, fminf(r - 1.5f, h * 0.21f), with_alpha(TEXT, a * 55 / 255)); // (glass on top)
}
static void glass(float x, float y, float w, float h, int round) {
  void (*rr)(float, float, float, float, float, unsigned) = round ? rrect_round : rrect;
  float r = round ? h / 2 : h * 0.42f;
  rr(x, y + 1.5f, w, h, r, RGBA(0, 0, 0, 36));                                     // (its shadow)
  rr(x, y, w, h, r, with_alpha(TEXT, 30));                                         // (its rim: faint, even, all round)
  rr(x + 1, y + 1, w - 2, h - 2, r - 1, with_alpha(mix_col(SURF2, ACC, 0.32f), 235)); // (the glass, a little of the accent in it)
  // (light from above: a soft gradient, a few faint layers, no edge of its own)
  // (each layer inset from the ends by as much as its corners are tighter than the bubble's, so its
  //  top edge stays inside the bubble's curve: nothing sticks out at the corners)
  for (int k = 0; k < 4; k++) {
    float hh = (h - 2) * (0.62f - k * 0.13f), lr = fminf(r - 1, hh / 2), in = fmaxf(0, r - 1 - lr);
    if (w - 2 - 2 * in < 2 * lr) continue;
    rr(x + 1 + in, y + 1, w - 2 - 2 * in, hh, lr, with_alpha(TEXT, 7));
  }
}
static void tab_bubble(float x, float y, float w, float h) { glass(x, y, w, h, 1); }
// (the screens leave BAR at the bottom free when scrolled to the end; meanwhile they show through
// around the pill)
#define PILL_W 600
#define PILL_H 62
static float pill_sel_x = -1, pill_sel_r = -1; // (the capsule's two edges: the leading one moves faster)
static int lib_new;          // saved from the Browser since the Library was last looked at: its badge
static float bar_busy(void); // (favs.c) a save or conversion going: 0-1, 2 not known how far; -1 none
static void set_mode(int m); // (lib.c)
static void favs_open(int tab); // (favs.c)
static void ac_build(const char *q); // (web.c)
static void draw_onboarding(void);
static int tab_prev = -1; static unsigned tab_at; // (the tab switched from, and when: the slide)
static int stats_back = S_SETTINGS; // (where ○ goes from Your stats: the Library's corner, or Settings)
static int browser_screen = S_WEB; // (the Browser tab: back to whichever of its screens was last open)
static int tab_of_screen(void) {
  switch (screen) {
    case S_LIBRARY: case S_FAVS: return 0;
    case S_WEB: case S_VIDEOS: case S_MANGA: return 1;
    case S_SHOW: return 2;
    case S_STATS: return stats_back == S_LIBRARY ? 0 : 3;
    case S_SETTINGS: case S_DRIVE: case S_WIFI: case S_HEALTH: case S_DUPES: return 3;
  }
  return -1;
}
static void draw_bar(void) {
  // a tab changed (a tap, ○, a shortcut): the new one slides in from its side, a curtain of the
  // background drawing back over the first few frames (nothing of the old screen is kept)
  {
    static int last_tab = -1;
    int now_tab = tab_of_screen();
    if (now_tab >= 0 && last_tab >= 0 && now_tab != last_tab) { tab_prev = last_tab; tab_at = frame_no; }
    last_tab = now_tab;
    unsigned age = frame_no - tab_at;
    if (tab_prev >= 0 && now_tab >= 0 && age < 14) {
      float p = age / 14.0f, e = 1 - (1 - p) * (1 - p) * (1 - p);
      float cover = W * (1 - e);
      int a = (int)(255 * (1 - e * 0.6f));
      if (now_tab > tab_prev) { // (from the right: the left part covered, drawing back to the left)
        vita2d_draw_rectangle(0, 0, cover, H, with_alpha(BG, a));
        grad(cover, 0, 80, H, BG, a, 0, 1);
      } else {
        vita2d_draw_rectangle(W - cover, 0, cover, H, with_alpha(BG, a));
        grad(W - cover - 80, 0, 80, H, BG, 0, a, 1);
      }
    } else tab_prev = -1;
  }
  // (tucked: a smaller pill, icons only)
  float t = chrome_hide;
  int pw = (int)(PILL_W - 300 * t), ph = (int)(PILL_H - 20 * t);
  int px = (W - pw) / 2, py = H - ph - (int)(10 - 4 * t), tw = pw / 4;
#undef PILL_W
#undef PILL_H
#define PILL_W pw
#define PILL_H ph
  // a soft shadow, the glassy body, a light rim along its top
  rrect_round(px - 4, py + 2, PILL_W + 8, PILL_H + 8, (PILL_H + 8) / 2, RGBA(0, 0, 0, 50));
  rrect_round(px - 1, py + 3, PILL_W + 2, PILL_H + 2, (PILL_H + 2) / 2, RGBA(0, 0, 0, 70));
  rrect_round(px, py, PILL_W, PILL_H, PILL_H / 2, with_alpha(SURF2, 238));
  rrect_round(px + 2, py + 1, PILL_W - 4, PILL_H / 2, PILL_H / 2 - 2, with_alpha(TEXT, 12));
  static const char *labels[] = {"Library", "Browser", "Slideshow", "Settings"};
  static const char *icons[][2] = {{"library", "library_fill"}, {"compass", "compass_fill"}, {"play", "play_fill"}, {"gear", "gear_fill"}};
  int sel_tab = tab_of_screen();
  // the selected tab's capsule morphs to it: its leading edge goes first, the other follows, so it
  // stretches on the way and settles to its size there
  if (sel_tab >= 0) {
    float tl = px + sel_tab * tw + 6, tr = px + (sel_tab + 1) * tw - 6;
    if (pill_sel_x < 0) { pill_sel_x = tl; pill_sel_r = tr; }
    int right = tl > pill_sel_x;
    pill_sel_x += (tl - pill_sel_x) * (right ? 0.18f : 0.38f);
    pill_sel_r += (tr - pill_sel_r) * (right ? 0.38f : 0.18f);
    if (fabsf(tl - pill_sel_x) < 0.3f) pill_sel_x = tl;
    if (fabsf(tr - pill_sel_r) < 0.3f) pill_sel_r = tr;
    tab_bubble(pill_sel_x, py + 5, pill_sel_r - pill_sel_x, PILL_H - 10);
  }
  if (sel_tab == 0 && screen == S_LIBRARY) lib_new = 0;
  // (held on a tab: its shortcut, Library: the Comics shelf, Browser: Favourites)
  static int long_fired;
  if (!in.touching) long_fired = 0;
  if (in.touching && !in.moved && !long_fired && frame_no - in.down_frame > 32 && in.down_y >= py - 10 && in.down_x >= px && in.down_x < px + 2 * tw) {
    long_fired = 1;
    sfx(SFX_OPEN);
    if (in.down_x < px + tw) { screen = S_LIBRARY; set_mode(3); say("Comics"); }
    else { favs_open(0); }
  }
  if (long_fired) in.tap = 0;
  // the right stick, flicked across: the next tab or the one before
  if (in.r_flick && sel_tab >= 0 && screen != S_DRIVE && screen != S_WIFI && screen != S_HEALTH && screen != S_DUPES) {
    int k = (sel_tab + in.r_flick + 4) % 4;
    sfx(SFX_TAB);
    if (k == 0) screen = S_LIBRARY;
    if (k == 1) screen = browser_screen;
    if (k == 2) { screen = S_SHOW; plan_sel = 0; }
    if (k == 3) screen = S_SETTINGS;
    in.r_flick = 0;
  }
  for (int k = 0; k < 4; k++) {
    int cx = px + k * tw + tw / 2;
    int on = k == sel_tab;
    unsigned c = on ? TEXT : with_alpha(TEXT, 200); // (the chosen one white on its bubble)
    icon(icons[k][on], cx, t > 0.5f ? py + PILL_H / 2 : py + 23, c);
    if (t < 0.5f) text_center(cx, py + PILL_H - 9, with_alpha(c, (int)(255 * (1 - 2 * t))), 12, labels[k]);
    int iy = t > 0.5f ? py + PILL_H / 2 : py + 23;
    // (the Library: how many came in from the Browser since it was looked at)
    if (k == 0 && lib_new > 0 && !on) {
      // (a bigger badge, ringed, with a soft beat: it's seen)
      char b[8];
      snprintf(b, sizeof b, "%d", lib_new > 99 ? 99 : lib_new);
      int bw = text_w(13, b) + 12, bh = 22;
      if (bw < bh) bw = bh;
      float pulse = 0.5f + 0.5f * sinf(frame_no * 0.09f);
      float bx = cx + 6, by = iy - 24;
      rrect_round(bx - 4 - pulse * 3, by - 4 - pulse * 3, bw + 8 + pulse * 6, bh + 8 + pulse * 6, (bh + 8 + pulse * 6) / 2, with_alpha(ACC2, (int)(70 * (1 - pulse) + 30)));
      rrect_round(bx - 2, by - 2, bw + 4, bh + 4, (bh + 4) / 2.0f, SURF2);
      rrect_round(bx, by, bw, bh, bh / 2.0f, ACC2);
      text_center(bx + bw / 2, by + 16, RGBA(255, 255, 255, 255), 13, b);
    }
    // (the Browser: a ring round it while something's being saved or converted)
    if (k == 1) {
      float f = bar_busy();
      if (f >= 0) {
        int dots = 24;
        for (int d = 0; d < dots; d++) {
          float a = -1.5708f + d * 6.2832f / dots;
          int lit = f > 1.5f ? ((d - (int)(frame_no / 3)) % dots + dots) % dots < 7 : d < (int)(f * dots + 0.5f);
          circle(cx + cosf(a) * 17, iy + sinf(a) * 17, 1.6f, lit ? ACC : with_alpha(TEXT, 40));
        }
      }
    }
    // (the button that leads here: ○ goes round Library, Browser, Slideshow, Settings)
    int next = sel_tab >= 0 && k == (sel_tab + 1) % 4 && screen != S_DRIVE && screen != S_WIFI && screen != S_STATS && screen != S_HEALTH;
    (void)next; // (no ring by it: the Library's badge is the only mark on the bar)
    // (the whole quarter of the pill, and a little round it, is the tab's)
    if (in.tap && in.tap_x >= px + k * tw - (k == 0 ? 20 : 0) && in.tap_x < px + (k + 1) * tw + (k == 3 ? 20 : 0) && in.tap_y >= py - 10) {
      sfx(SFX_TAB);
      if (k == 0) { screen = S_LIBRARY; if (query[0]) { query[0] = 0; refilter(); } }
      if (k == 1) screen = browser_screen;
      if (k == 2) { screen = S_SHOW; plan_sel = 0; }
      if (k == 3) screen = S_SETTINGS;
      in.tap = 0;
    }
  }
}
#undef PILL_W
#undef PILL_H
#define PILL_W 600
#define PILL_H 62

// ---------- gradients: one stretched, tinted ramp each (not a rectangle per line) ----------
// ramps[0] alpha 0->255 down, [1] 255->0 down, [2] 0->255 across, [3] 255->0 across (white)
static vita2d_texture *ramps[4];
static void ramps_init(void) {
  for (int k = 0; k < 4; k++) {
    int across = k >= 2;
    ramps[k] = tex_new(across ? 256 : 1, across ? 1 : 256);
    if (!ramps[k]) continue;
    uint32_t *d = vita2d_texture_get_datap(ramps[k]);
    unsigned stride = vita2d_texture_get_stride(ramps[k]) / 4;
    for (int i = 0; i < 256; i++) {
      unsigned a = (k & 1) ? 255 - i : i;
      d[across ? i : i * stride] = RGBA(255, 255, 255, a);
    }
    vita2d_texture_set_filters(ramps[k], SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
  }
}
// (x, y, w, h) in col's colour, its alpha going from a0 to a1 (down, or across)
static void grad(float x, float y, float w, float h, unsigned col, int a0, int a1, int across) {
  DRAW_GUARD(x, y, w, h);
  if (!ramps[0]) ramps_init();
  if (a0 < 0) a0 = 0;
  if (a0 > 255) a0 = 255;
  if (a1 < 0) a1 = 0;
  if (a1 > 255) a1 = 255;
  if (w <= 0 || h <= 0 || (a0 == 0 && a1 == 0)) return;
  int up = a0 <= a1;
  vita2d_texture *t = ramps[(across ? 2 : 0) + (up ? 0 : 1)];
  if (!t) return;
  // the part of the ramp from a0 to a1 (on the falling one, 255 - a)
  int p0 = up ? a0 : 255 - a0, p1 = up ? a1 : 255 - a1;
  float span = p1 - p0 < 1 ? 1 : p1 - p0;
  unsigned c = (col & 0x00FFFFFF) | 0xFF000000u;
  if (across) vita2d_draw_texture_tint_part_scale(t, x, y, p0, 0, span, 1, w / span, h, c);
  else vita2d_draw_texture_tint_part_scale(t, x, y, 0, p0, 1, span, w, h / span, c);
}
// A loading circle: eight dots going round (anything that takes a while shows one)
static void spinner(float cx, float cy, float r) {
  DRAW_GUARD(cx, cy, r);
  for (int k = 0; k < 8; k++) {
    float a = frame_no * 0.12f + k * 0.785f;
    circle(cx + cosf(a) * r, cy + sinf(a) * r, r * 0.13f + k * r * 0.022f, with_alpha(ACC, 70 + k * 22));
  }
}
static int chip(int x, int y, const char *label, int on) {
  int w = text_w(17, label) + 36;
  rrect(x, y, w, 38, 19, on ? ACC : SURF2);
  text(x + 18, y + 26, on ? RGBA(20, 12, 16, 255) : TEXT, 17, label);
  return w;
}
// A header button: a capsule 36 high with an icon (or none) and a label; its width. x is its right edge.
static int head_button(int right, const char *ic, const char *label, unsigned bg, int *hit_out) {
  int w = text_w(16, label) + (ic ? 50 : 32);
  int x = right - w, y = HDR_Y;
  int pressed = in.touching && !in.moved && in.down_x >= x && in.down_x < x + w && in.down_y >= y + 4 && in.down_y < y + 52;
  if (bg == SURF2) {
    // (a plain one: glass; pressed: sunk a little, darker)
    if (pressed) rrect_round(x + 1, y + 9, w - 2, 38, 19, mix_col(SURF2, RGBA(0, 0, 0, 255), 0.25f));
    else glass(x, y + 8, w, 40, 1);
  } else {
    rrect(x, y + 8, w, 40, 20, pressed ? mix_col(bg, RGBA(0, 0, 0, 255), 0.25f) : bg);
    rrect_round(x + 3, y + 9, w - 6, 16, 8, with_alpha(TEXT, 14)); // (a sheen on a tinted one)
  }
  if (ic) icon(ic, x + 22, y + 28, TEXT);
  text(x + (ic ? 38 : 16), y + 34, TEXT, 16, label);
  *hit_out = hit(x, y + 4, w, 48);
  return w;
}
// (following the scroll: down a little way tucks the header and the bar away, up brings them back)
static void chrome_follow(float dscroll, float pos) {
  // (a new target is kept a moment: the page moving as the bar tucks doesn't flip it straight back)
  static unsigned set_at;
  float was = chrome_target;
  if (pos < 40) chrome_target = 0;
  else if (frame_no - set_at < 24) return;
  else if (dscroll > 2.5f) chrome_target = 1;
  else if (dscroll < -2.5f) chrome_target = 0;
  if (chrome_target != was) set_at = frame_no;
}
// The connection's light: green online, red not; a tap says which (and what to do)
static int net_state_cached = -1;
static unsigned net_state_at;
static int online_now(void) {
  if (net_state_cached < 0 || frame_no - net_state_at > 90) { net_state_cached = lv_net_online(); net_state_at = frame_no; }
  return net_state_cached;
}
// The connection's speed, measured when a Wi-Fi button is tapped: about 2 MB from Cloudflare's speed
// test (a thread), timed; the answer as a notice. Only while online, one at a time.
static void ev_notice(const char *ic, unsigned col, const char *fmt, ...);
static void otag_begin(const char *uid);
static volatile int speed_state; // 0 idle, 1 measuring, 2 done, 3 failed
static volatile float speed_mbit;
static void *speed_main(void *arg) {
  (void)arg;
  LvHttp *h = lv_http_new();
  uint8_t *b = NULL; size_t n = 0; const char *err = NULL;
  uint64_t t0 = sceKernelGetProcessTimeWide();
  int st = h ? lv_http_get(h, "https://speed.cloudflare.com/__down?bytes=2000000", &b, &n, 3u << 20, NULL, NULL, &err) : -1;
  uint64_t dt = sceKernelGetProcessTimeWide() - t0;
  if (h) lv_http_free(h);
  free(b);
  if (st == 200 && n > 100000 && dt > 0) { speed_mbit = n * 8.0f / (dt / 1e6f) / 1e6f; speed_state = 2; } else speed_state = 3;
  return NULL;
}
static void speed_tap(int on) {
  sfx(SFX_TICK); net_state_cached = -1;
  if (!on) { say("Offline: turn on Wi-Fi in the Vita's settings (the Library still works)"); return; }
  if (speed_state == 1) { say("Still measuring\u2026"); return; }
  pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 128 * 1024);
  speed_state = 1;
  if (pthread_create(&t, &at, speed_main, NULL) == 0) { pthread_detach(t); say("Online \u00b7 measuring the speed\u2026"); } else { speed_state = 0; say("Online"); }
}
static void speed_poll(void) { // (each frame: the answer, once)
  if (speed_state == 2) { speed_state = 0; ev_notice("wifi", RGBA(90, 214, 120, 255), "Connection: %.1f Mbit/s (%.1f MB/s)", speed_mbit, speed_mbit / 8); }
  else if (speed_state == 3) { speed_state = 0; ev_notice("wifi", ACC2, "The speed couldn't be measured"); }
}
static int online_button(int right) {
  int on = online_now(), x = right - 44, y = HDR_Y;
  rrect(x, y + 8, 44, 40, 20, SURF2);
  icon("wifi", x + 22, y + 26, on ? RGBA(90, 214, 120, 255) : RGBA(240, 84, 84, 255));
  circle(x + 22, y + 39, 3, on ? RGBA(90, 214, 120, 255) : RGBA(240, 84, 84, 255));
  if (hit(x, y + 4, 44, 48)) speed_tap(on);
  return 44;
}

// The left stick as the D-pad, for the grids: a push is a press, held it repeats (sooner the further
// it's pushed), so it moves the selection as the D-pad does
static unsigned stick_dirs(void) {
  static unsigned held, next_at;
  unsigned d = 0;
  if (in.ly < -0.5f) d = SCE_CTRL_UP; else if (in.ly > 0.5f) d = SCE_CTRL_DOWN;
  else if (in.lx < -0.5f) d = SCE_CTRL_LEFT; else if (in.lx > 0.5f) d = SCE_CTRL_RIGHT;
  if (!d) { held = 0; return 0; }
  float m = fmaxf(fabsf(in.lx), fabsf(in.ly));
  if (d != held) { held = d; next_at = frame_no + 16; return d; }
  if (frame_no >= next_at) { next_at = frame_no + (unsigned)(12 - 8 * (m - 0.5f) * 2); return d; }
  return 0;
}
// (the selection scrolled out of sight by a touch or a flick: a D-pad press starts from the first
//  card in view instead of jumping back to it)
static int sel_in_view(const void *grid, int sel, float scroll, float view_h) {
  const Grid *g = grid;
  if (sel < 0 || sel >= g->n) return -1;
  Rect r = g->r[sel];
  if (r.y + r.h > scroll + 4 && r.y < scroll + view_h - 4) return sel;
  // (the first card whose top is in sight)
  for (int k = grid_first(g, scroll); k < g->n; k++) if (g->r[k].y >= scroll - 2) return k;
  return grid_first(g, scroll);
}
// (the Wi-Fi mark, drawn: three arcs and a dot, centred on (cx, cy) as a whole)
static void wifi_mark(float cx, float cy, unsigned c) {
  float by = cy + 6.5f; // (the dot: the arcs' centre)
  circle(cx, by, 2.2f, c);
  for (int k = 1; k <= 3; k++) {
    float r = 4.6f * k, thick = 1.25f;
    int n = 6 + k * 5;
    for (int j = 0; j <= n; j++) { float an = -2.36f + 1.57f * j / n; circle(cx + cosf(an) * r, by + sinf(an) * r, thick, c); }
  }
}
// (the connection as a small pressed-in button: dark inside with its shadow at the top and a lit lip
//  below, the wifi mark green online and red not; a tap says which)
static int online_inset(int right) {
  int on = online_now(), d = 36, x = right - d, y = HDR_Y + 10;
  unsigned c = on ? RGBA(90, 214, 120, 255) : RGBA(240, 84, 84, 255);
  rrect_round(x - 1, y - 1, d + 2, d + 2, d / 2 + 1, with_alpha(RGBA(0, 0, 0, 255), 80));
  rrect_round(x, y, d, d, d / 2, mix_col(SURF2, RGBA(0, 0, 0, 255), 0.45f));
  rrect_round(x + 1, y + 1, d - 2, d * 0.45f, d * 0.22f, RGBA(0, 0, 0, 70));       // (shadow inside its top)
  rrect_round(x + 7, y + d - 3, d - 14, 1.5f, 0.75f, with_alpha(TEXT, 30));          // (the lip lit below)
  circle(x + d / 2, y + d / 2, 13, with_alpha(c, 34));                                 // (its colour glowing in it)
  wifi_mark(x + d / 2.0f, y + d / 2.0f, c);
  if (hit(x - 4, y - 6, d + 8, d + 12)) speed_tap(on);
  if (speed_state == 1) { float an = frame_no * 0.15f; circle(x + d / 2 + cosf(an) * 15, y + d / 2 + sinf(an) * 15, 2, c); } // (measuring: a dot going round)
  return d;
}
// The Library's corner: a quarter circle in the screen's top right, Your stats behind it (its bars),
// and the connection's light on it, green or red, so the corner does both in the room of one button
static int stats_corner(void) {
  const float R = 58;
  int y0 = HDR_Y - 2; // (the header's top: tucked away with it)
  int on = online_now();
  circle(W, y0, R + 2, with_alpha(BG, 200));
  circle(W, y0, R, SURF2);
  circle(W, y0, R - 1, mix_col(with_alpha(SURF2, 255), ACC, 0.18f));
  // (a chart: four bars and a line of dots across them, each dot set off from its bar by a gap)
  (void)on;
  {
    unsigned bgc = mix_col(with_alpha(SURF2, 255), ACC, 0.18f), ink = with_alpha(mix_col(TEXT, ACC, 0.55f), 205); // (quiet: in the accent)
    float bx0 = W - 34.4f, base = y0 + 37.3f, sp = 6.5f; // (centred on the quarter circle's middle, 4R/3π from each edge)
    static const float bh[4] = {8.5f, 16.5f, 11.5f, 22};
    for (int k = 0; k < 4; k++) rrect_round(bx0 + k * sp - 2.1f, base - bh[k], 4.2f, bh[k], 1.3f, ink);
    for (int k = 0; k < 3; k++) { // (the line: a row of little discs, thick and smooth)
      float x1 = bx0 + k * sp, yA = base - bh[k], x2 = bx0 + (k + 1) * sp, yB = base - bh[k + 1];
      for (int j = 0; j <= 12; j++) { float t = j / 12.0f; circle(x1 + (x2 - x1) * t, yA + (yB - yA) * t, 2.0f, bgc); }
      for (int j = 0; j <= 12; j++) { float t = j / 12.0f; circle(x1 + (x2 - x1) * t, yA + (yB - yA) * t, 0.95f, ink); }
    }
    for (int k = 0; k < 4; k++) { circle(bx0 + k * sp, base - bh[k], 3.6f, bgc); circle(bx0 + k * sp, base - bh[k], 2.3f, ink); }
  }
  // (its own touch: the corner, a little more than drawn)
  if (in.tap && in.tap_x >= W - R - 4 && in.tap_y < y0 + R + 4 && hypotf(W - in.tap_x, in.tap_y - y0) < R + 8) {
    sfx(SFX_OPEN); stats_back = S_LIBRARY; screen = S_STATS; in.tap = 0;
  }
  return (int)R + 2;
}
static int cats_x; // (where the categories' list opens)
static void favs_open(int tab); // (favs.c)
#include "decor.c"
#include "lib.c"
static void vc_tri(float cx, float cy, float s, unsigned c);
static int text_wrap(int x, int y, unsigned c, int size, const char *s, int w, int lh);
#include "show.c"
#include "drive_ui.c"
#include "pin.c"
#include "privacy.c"
#include "clean.c"
#include "dupes.c"
#include "livearea.c"

// ----- the hero row over All: Continue watching, then Recently appreciated (♥) -----
// Up to sixteen of what's shown: videos stopped part way, then the ones given a heart most lately. It
// sits above the grid as part of the page (the grid's cards move down by its height), so it scrolls
// away with it. ↑ from the first row goes into it; ←→ along; ✕ opens; ↓ back to the grid.
#define LHERO_H 150
#define LHERO_CW 176
#define LHERO_CH 99
static int lhero[16], lhero_kind[16], nlhero, lhero_on, lhero_sel;
static float lhero_done[16]; // (how far each was watched: found when the row's made, not each frame)
static float lhero_x;
static int cmp_last_like(const void *a, const void *b) {
  const LvItem *x = &vault.items[*(const int *)a], *y = &vault.items[*(const int *)b];
  const char *lx = x->view >= 0 && vault.views[x->view].last_like ? vault.views[x->view].last_like : "";
  const char *ly = y->view >= 0 && vault.views[y->view].last_like ? vault.views[y->view].last_like : "";
  return strcmp(ly, lx);
}
static void lhero_build(void) {
  int had = nlhero;
  nlhero = 0;
  if (cfg.lmode == 0 && cat < 0 && !lsearch.n && !selecting && !book_inside && nshown > 6) {
    for (int k = 0; k < nshown && nlhero < 8; k++) {
      const LvItem *it = &vault.items[shown[k]];
      if (is_video(it) && it->path && resume_of(it) > 0 && !is_locked(shown[k])) { lhero_kind[nlhero] = 0; lhero[nlhero++] = shown[k]; }
    }
    int liked[64], nl = 0;
    for (int k = 0; k < nshown && nl < 64; k++)
      if (lv_likes_of(&vault, &vault.items[shown[k]]) > 0 && !is_locked(shown[k])) liked[nl++] = shown[k];
    qsort(liked, nl, sizeof *liked, cmp_last_like);
    for (int k = 0; k < nl && nlhero < 16; k++) {
      int dup = 0;
      for (int j = 0; j < nlhero; j++) dup |= lhero[j] == liked[k];
      if (!dup) { lhero_kind[nlhero] = 1; lhero[nlhero++] = liked[k]; }
    }
    if (nlhero < 2) nlhero = 0; // (one alone isn't a row)
  }
  for (int k = 0; k < nlhero; k++) {
    const LvItem *it = &vault.items[lhero[k]];
    lhero_done[k] = lhero_kind[k] == 0 && it->duration > 0 ? fminf(1, resume_of(it) / (float)it->duration) : 0;
  }
  if (!nlhero) lhero_on = 0;
  if (lhero_sel >= nlhero) lhero_sel = nlhero ? nlhero - 1 : 0;
  if (nlhero != had) lhero_x = 0;
  lhero_offset();
}
// (the grid moved down under the row: also after a relayout, which doesn't need the row worked out again)
static void lhero_offset(void) {
  if (nlhero) {
    for (int k = 0; k < lgrid.n; k++) lgrid.r[k].y += LHERO_H;
    lgrid.height += LHERO_H;
  }
}
// its input (before the grid's): 1 if it took the buttons this frame
static int lhero_input(void) {
  if (!nlhero) return 0;
  if (!lhero_on) {
    // (↑ from the first row of the grid: into it)
    if ((in.pressed & SCE_CTRL_UP) && sel >= 0 && sel < lgrid.n && lgrid.row[sel] == 0) {
      lhero_on = 1; sfx(SFX_TICK);
      // (the card nearest above)
      float cx = lgrid.r[sel].x + lgrid.r[sel].w / 2 + lhero_x;
      lhero_sel = (int)(cx / (LHERO_CW + 12));
      if (lhero_sel >= nlhero) lhero_sel = nlhero - 1;
      in.pressed &= ~SCE_CTRL_UP;
      return 1;
    }
    return 0;
  }
  if (in.pressed & SCE_CTRL_RIGHT) { if (lhero_sel < nlhero - 1) { lhero_sel++; sfx(SFX_TICK); } }
  if (in.pressed & SCE_CTRL_LEFT) { if (lhero_sel > 0) { lhero_sel--; sfx(SFX_TICK); } }
  if (in.pressed & SCE_CTRL_DOWN) { lhero_on = 0; sfx(SFX_TICK); }
  if (in.pressed & SCE_CTRL_CIRCLE) { lhero_on = 0; sfx(SFX_BACK); }
  int open = (in.pressed & SCE_CTRL_CROSS) != 0;
  in.pressed &= ~(SCE_CTRL_RIGHT | SCE_CTRL_LEFT | SCE_CTRL_DOWN | SCE_CTRL_UP | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_SQUARE | SCE_CTRL_TRIANGLE);
  scroll += (0 - scroll) * ease(0.3f); // (in sight)
  if (open) {
    for (int k = 0; k < nshown; k++) if (shown[k] == lhero[lhero_sel]) { lhero_on = 0; lib_open(k); return 2; }
  }
  return 1;
}
// drawn at the top of the page (y: where the page starts on screen); the thumbnails it needs go in want
static int lhero_draw(float y, int *want, int nwant_max) {
  if (!nlhero || y + LHERO_H < TOP - 4) return 0;
  int nw = 0;
  // where along: the chosen one in sight
  if (lhero_on) {
    float want_x = lhero_sel * (LHERO_CW + 12) - (W - 2 * JMARGIN - LHERO_CW) / 2;
    float maxx = nlhero * (LHERO_CW + 12) - 12 - (W - 2 * JMARGIN);
    if (want_x > maxx) want_x = maxx;
    if (want_x < 0) want_x = 0;
    lhero_x += (want_x - lhero_x) * ease(0.25f);
  }
  if (in.touching && in.down_y > y + 20 && in.down_y < y + 20 + LHERO_CH && abs(in.tx - in.down_x) > abs(in.ty - in.down_y)) {
    lhero_x -= in.drag_dx;
  }
  float maxx = fmaxf(0, nlhero * (LHERO_CW + 12) - 12 - (W - 2 * JMARGIN));
  if (!in.touching) { if (lhero_x < 0) lhero_x *= decay(0.7f); if (lhero_x > maxx) lhero_x = maxx + (lhero_x - maxx) * 0.7f; }
  // the titles: above the first of each kind
  float ring_x = -1, ring_y = 0;
  for (int k = 0; k < nlhero; k++) {
    float x = JMARGIN + k * (LHERO_CW + 12) - lhero_x, cy = y + 24;
    if (k == 0 || lhero_kind[k] != lhero_kind[k - 1]) {
      const char *ttl = lhero_kind[k] == 0 ? "Continue watching" : "Recently appreciated";
      float tx = fmaxf(JMARGIN, x);
      if (tx < W) text(tx, y + 16, DIM, 15, ttl);
    }
    if (x > W || x + LHERO_CW < 0) continue;
    int i = lhero[k];
    vita2d_texture *t = thumb_of(i);
    if (t) draw_cover(t, x, cy, LHERO_CW, LHERO_CH, RGBA(255, 255, 255, 255));
    else {
      skeleton(x, cy, LHERO_CW, LHERO_CH, 8);
      if (nw < nwant_max && tstate[i] != 3) { want_size(i, LHERO_CW, LHERO_CH); want[nw++] = i; }
    }
    const LvItem *it = &vault.items[i];
    if (lhero_kind[k] == 0 && it->duration > 0) {
      // how far it got
      float f = lhero_done[k];
      rrect(x, cy + LHERO_CH - 4, LHERO_CW, 4, 2, RGBA(0, 0, 0, 160));
      rrect(x, cy + LHERO_CH - 4, LHERO_CW * f, 4, 2, ACC);
      circle(x + 18, cy + LHERO_CH - 22, 12, RGBA(0, 0, 0, 150));
      vc_tri(x + 19, cy + LHERO_CH - 22, 7, TEXT);
    } else heart(x + 16, cy + LHERO_CH - 16, 6, ACC);
    if (lhero_on && k == lhero_sel) { ring_x = x; ring_y = cy; }
    if (hit((int)x, (int)cy, LHERO_CW, LHERO_CH) && in.tap_y > TOP && in.tap_y < H - BAR) {
      for (int j = 0; j < nshown; j++) if (shown[j] == i) { lhero_on = 0; lib_open(j); return -1; }
    }
  }
  if (ring_x > -1 && lsheet == LS_NONE) focus_ring(ring_x, ring_y, LHERO_CW, LHERO_CH);
  return nw;
}

static int last_top_row = -1;
// the card under a point on the screen, or -1
static int lib_card_at(int px, int py) {
  for (int k = grid_first(&lgrid, scroll - 8); k < lgrid.n && k < nshown; k++) {
    Rect r = lgrid.r[k];
    float ry = TOP + 6 + r.y - scroll;
    if (ry > H) break;
    if (px >= r.x && px < r.x + r.w && py >= ry && py < ry + r.h) return k;
  }
  return -1;
}
static void draw_library(void) {
  float content = lgrid.height + 12;
  float view_h = H - TOP - BAR;
  float max_scroll = fmaxf(0, content - view_h);
  // a sheet open: the grid stays drawn but doesn't take the buttons or touch
  if (side_gesture()) { in.tap = 0; in.touching = 0; in.flicked = 0; }
  Input saved_in = in;
  int sheet_up = lsheet != LS_NONE || side_open || side_t > 0;
  if (sheet_up) { in.pressed = 0; in.tap = 0; in.touching = 0; in.flicked = 0; in.ly = 0; in.dbl = 0; }
  if (!sheet_up && lhero_input() == 2) { in = saved_in; return; }
  int open_now = sheet_up ? 0 : lib_cross();
  int fast = sheet_up ? 0 : fast_scroll(max_scroll, view_h);
  if (fast) { in.touching = 0; in.tap = 0; }

  // --- moving: touch drag and flick, the stick, the D-pad selection ---
  // (two fingers pinched apart or together: the cards a size bigger or smaller, the three sizes the
  //  previews are made for; the card between the fingers stays where it is)
  {
    static float pd0;
    if (!sheet_up && in.tcount >= 2 && !book_shelf) {
      float d = hypotf(in.t2x - in.tx, in.t2y - in.ty);
      if (pd0 <= 0) pd0 = d;
      else if (d > pd0 * 1.35f || d < pd0 * 0.74f) {
        int step = d > pd0 ? 1 : -1, ns = lf.size + step;
        pd0 = d;
        if (ns >= 0 && ns <= 2) {
          float mx = (in.tx + in.t2x) / 2.0f, my = (in.ty + in.t2y) / 2.0f;
          // the card under the fingers, and where it is on screen
          int anchor = -1;
          float ay = my;
          for (int k = grid_first(&lgrid, scroll - 8); k < lgrid.n; k++) {
            Rect r = lgrid.r[k];
            float ry = TOP + 6 + r.y - scroll;
            if (ry > H) break;
            if (mx >= r.x && mx < r.x + r.w && my >= ry && my < ry + r.h) { anchor = k; ay = ry; break; }
          }
          if (anchor < 0) anchor = sel;
          lf.size = ns;
          relayout(); // (the same cards, a new size: not filtered and sorted again)
          if (anchor >= 0 && anchor < lgrid.n) { scroll = TOP + 6 + lgrid.r[anchor].y - ay; sel = anchor; }
          sfx(SFX_TOGGLE);
          say(SIZES[ns]);
        }
      }
      in.touching = 0; in.tap = 0; in.flicked = 0; // (a pinch isn't a scroll or a tap)
    } else if (in.tcount < 2) pd0 = 0;
  }
  // (a sideways drag in the hero row moves it, not the page)
  int hero_drag = nlhero && in.touching && in.down_y > TOP + 6 - scroll + 24 && in.down_y < TOP + 6 - scroll + 24 + LHERO_CH && abs(in.tx - in.down_x) > abs(in.ty - in.down_y);
  // (choosing many: a drag that starts sideways on a card paints the choice over every card it
  //  crosses; up and down still scroll)
  {
    static int painting, paint_val;
    if (!in.touching) painting = 0;
    else if (selecting && picked && !painting && in.moved && fabsf((float)(in.tx - in.down_x)) > 14 && fabsf((float)(in.tx - in.down_x)) > 1.5f * fabsf((float)(in.ty - in.down_y))) {
      int k0 = lib_card_at(in.down_x, in.down_y);
      if (k0 >= 0) { painting = 1; paint_val = !picked[shown[k0]]; picked[shown[k0]] = paint_val; npicked += paint_val ? 1 : -1; sfx(SFX_TOGGLE); }
    }
    if (painting) {
      int k = lib_card_at(in.tx, in.ty);
      if (k >= 0 && picked[shown[k]] != paint_val) { picked[shown[k]] = paint_val; npicked += paint_val ? 1 : -1; sfx(SFX_TICK); }
      in.drag_dy = 0; in.flicked = 0; in.tap = 0;
    }
  }
  if (in.touching && in.ty > TOP && in.ty < H - BAR && !hero_drag) { scroll -= in.drag_dy; scroll_v = 0; }
  if (in.flicked && in.down_y > TOP) scroll_v = -in.fvy * 1.6f;
  scroll += scroll_v * ft;
  scroll_v *= decay(0.94f);
  if (fabsf(scroll_v) < 0.2f) scroll_v = 0;
  scroll -= in.bdy * 1.5f;
  in.pressed |= stick_dirs(); // (the left stick moves the selection, as the D-pad)
  if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_LEFT | SCE_CTRL_DOWN | SCE_CTRL_UP)) {
    int v = sel_in_view(&lgrid, sel, scroll, view_h);
    if (v >= 0 && v != sel) { sel = v; in.pressed &= ~(SCE_CTRL_RIGHT | SCE_CTRL_LEFT | SCE_CTRL_DOWN | SCE_CTRL_UP); sfx(SFX_TICK); }
  }
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
    else if (r.y + r.h > scroll + view_h) scroll += (r.y + r.h - view_h + 10 - scroll) * ease(0.6f);
    if (r.y < scroll) scroll = r.y;
    if (r.y + r.h > scroll + view_h) scroll = r.y + r.h - view_h + 10;
  }
  if (scroll < 0) { scroll = in.touching ? scroll : scroll * 0.7f; if (!in.touching) scroll_v = 0; }
  if (scroll > max_scroll) { scroll = in.touching ? scroll : max_scroll + (scroll - max_scroll) * 0.7f; if (!in.touching) scroll_v = 0; }
  {
    static float last;
    if (!sheet_up && !fast) chrome_follow(scroll - last, scroll);
    if (sheet_up) chrome_target = 0;
    last = scroll;
  }

  PT("lib: moving");
  // --- the grid ---
  int first = grid_first(&lgrid, scroll - 8);
  // (a soft tick each time a row of pictures passes the top, as the Vita's own lists do)
  if (first < lgrid.n && lgrid.row[first] != last_top_row) { if (last_top_row >= 0 && (in.touching || scroll_v != 0 || in.ly)) sfx(SFX_TICK); last_top_row = lgrid.row[first]; }
  int wanted[64], nw = 0, last = first;
  // (flung fast: nothing new asked for, the cards in passing aren't decoded for nothing, they show
  //  their loading rings; what's in view is asked for as it slows)
  int flinging = fabsf(scroll_v) > 30 || fast;
  float fr_x = -1, fr_y = 0, fr_w = 0, fr_h = 0;
  {
    int hn = lhero_draw(TOP + 6 - scroll, wanted, 16);
    if (hn < 0) { in = saved_in; return; }
    nw += hn;
  }
  for (int k = first; k < nshown && k < lgrid.n; k++) {
    int i = shown[k];
    Rect rc = lgrid.r[k];
    float x = rc.x, y = TOP + 6 + rc.y - scroll, cw = rc.w, ch = rc.h;
    if (y > H) break;
    last = k + 1;
    if (y + ch < TOP - 4) continue;
    LvItem *it = &vault.items[i];
    vita2d_texture *t = thumb_of(i);
    int on = k == sel && !in.touching && !lhero_on && !sheet_up; // (a sheet open: the ring is the sheet's)
    float grow = on ? 3 : 0;
    if (on) { fr_x = x - grow; fr_y = y - grow; fr_w = cw + 2 * grow; fr_h = ch + 2 * grow; }
    int in_all_book = !book_shelf && is_cover_in_all(i);
    if (book_shelf || in_all_book) { // (the pages' edges behind it, a book's thickness)
      int bp = book_shelf ? book_count[i] : cover_pages ? cover_pages[i] : 1;
      int pages = bp > 40 ? 4 : bp > 12 ? 3 : 2;
      for (int e = pages; e >= 1; e--) rrect(x - grow + e * 3, y - grow + e * 3, cw + 2 * grow, ch + 2 * grow, 6, e & 1 ? RGBA(238, 232, 224, 255) : RGBA(180, 172, 164, 255));
    }
    if (t) draw_cover(t, x - grow, y - grow, cw + 2 * grow, ch + 2 * grow, RGBA(255, 255, 255, 255));
    else {
      if (tstate[i] == 3) { rrect(x - grow, y - grow, cw + 2 * grow, ch + 2 * grow, 8, SURF); text_center(x + cw / 2, y + ch / 2, FAINT, 14, "No picture"); }
      else {
        // (still coming: a shimmering card with a small loading ring; the grid never waits for it)
        skeleton(x - grow, y - grow, cw + 2 * grow, ch + 2 * grow, 8);
        spinner(x + cw / 2, y + ch / 2, 8);
        if (!flinging && nw < 64) { want_size(i, cw, ch); wanted[nw++] = i; }
      }
    }
    if (is_video(it) || is_gif(it)) {
      char d[24];
      if (is_gif(it)) snprintf(d, sizeof d, "GIF");
      else snprintf(d, sizeof d, "%d:%02d", (int)it->duration / 60, (int)it->duration % 60);
      int tw = text_w(13, d);
      rrect(x + cw - tw - 16, y + ch - 26, tw + 12, 20, 10, RGBA(0, 0, 0, 150));
      text(x + cw - tw - 10, y + ch - 11, TEXT, 13, d);
    }
    if (book_shelf) { // (a book: its name across the foot, its pages in the corner)
      // a book's look: its spine at the left (a shade and a light line), and how far it's been read
      grad(x, y, 14, ch, RGBA(0, 0, 0, 255), 150, 0, 1);
      vita2d_draw_rectangle(x + 14, y + 4, 1, ch - 8, RGBA(255, 255, 255, 40));
      if (book_read && book_read[i] > 0 && book_count[i] > 0) {
        float f = fminf(1, book_read[i] / (float)book_count[i]);
        rrect(x + 6, y + ch - 5, (cw - 12) * f, 3, 1, ACC);
      }
      grad(x, y + ch - 46, cw, 46, RGBA(0, 0, 0, 255), 0, 200, 0);
      text_fit((int)x + 8, (int)(y + ch - 10), TEXT, 14, it->collection, (int)cw - 16);
      char pc[24];
      snprintf(pc, sizeof pc, "%d p", book_count[i]);
      int tw = text_w(13, pc);
      rrect(x + cw - tw - 16, y + 6, tw + 12, 20, 10, RGBA(0, 0, 0, 160));
      text(x + cw - tw - 10, y + 21, TEXT, 13, pc);
    }
    if (in_all_book) { // (a comic in All: its spine, and a badge saying so, with its pages)
      grad(x, y, 14, ch, RGBA(0, 0, 0, 255), 150, 0, 1);
      char bl[32];
      snprintf(bl, sizeof bl, "COMIC \u00b7 %d p", cover_pages ? cover_pages[i] : 0);
      int bw = text_w(11, bl) + 34;
      rrect(x + 8, y + 8, bw, 22, 11, with_alpha(ACC, 235));
      icon("book", x + 21, y + 19, RGBA(20, 12, 16, 255));
      text(x + 32, y + 23, RGBA(20, 12, 16, 255), 11, bl);
    }
    if (lv_likes_of(&vault, it) > 0) { // (its hearts: how many times it's been appreciated)
      int nl = lv_likes_of(&vault, it);
      char nb[12]; snprintf(nb, sizeof nb, "%d", nl);
      rrect(x + 6, y + ch - 26, 24 + (nl > 1 ? text_w(11, nb) + 4 : 0), 20, 10, RGBA(0, 0, 0, 140));
      heart(x + 16, y + ch - 16, 6, ACC);
      if (nl > 1) text(x + 25, y + ch - 11, TEXT, 11, nb);
    }
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
      else { lib_open(k); in = saved_in; return; }
    }
  }
  if (fr_x >= 0) focus_ring(fr_x, fr_y, fr_w, fr_h);
  // (and the next screenful the way it's going, ready before it's needed: below going down, above
  //  going up)
  if (!flinging) {
    int up = scroll_v < -1 || in.ly < -0.2f || (in.touching && in.drag_dy > 2);
    for (int j = 0; j < 18 && nw < 64; j++) {
      int k = up ? first - 1 - j : last + j;
      if (k < 0 || k >= nshown) break;
      if (!thumb_of(shown[k]) && tstate[shown[k]] != 3) { if (k < lgrid.n) want_size(shown[k], lgrid.r[k].w, lgrid.r[k].h); wanted[nw++] = shown[k]; }
    }
  }
  ask_thumbs(wanted, nw); // (flinging: none, and the ones asked for before are let go)
  PT("lib: grid");
  if (!nshown) {
    if (cfg.lmode == 3 && !query[0]) {
      text_center(W / 2, TOP + 150, DIM, 20, "No comics yet");
      text_center(W / 2, TOP + 180, FAINT, 16, "Save a chapter from the Comics tab, or give a collection the category Comics");
    } else if (vault.n == 0 && !query[0]) {
      // (a new vault: where things come from)
      decor_draw(DEC_TEARJU, W - 150, H - 70, 300, -150); // (still, and fainter: a negative alpha) // (one of the girls, waiting by it)
      text_center(W / 2, TOP + 140, TEXT, 22, "Your vault is empty");
      text_center(W / 2, TOP + 172, DIM, 16, "Find pictures and videos in the Browser and save them here,");
      text_center(W / 2, TOP + 196, DIM, 16, "or save comics from the Comics tab.");
    } else
    { decor_draw(query[0] ? DEC_CROPTOP : DEC_SONICO, W - 140, H - BAR + 6, 260, 255); }
    text_center(W / 2, TOP + 150, DIM, 20, query[0] ? "Nothing matches that search" : "Nothing here");
    if (query[0]) text_center(W / 2, TOP + 180, FAINT, 16, "Tap Library below to see everything again");
  }
  // the scroll bar
  if (max_scroll > 0) {
    float bh = fmaxf(30, view_h * view_h / content);
    float by = TOP + (view_h - bh) * fminf(1, fmaxf(0, scroll / max_scroll));
    rrect(W - 6, by, 4, bh, 2, with_alpha(TEXT, 70));
  }

  PT("lib: empty+bar");
  // --- the top: one row of large buttons (tucked away while scrolling down) ---
  int hy = HDR_Y;
  vita2d_draw_rectangle(0, hy, W, TOP_FULL, with_alpha(BG, 250));
  int sx = W - 12;
  int title_end;
  if (book_inside) { // (in a book: back to the shelf, and its name)
    rrect(10, hy + 8, 48, 40, 20, SURF2);
    icon("back", 34, hy + 28, TEXT);
    if (hit(10, hy + 4, 48, 48)) { sfx(SFX_BACK); close_book(); in = saved_in; in.pressed = 0; in.tap = 0; draw_bar(); return; }
    title_end = 70;
  } else {
    title_end = 10 + draw_modes(10, hy + 8) + 10;
    // the category: one button, its list opening under it
    char cl[80];
    snprintf(cl, sizeof cl, "%s", smart ? SMART[smart - 1] : cat < 0 ? "All categories" : vault.cats[cat]);
    int cw = text_w(16, cl) + 44;
    if (cw > 180) cw = 180;
    rrect(title_end, hy + 8, cw, 40, 20, cat < 0 && !smart ? SURF2 : with_alpha(ACC, 110));
    text_fit(title_end + 16, hy + 34, TEXT, 16, cl, cw - 40);
    icon("chev_r", title_end + cw - 18, hy + 28, DIM);
    if (hit(title_end, hy + 4, cw, 48)) { sfx(SFX_OPEN); lsheet = LS_CATS; lsheet_sel = cat + 1; cats_x = title_end; }
    title_end += cw + 10;
  }
  char kl[64];
  int nf = (cfg.kind != 0) + (lf.nsfw_max != 0) + (lf.stars_min != 0) + (lf.album != 0) + (lf.coll[0] != 0 && !book_inside);
  if (nf) snprintf(kl, sizeof kl, "Filters \u00b7 %d", nf); else snprintf(kl, sizeof kl, "Filters");
  int hb;
  sx -= stats_corner() + 2; // (Your stats, in the corner)
  // (Videos and Comics: their favourites, kept as links)
  if (!book_inside && (cfg.lmode == 2 || cfg.lmode == 3)) {
    int x = sx - 44, y = HDR_Y;
    rrect(x, y + 8, 44, 40, 20, with_alpha(ACC, 90));
    heart(x + 22, y + 28, 8, ACC);
    if (hit(x, y + 4, 44, 48)) { sfx(SFX_OPEN); favs_open(cfg.lmode == 3); in = saved_in; in.pressed = 0; in.tap = 0; return; }
    sx -= 52;
  }
  if (!book_inside) {
    sx -= head_button(sx, "sort", SORTS[cfg.sort], SURF2, &hb) + 8;
    if (hb || (!selecting && (in.pressed & SCE_CTRL_TRIANGLE))) { sfx(SFX_OPEN); lsheet = LS_SORT; lsheet_sel = cfg.sort; }
  }
  if (nf) { // (one tap clears them all)
    int cx2 = sx - 40, cy2 = HDR_Y + 8;
    rrect(cx2, cy2, 40, 40, 20, with_alpha(ACC, 80));
    icon("xmark", cx2 + 20, cy2 + 20, TEXT);
    if (hit(cx2, cy2 - 4, 40, 48)) { sfx(SFX_CLOSE); cfg.kind = 0; lf.nsfw_max = 0; lf.stars_min = 0; lf.album = 0; if (!book_inside) lf.coll[0] = 0; refilter(); say("Filters cleared"); }
    sx -= 48;
  }
  sx -= head_button(sx, "sliders", kl, nf ? with_alpha(ACC, 110) : SURF2, &hb) + 8;
  if (hb || (!selecting && (in.pressed & SCE_CTRL_SQUARE))) { sfx(SFX_OPEN); lsheet = LS_FILTERS; lsheet_sel = 0; }
  sx -= online_inset(sx) + 8; // (the connection, left of Filters: a small pressed-in button, green or red)
  if (query[0] && sx - title_end > 90) {
    char q[160];
    snprintf(q, sizeof q, "\u201c%.12s%s\u201d", query, strlen(query) > 12 ? "\u2026" : "");
    sx -= head_button(sx, "xmark", q, with_alpha(ACC, 80), &hb) + 8;
    if (hb) { query[0] = 0; refilter(); }
  }
  {
    char info[200];
    if (book_inside) snprintf(info, sizeof info, "%s \u00b7 %d pages", lf.coll, nshown);
    else snprintf(info, sizeof info, "%d", nshown);
    int room = sx - title_end - 4;
    if (book_inside) text_fit(title_end, hy + 35, TEXT, 20, info, room);
    else if (room > text_w(14, info)) text(title_end, hy + 33, FAINT, 14, info);
  }
  PT("lib: header");
  // the modes: L / R (in a book: back to the shelf first)
  if (in.pressed & (SCE_CTRL_RTRIGGER | SCE_CTRL_LTRIGGER)) {
    if (book_inside) close_book();
    set_mode((cfg.lmode + ((in.pressed & SCE_CTRL_RTRIGGER) ? 1 : 3)) % 4);
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
    if (open_now && nshown) lib_open(sel);
    if ((in.pressed & SCE_CTRL_START) && nshown && !book_shelf) {
      int k = show_pool_on(sel < nshown ? shown[sel] : -1); // (the slideshow's own list: pictures, GIFs, short clips)
      if (nshown) { sfx(SFX_SLIDESHOW); open_viewer(k, 1); } else { show_pool = 0; refilter(); sfx(SFX_ERROR); say("No pictures, GIFs or short clips to show"); }
    }
    if ((in.pressed & SCE_CTRL_CIRCLE) && book_inside) { sfx(SFX_BACK); close_book(); }
    else if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_TAB); screen = S_WEB; } // (○ goes round the tabs: Library, Browser, Settings)
    // a flick across the grid (sideways, not from the right edge): the next or previous mode
    if (in.flicked && in.down_y > TOP && in.down_x < W - 40 && fabsf(in.fvx) > 16 && fabsf(in.fvx) > 2.5f * fabsf(in.fvy) && !book_inside && !fast)
      set_mode((cfg.lmode + (in.fvx < 0 ? 1 : 3)) % 4);
  }
  draw_side_tab();
  draw_bar();
  in = saved_in;
  // (a sheet opened this frame doesn't also take the press that opened it: □ would close it again)
  if (!sheet_up) { in.pressed = 0; in.tap = 0; in.dbl = 0; }
  if (lsheet == LS_FILTERS) draw_filters_sheet();
  else if (lsheet == LS_TAGS) draw_tags_sheet();
  else if (lsheet == LS_QUICK) draw_quick_sheet();
  else if (lsheet == LS_BATCH) draw_batch_sheet();
  else if (lsheet == LS_CATS) draw_cats_sheet();
  else if (lsheet == LS_SORT) draw_sort_sheet();
  draw_side();
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
static int tall_top;   // (a tall page just shown: it starts at its top)
static float tall_v;   // (and a flick keeps it moving)

// A video is closed at the start of the next frame, not in the middle of one: the frame being put
// together may already draw its picture, which lives in the player's own memory.
static LvVideo *closing[8];
static int nclosing;
static void video_close_later(LvVideo *v) {
  if (!v) return;
  if (nclosing < 8) closing[nclosing++] = v;
  // (eight in one frame can't happen; if it did, a leak is better than a crash)
}
// (closed on a thread of their own: a software player's threads take a while to stop, and the screen
// mustn't wait for them; it waits three frames first, so the GPU is surely done with the last picture)
static LvVideo *to_close[16];
static int nto_close, closer_on;
static pthread_mutex_t cmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ccv = PTHREAD_COND_INITIALIZER;
// ...and videos are opened there too (opening a file, reading its streams, starting its decoder: too
// slow for the drawing thread): the full view asks, shows the thumbnail meanwhile, and takes the player
// when it's ready, if it's still for the same picture (vopen_gen).
static struct { char path[800], clip[800]; int loop; uint8_t key[32]; int has_key, gen; } vopen_job;
static volatile int vopen_want, vopen_gen, vopen_ready_gen = -1, vopen_ready_clip, vopen_fail_gen = -1;
static LvVideo *volatile vopen_ready;
static void *closer_main(void *arg) {
  pin_to_core(2); // (off the drawing thread's core)
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&cmx);
    while (!nto_close && !vopen_want) pthread_cond_wait(&ccv, &cmx);
    if (vopen_want) {
      typeof(vopen_job) j = vopen_job;
      vopen_want = 0;
      pthread_mutex_unlock(&cmx);
      int clip = 0;
      LvVideo *v = j.path[0] ? lv_video_open(j.path, j.has_key ? j.key : NULL, j.loop) : NULL;
      if (!v && j.clip[0]) { v = lv_video_open(j.clip, j.has_key ? j.key : NULL, 1); clip = 1; }
      memset(j.key, 0, sizeof j.key);
      if (!v) vopen_fail_gen = j.gen; // (nothing to play: the spinner stops)
      pthread_mutex_lock(&cmx);
      if (v && j.gen == vopen_gen && !vopen_ready) { vopen_ready = v; vopen_ready_gen = j.gen; vopen_ready_clip = clip; v = NULL; }
      pthread_mutex_unlock(&cmx);
      if (v) lv_video_close(v); // (too late: another picture by now)
      continue;
    }
    LvVideo *v = to_close[--nto_close];
    pthread_mutex_unlock(&cmx);
    sceKernelDelayThread(50000);
    lv_video_close(v);
  }
  return NULL;
}
static void video_open_async(const char *path, const char *clip, int loop) {
  make_room(32u << 20); // (a player: its buffers and frames, ~25 MB)
  pthread_mutex_lock(&cmx);
  vopen_gen++;
  snprintf(vopen_job.path, sizeof vopen_job.path, "%s", path ? path : "");
  snprintf(vopen_job.clip, sizeof vopen_job.clip, "%s", clip ? clip : "");
  vopen_job.loop = loop;
  vopen_job.has_key = vault.has_key;
  if (vault.has_key) memcpy(vopen_job.key, vault.key, 32);
  vopen_job.gen = vopen_gen;
  vopen_want = 1;
  pthread_cond_signal(&ccv);
  pthread_mutex_unlock(&cmx);
}
// (a player opened for a picture no longer shown, or not yet taken: let go)
static void video_open_cancel(void) {
  pthread_mutex_lock(&cmx);
  vopen_gen++;
  vopen_want = 0;
  LvVideo *v = vopen_ready;
  vopen_ready = NULL;
  pthread_mutex_unlock(&cmx);
  if (v) video_close_later(v);
}
static void closer_start(void) {
  if (closer_on) return;
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024); // (it opens videos: FFmpeg and libvpx want room)
  closer_on = pthread_create(&t, &at, closer_main, NULL) == 0 ? 1 : -1;
}
static void videos_close_now(void) {
  if (!nclosing) return;
  closer_start();
  pthread_mutex_lock(&cmx);
  for (int k = 0; k < nclosing; k++) {
    if (closer_on == 1 && nto_close < 16) to_close[nto_close++] = closing[k];
    else { pthread_mutex_unlock(&cmx); vita2d_wait_rendering_done(); lv_video_close(closing[k]); pthread_mutex_lock(&cmx); }
  }
  nclosing = 0;
  pthread_cond_signal(&ccv);
  pthread_mutex_unlock(&cmx);
}
static void video_open_cancel(void);
static void stop_video(void) {
  video_open_cancel();
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
static void vhero_reset(void);
static void show_item(int k) {
  vhero_reset(); // (a new one: not mid-swipe)
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
  tall_top = 1; tall_v = 0;
  big_hi_item = -1;
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
    int has_clip = lv_clip_path(&vault, it, clip, sizeof clip);
    if (can || has_clip) video_open_async(can ? it->path : NULL, has_clip ? clip : NULL, slideshow);
  }
  ask_big(shown[vk]);
}
// The full views' back button (the Library's and the Browser's): a round glass button with the back
// arrow, big enough for a thumb, the whole corner answering a tap
static void fv_back_draw(int a) {
  circle(38, 34, 25, with_alpha(RGBA(0, 0, 0, 255), a * 90 / 255));
  circle(38, 34, 23, with_alpha(mix_col(SURF2, ACC, 0.18f), a * 215 / 255));
  circle(38, 28, 15, with_alpha(TEXT, a / 16)); // (light from above)
  icon("back", 37, 34, with_alpha(TEXT, a));
}
static int fv_back_hit(void) { return in.tap && in.tap_x < 70 && in.tap_y < 82; }
// A picture's (or video's) resolution as a rank, by its pixels against the usual sizes:
//   S+ 4K and up (8.3 MP)  ·  S 1440p and up, gold  ·  A+ about 1080p or a little over  ·  A a little
//   under 1080p, green  ·  B+ about 720p, blue  ·  B  ·  C+  ·  C small, pink
static const char *res_rank(int w, int h, unsigned *col) {
  long long px = (long long)w * h;
  if (px >= 8000000) { *col = RGBA(255, 214, 102, 255); return "S+"; }
  if (px >= 3600000) { *col = RGBA(232, 185, 49, 255); return "S"; }
  if (px >= 1900000) { *col = RGBA(110, 214, 140, 255); return "A+"; }
  if (px >= 1300000) { *col = RGBA(76, 195, 107, 255); return "A"; }
  if (px >= 850000) { *col = RGBA(90, 160, 240, 255); return "B+"; }
  if (px >= 480000) { *col = RGBA(74, 128, 214, 255); return "B"; }
  if (px >= 220000) { *col = RGBA(240, 120, 165, 255); return "C+"; }
  *col = RGBA(232, 93, 138, 255); return "C";
}
// (the rank in a squircle beside the back button: its colour, a glassy top; S+ with a shine running
//  across it)
static unsigned res_info_at, res_drawn_at; // (the ranks box: open since; the badge last drawn)
// (a tap for the badge: on it or its label, the ranks explained; while they show, any tap closes them)
static int res_info_tap(void) {
  if (!in.tap) return 0;
  if (res_info_at) { res_info_at = 0; sfx(SFX_CLOSE); return 1; }
  if (in.tap_x >= 70 && in.tap_x < 216 && in.tap_y < 58) { res_info_at = frame_no ? frame_no : 1; sfx(SFX_OPEN); return 1; }
  return 0;
}
static int res_rel; // (the Library's badge: ranked against the vault's biggest picture, not the usual sizes)
static long long vault_max_px(void) {
  static long long mx; static int at_n = -1;
  if (at_n != vault.n) { mx = 0; for (int i = 0; i < vault.n; i++) { long long p = (long long)vault.items[i].width * vault.items[i].height; if (p > mx) mx = p; } at_n = vault.n; }
  return mx;
}
static void res_badge(int w, int h, int a) {
  if (w <= 0 || h <= 0) return;
  unsigned c;
  long long mx = res_rel ? vault_max_px() : 0;
  float boost = mx > 0 && mx < 8000000 ? 8000000.0f / mx : 1;
  const char *r = res_rank((int)(w * sqrtf(boost)), (int)(h * sqrtf(boost) + 0.5f), &c);
  float x = 74, y = 16, bw = 46, bh = 34;
  rrect(x - 1, y + 2, bw + 2, bh, 12, with_alpha(RGBA(0, 0, 0, 255), a * 80 / 255));
  rrect(x, y, bw, bh, 12, with_alpha(c, a));
  rrect(x + 2, y + 2, bw - 4, bh * 0.42f, 9, with_alpha(RGBA(255, 255, 255, 255), a * 55 / 255));
  if (r[0] == 'S' && r[1] == '+') { // (a shine sweeping across)
    float sx = fmodf(frame_no * 1.6f, bw + 60) - 30;
    if (sx > -10 && sx < bw - 4) rrect(x + fmaxf(2, sx), y + 2, fminf(10, bw - 2 - fmaxf(2, sx)), bh - 4, 4, with_alpha(RGBA(255, 255, 255, 255), a * 90 / 255));
  }
  text_center((int)(x + bw / 2), (int)(y + 24), with_alpha(RGBA(24, 16, 20, 255), a), 18, r);
  // (what it is, beside it: "Quality" and its size; a tap on either opens the ranks explained)
  char sz[32];
  snprintf(sz, sizeof sz, "%d \xc3\x97 %d", w, h);
  { int lw = text_w(11, sz), qw = text_w(12, "Quality"); if (qw > lw) lw = qw;
    rrect(x + bw + 3, y, lw + 12, bh, 9, with_alpha(RGBA(0, 0, 0, 255), a * 70 / 255)); } // (a dim plate: readable on a bright picture)
  text((int)(x + bw + 8), (int)y + 14, with_alpha(TEXT, a), 12, "Quality");
  text((int)(x + bw + 8), (int)y + 29, with_alpha(TEXT, a * 170 / 255), 11, sz);
  if (res_info_at && res_drawn_at + 1 < frame_no) res_info_at = 0; // (the controls were hidden: closed with them)
  res_drawn_at = frame_no;
  unsigned info_at = res_info_at;
  if (!info_at) return;
  static const struct { const char *r, *what; } RK[8] = {
    {"S+", "4K and up"}, {"S", "1440p and up"}, {"A+", "about 1080p"}, {"A", "a little under 1080p"},
    {"B+", "about 720p"}, {"B", "under 720p"}, {"C+", "small"}, {"C", "very small"}};
  float u = fminf(1, (frame_no - info_at) / 10.0f), e = 1 - (1 - u) * (1 - u);
  int ba = (int)(a * e), bx = (int)x, by = (int)(y + bh + 8 - (1 - e) * 10), bw2 = 236, bh2 = 40 + 8 * 20;
  rrect(bx, by, bw2, bh2, 10, RGBA(0, 0, 0, ba * 150 / 255));
  rrect(bx + 1, by + 1, bw2 - 2, 26, 9, with_alpha(TEXT, ba / 16));
  text(bx + 12, by + 22, with_alpha(TEXT, ba), 13, res_rel ? "Quality: against your biggest" : "Quality: by its resolution");
  for (int k = 0; k < 8; k++) {
    unsigned kc;
    static const int px[8] = {3840 * 2160, 2560 * 1440, 1920 * 1080, 1280 * 1024, 1280 * 720, 800 * 600, 640 * 400, 1};
    res_rank(px[k], 1, &kc);
    int ry = by + 34 + k * 20, on = !strcmp(RK[k].r, r);
    if (on) rrect(bx + 6, ry, bw2 - 12, 19, 6, with_alpha(TEXT, ba / 8));
    rrect(bx + 12, ry + 2, 30, 15, 5, with_alpha(kc, ba));
    text_center(bx + 27, ry + 14, with_alpha(RGBA(24, 16, 20, 255), ba), 11, RK[k].r);
    text(bx + 52, ry + 14, with_alpha(TEXT, on ? ba : ba * 190 / 255), 12, RK[k].what);
  }
}
static void fmt_time(uint64_t ms, char *o, size_t n);
static unsigned resumed_at; // (a video carried on from where it was stopped: when)
// "Similar to this": the two tags of these that say most what it is: specific ones (a character, a
// series: long, with underscores or brackets) before generic ones, never the generic ones of a list,
// never one already searched for (exclude). out: "a b", or "" when there's none.
static void similar_tags(const char *tags, const char *exclude, char *out, size_t cap) {
  static const char *const GENERIC[] = {"1girl", "1boy", "2girls", "solo", "highres", "absurdres", "video", "animated", "sound", "tagme",
    "mp4", "webm", "gif", "nude", "sex", "penis", "pussy", "breasts", "large_breasts", "nipples", "hentai", "anime", "uncensored", "censored",
    "english", "japanese", "translated", "commentary", "3d", "2d", "looking_at_viewer", "blush", "smile", "open_mouth", NULL};
  char b[2048], best[2][64] = {"", ""};
  int score[2] = {-1, -1};
  snprintf(b, sizeof b, "%s", tags ? tags : "");
  for (char *s = strtok(b, " "); s; s = strtok(NULL, " ")) {
    size_t n = strlen(s);
    if (n < 3 || n > 60 || strchr(s, ':')) continue;
    int skip = 0;
    for (int g = 0; GENERIC[g] && !skip; g++) skip = !strcasecmp(s, GENERIC[g]);
    if (skip || (exclude && strcasestr(exclude, s))) continue;
    int sc = (int)n + (strchr(s, '(') ? 20 : 0) + (strchr(s, '_') ? 6 : 0);
    if (sc > score[0]) { score[1] = score[0]; memcpy(best[1], best[0], 64); score[0] = sc; snprintf(best[0], 64, "%s", s); }
    else if (sc > score[1]) { score[1] = sc; snprintf(best[1], 64, "%s", s); }
  }
  snprintf(out, cap, "%s%s%s", best[0], best[1][0] ? " " : "", best[1]);
}
// ----- the video controls, the same in both full views (the Library's and the Browser's) -----
// A play/pause button in the middle with ten seconds back and on either side; the bar at the foot
// with what has downloaded as a lighter part, the time gone, and a pill with the time left. A skip
// (by a button, a double tap at a side, or ↑↓/←→) flashes a ripple at that side even when the
// controls are hidden.
static unsigned vflash_at;
static int vflash_dir;
static void vflash(int dir) { vflash_at = frame_no ? frame_no : 1; vflash_dir = dir; }
#define VC_BTN_Y (H / 2)
static void vc_tri(float cx, float cy, float s, unsigned c) { // (a play triangle, pointing right)
  for (int k = 0; k < (int)(s * 1.6f); k++) vita2d_draw_rectangle(cx - s * 0.55f + k, cy - s * 0.8f + k / 2.0f, 1, s * 1.6f - k, c);
}
static void vc_skip_icon(float cx, float cy, int dir, unsigned c) {
  // a round arrow and "10"
  for (int k = 0; k < 28; k++) {
    float ang = (dir > 0 ? -2.6f : -0.54f) + k * 0.115f * (dir > 0 ? 1 : -1) * -1;
    vita2d_draw_rectangle(cx + cosf(ang) * 15 - 1, cy + sinf(ang) * 15 - 1, 2.5f, 2.5f, c);
  }
  float ax = dir > 0 ? cx + 13 : cx - 13, ay = cy - 9;
  vita2d_draw_line(ax, ay, ax + (dir > 0 ? -7 : 7), ay - 4, c);
  vita2d_draw_line(ax, ay, ax + (dir > 0 ? -5 : 5), ay + 6, c);
  text_center(cx, cy + 6, c, 13, "10");
}
// the ripple of a skip, at its side
static void vctl_flash_draw(void) {
  if (!vflash_at) return;
  unsigned age = frame_no - vflash_at;
  if (age > 36) { vflash_at = 0; return; }
  float p = age / 36.0f, a = 1 - p;
  float cx = vflash_dir > 0 ? W - 150 : 150;
  circle(cx, H / 2, 70 + 60 * p, with_alpha(TEXT, (int)(60 * a)));
  circle(cx, H / 2, 48, RGBA(0, 0, 0, (int)(190 * a)));
  text_center(cx, H / 2 + 7, with_alpha(TEXT, (int)(255 * a)), 20, vflash_dir > 0 ? "+10 s" : "−10 s");
}
// The controls drawn at alpha a (0-240); have: the share downloaded (0-1), or a negative number for
// all of it
static void vctl_draw(const LvVideo *v, int a, float have) {
  if (!v) return;
  uint64_t d = lv_video_duration(v), q = lv_video_pos(v);
  float f = d ? fminf(1, q / (float)d) : 0;
  grad(0, H - 84, W, 84, RGBA(0, 0, 0, 255), 0, 90 * a / 240, 0);
  // the middle: back, play/pause, on
  circle(W / 2, VC_BTN_Y, 38, RGBA(0, 0, 0, 120 * a / 240));
  circle(W / 2, VC_BTN_Y, 38, with_alpha(TEXT, 30 * a / 240));
  circle(W / 2, VC_BTN_Y, 36, RGBA(0, 0, 0, 130 * a / 240));
  if (lv_video_paused(v)) vc_tri(W / 2 + 4, VC_BTN_Y, 20, with_alpha(TEXT, a));
  else { rrect(W / 2 - 13, VC_BTN_Y - 16, 9, 32, 3, with_alpha(TEXT, a)); rrect(W / 2 + 4, VC_BTN_Y - 16, 9, 32, 3, with_alpha(TEXT, a)); }
  for (int s = -1; s <= 1; s += 2) {
    float cx = W / 2 + s * 120;
    circle(cx, VC_BTN_Y, 28, RGBA(0, 0, 0, 110 * a / 240));
    vc_skip_icon(cx, VC_BTN_Y, s, with_alpha(TEXT, a));
  }
  // the bar: the whole, what's here, what's been played, the knob
  const float bx = 40, bw = W - 80 - 92, by = H - 30;
  rrect(bx, by, bw, 6, 3, with_alpha(TEXT, a / 5));
  if (have >= 0) rrect(bx, by, bw * fminf(1, have), 6, 3, with_alpha(TEXT, a / 2));
  else rrect(bx, by, bw, 6, 3, with_alpha(TEXT, a / 3));
  rrect(bx, by, bw * f, 6, 3, with_alpha(ACC, a));
  circle(bx + bw * f, by + 3, 8, with_alpha(TEXT, a));
  char t1[16], t2[20];
  fmt_time(q, t1, sizeof t1);
  text(bx, by - 10, with_alpha(TEXT, a), 14, t1);
  // the time left, in a pill
  fmt_time(d > q ? d - q : 0, t2 + 1, sizeof t2 - 1);
  t2[0] = '-';
  int pw = text_w(14, t2) + 20;
  rrect(W - 40 - pw, by - 9, pw, 24, 12, with_alpha(SURF2, a));
  text(W - 40 - pw + 10, by + 8, with_alpha(TEXT, a), 14, t2);
}
// A tap while the controls show: 1 if it was theirs (the play button, a skip, the bar)
static int vctl_tap(LvVideo *v, int x, int y, int64_t *seek_to) {
  if (!v) return 0;
  uint64_t d = lv_video_duration(v);
  if (hypotf(x - W / 2, y - VC_BTN_Y) < 44) { lv_video_pause(v, !lv_video_paused(v)); sfx(SFX_TOGGLE); return 1; }
  for (int s = -1; s <= 1; s += 2)
    if (hypotf(x - (W / 2 + s * 120), y - VC_BTN_Y) < 34) { *seek_to = (int64_t)lv_video_pos(v) + s * 10000; vflash(s); return 2; }
  const float bx = 40, bw = W - 80 - 92;
  if (y > H - 60 && x < bx + bw + 20 && d) { *seek_to = (int64_t)(d * fminf(1, fmaxf(0, (x - bx) / bw))); return 2; }
  return 0;
}
// A double tap on a video: at the left or right third, ten seconds back or on (1 / -1), else 0
static int vctl_dbl_side(int x) { return x < W / 3 ? -1 : x > W * 2 / 3 ? 1 : 0; }

// A picture drawn in a rectangle: contained (cover 0) to covering it, cropped (cover 1)
static void draw_in_rect(vita2d_texture *t, float tw, float th, float x, float y, float w, float h, float cover, unsigned tint) {
  DRAW_GUARD(tw, th, x, y, w, h, cover);
  float sc = fminf(w / tw, h / th), sv = fmaxf(w / tw, h / th), s = sc + (sv - sc) * cover;
  float pw = fminf(tw, w / s), ph = fminf(th, h / s);
  vita2d_draw_texture_tint_part_scale(t, x + (w - pw * s) / 2, y + (h - ph * s) / 2, (tw - pw) / 2, (th - ph) / 2, pw, ph, s, s, tint);
}
// Swipe down to close: the picture follows the finger, smaller, the black behind it thinning; let go
// far enough and it flies back into its card in the grid (the "hero"), else springs back.
typedef struct { float drag; int closing; unsigned at; float x, y, w, h; } HeroClose;
static int hero_drag(HeroClose *hc, int may) {
  // (call while touching; 1 while this gesture is a swipe down)
  if (!may) return 0;
  float dy = in.ty - in.down_y, dx = in.tx - in.down_x;
  if (hc->drag > 0 || (dy > 26 && dy > fabsf(dx) * 1.3f)) { hc->drag = fmaxf(0, dy); return 1; }
  return 0;
}
static int hero_release(HeroClose *hc) {
  // (call when not touching: 1 when it should start flying home)
  if (hc->drag > 110 && !hc->closing) { hc->closing = 1; hc->at = frame_no; return 1; }
  hc->drag *= decay(0.6f);
  if (hc->drag < 1) hc->drag = 0;
  return 0;
}
// where the picture is drawn this frame, from where it would be (x, y, w, h) and the card (cx..ch)
static float hero_place(HeroClose *hc, float *x, float *y, float *w, float *h, Rect card, int has_card) {
  if (hc->closing) {
    float p = fminf(1, (frame_no - hc->at) / 12.0f), e = 1 - (1 - p) * (1 - p) * (1 - p);
    float fx = hc->x, fy = hc->y, fw = hc->w, fh = hc->h;
    float tx = has_card ? card.x : W / 2 - 40, ty = has_card ? card.y : H / 2 - 40, tw = has_card ? card.w : 80, th = has_card ? card.h : 80;
    *x = fx + (tx - fx) * e; *y = fy + (ty - fy) * e; *w = fw + (tw - fw) * e; *h = fh + (th - fh) * e;
    return e; // (0 to 1: how far home; also how much it covers its card)
  }
  if (hc->drag > 0) {
    float k = 1 - fminf(0.45f, hc->drag / (float)H * 0.7f);
    float cx = *x + *w / 2, cy = *y + *h / 2 + hc->drag;
    *w *= k; *h *= k; *x = cx - *w / 2; *y = cy - *h / 2;
  }
  hc->x = *x; hc->y = *y; hc->w = *w; hc->h = *h;
  return 0;
}
static int hero_done(const HeroClose *hc) { return hc->closing && frame_no - hc->at >= 12; }
// how dark the screen behind is: thinning as it's dragged, gone as it flies home
static int hero_backdrop(const HeroClose *hc) {
  if (hc->closing) return (int)(255 * (1 - fminf(1, (frame_no - hc->at) / 12.0f)));
  return (int)(255 * (1 - fminf(0.85f, hc->drag / 300.0f)));
}

static HeroClose vhero; // (the Library's full view, swiped down to close)
static void vhero_reset(void) { if (!vhero.closing) memset(&vhero, 0, sizeof vhero); }
// Where a card is in the Library's grid once the full view closes (it scrolls there if it's out of sight)
static int lib_card_target(int k, Rect *o) {
  if (k < 0 || k >= lgrid.n) return 0;
  Rect r = lgrid.r[k];
  float sc = scroll;
  if (r.y < sc || r.y + r.h > sc + H - TOP - BAR) sc = r.y - 40;
  o->x = r.x; o->y = TOP + 6 + r.y - sc; o->w = r.w; o->h = r.h;
  return 1;
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
  memset(&vhero, 0, sizeof vhero);
  details = 0;
  details_y = 0;
  swipe_x = 0;
  chrome_until = frame_no + 180;
  screen = S_VIEWER;
  PT("open_viewer start");
  show_item(k);
  PT("show_item");
}
static void close_viewer(void) {
  sfx(SFX_BACK);
  PT("close start");
  save_edits_now();
  PT("save_edits");
  account_time();
  stop_video();
  PT("stop_video");
  resume_save();
  PT("resume_save");
  views_save_async();
  PT("views_save_async");
  screen = S_LIBRARY;
  sel = vk;
  if (sel >= 0 && sel < lgrid.n) {
    Rect r = lgrid.r[sel];
    if (r.y < scroll || r.y + r.h > scroll + H - TOP - BAR) scroll = r.y - 40;
  }
  book_back_to_all(); // (a book opened from All: back there, at its card)
}

static void fmt_time(uint64_t ms, char *o, size_t n) { int s = (int)(ms / 1000); snprintf(o, n, "%d:%02d", s / 60, s % 60); }

static int tall_fit; static float tall_my;
static int lv_por; static float lv_mx, lv_my, lv_mag = 1, lv_aw = W, lv_ah = H; // (how the full view shows this one: viewopt.c)
#include "reader.c"
#include "viewopt.c"
static void draw_viewer(void) {
  if (nshown == 0) { screen = S_LIBRARY; return; }
  if (lib_reading_comic()) { draw_lib_comic(); return; } // (a comic's page: the comic reader)
  tall_fit = 0;
  LvItem *it = &vault.items[shown[vk]];
  // the player opened on the other thread, for this picture
  if (vopen_ready && !vid) {
    pthread_mutex_lock(&cmx);
    if (vopen_ready && vopen_ready_gen == vopen_gen) {
      vid = vopen_ready; vid_clip = vopen_ready_clip; vopen_ready = NULL;
      vid_item = shown[vk];
      lv_video_set_volume(vid, cfg.volume * 32768 / 100);
      if (!vid_clip && !slideshow) { int t = resume_of(it); if (t > 0) resume_to = (int64_t)t * 1000; }
    }
    pthread_mutex_unlock(&cmx);
  }
  int backdrop = hero_backdrop(&vhero);
  if (backdrop < 255) vita2d_draw_rectangle(0, 0, W, H, BG);
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, backdrop));
  xfade_wanted = slideshow && so.fade;
  // the thumbnails wanted here: this one (its glow) and the filmstrip's
  {
    int want_t[16], nwt = 0;
    // (this one and the next and last always, to show at once on a swipe; the rest of the filmstrip's
    //  only while it can show: not in a slideshow, a video, or with the controls hidden)
    int strip = !slideshow && !(vid && !vid_clip) && frame_no < chrome_until;
    for (int d = 0; d < (strip ? 11 : 3) && nwt < 16; d++) {
      int k = vk + (d % 2 ? (d + 1) / 2 : -(d / 2));
      if (k >= 0 && k < nshown && !thumb_of(shown[k]) && tstate[shown[k]] != 3) want_t[nwt++] = shown[k];
    }
    if (nwt) ask_thumbs(want_t, nwt);
  }
  // --- the glow: the picture's own colours, dim, filling the screen around it ---
  vita2d_texture *th_now = thumb_of(shown[vk]);
  if (cfg.glow && th_now && zoom < 1.01f && backdrop == 255) draw_cover(th_now, 0, 0, W, H, RGBA(255, 255, 255, 60));
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
    if (age >= 36 || !slideshow) { release_tex(fade_tex); fade_tex = NULL; }
    else {
      fade = age / 36.0f;
      float fw = vita2d_texture_get_width(fade_tex), fh = vita2d_texture_get_height(fade_tex);
      float fs = fminf(W / fw, avail_h / fh) * (so.motion == 1 ? 1.08f : 1);
      vita2d_draw_texture_tint_scale(fade_tex, (W - fw * fs) / 2, (avail_h - fh * fs) / 2, fs, fs, RGBA(255, 255, 255, (int)(255 * (1 - fade))));
    }
  }
  if (t && is_frame && (lv_video_width(vid) < 1 || lv_video_height(vid) < 1)) t = th_now, is_frame = 0; // (no size yet)
  if (t && (vita2d_texture_get_width(t) < 1 || vita2d_texture_get_height(t) < 1)) t = NULL;
  // (how it's shown: Viewing's size and orientation, for a video or a picture)
  view_tall = it->height > it->width * 1.15f;
  int isv = is_video(it), por = view_portrait(isv);
  lv_por = por; lv_mx = lv_my = 0; lv_mag = 1;
  lv_aw = por ? avail_h : W; lv_ah = por ? W : avail_h;
  if (t) {
    float tw = is_frame ? lv_video_width(vid) : vita2d_texture_get_width(t);
    float th = is_frame ? lv_video_height(vid) : vita2d_texture_get_height(t);
    // (a tall page, fitted: the screen's width, scrolled down; the thumbnail, cropped, stays whole meanwhile)
    tall_fit = !por && view_mode(isv) == VF_FIT && !is_frame && t != th_now && th > tw * 2.2f;
    float sx, sy;
    if (tall_fit) sx = sy = W / tw;
    else lv_mag = view_fit(view_mode(isv), tw, th, is_frame ? tw : it->width, is_frame ? th : it->height, lv_aw, lv_ah, &sx, &sy);
    sx *= zoom * ms; sy *= zoom * ms;
    float s = sx;
    float dw = tw * sx, dh = th * sy;
    // keep a zoomed (or bigger than the screen) picture's edges on screen
    float mx = fmaxf(0, (dw - lv_aw) / 2), my = fmaxf(0, (dh - lv_ah) / 2);
    lv_mx = mx; lv_my = my;
    if (tall_fit && tall_top) { pan_y = my; tall_top = 0; }
    if (tall_fit) { pan_y += tall_v * ft; tall_v *= decay(0.93f); if (fabsf(tall_v) < 0.3f) tall_v = 0; tall_my = my; }
    pan_x = fminf(mx, fmaxf(-mx, pan_x));
    pan_y = fminf(my, fmaxf(-my, pan_y));
    float x = (W - dw) / 2 + pan_x + swipe_x + mdx, y = (avail_h - dh) / 2 + pan_y + mdy;
    if (por) view_draw(t, 1, tw, th, pan_x + swipe_x + mdx, pan_y + mdy, sx, sy, avail_h, RGBA(255, 255, 255, fade < 1 ? (int)(255 * fade) : 255));
    else if (vhero.drag > 0 || vhero.closing) {
      // (swiped down: smaller, following the finger; let go, into its card)
      Rect card;
      int has = lib_card_target(vk, &card);
      float hx = x, hy = y, hw = dw, hh = dh;
      float e = hero_place(&vhero, &hx, &hy, &hw, &hh, card, has);
      draw_in_rect(t, tw, th, hx, hy, hw, hh, e, RGBA(255, 255, 255, 255));
    } else {
      hero_place(&vhero, &x, &y, &dw, &dh, (Rect){0}, 0);
      (void)s;
      vita2d_draw_texture_tint_scale(t, x, y, sx, sy, RGBA(255, 255, 255, fade < 1 ? (int)(255 * fade) : 255));
    }
  } else {
    if (big_failed) text_center(W / 2, H / 2, DIM, 18, "This file can't be shown on the Vita");
    else spinner(W / 2, H / 2, 24);
  }
  if (vhero.closing || (!t && vhero.drag > 0)) {
    // (flying home: nothing else drawn or done; closed once it's there)
    if (hero_done(&vhero) || !t) { memset(&vhero, 0, sizeof vhero); close_viewer(); }
    return;
  }
  if (!it->path) text_center(W / 2, (int)avail_h - 30, ACC2, 16, "This file isn't on the memory card (only its thumbnail)");
  // (the full picture, or the video, still on its way: a small spinner in the corner)
  if (t && ((!vid && !is_video(it) && !is_gif(it) && big_item != shown[vk] && !big_failed) || ((is_video(it) || is_gif(it)) && !vid && !vopen_ready && vopen_fail_gen != vopen_gen))) spinner(W - 34, H - 34, 12);
  if (vid && !vid_clip && resume_to >= 0 && lv_video_duration(vid) > 0) {
    lv_video_seek(vid, resume_to);
    char a1[16], m[64];
    fmt_time((uint64_t)resume_to, a1, sizeof a1);
    snprintf(m, sizeof m, "Carrying on from %s", a1);
    say(m);
    resume_to = -1;
    resumed_at = frame_no ? frame_no : 1;
  }
  // (just carried on: a button to start from the beginning instead, for five seconds)
  if (vid && resumed_at && frame_no - resumed_at < 300) {
    const char *l = "\u21ba  From the start";
    int bw = text_w(15, l) + 32, bx = W / 2 - bw / 2, by = H - 130;
    glass(bx, by, bw, 40, 0);
    text_center(W / 2, by + 26, TEXT, 15, l);
    if (hit(bx, by, bw, 40)) { lv_video_seek(vid, 0); resumed_at = 0; sfx(SFX_SELECT); in.tap = 0; }
  } else resumed_at = 0;
  if (vid && !vid_clip && ab_a >= 0 && ab_b > ab_a && (int64_t)lv_video_pos(vid) >= ab_b) lv_video_seek(vid, ab_a);
  if (vid && vid_clip && is_video(it)) text_center(W / 2, 30, with_alpha(TEXT, 200), 15, "Preview clip: the Vita can't play this video's own file");

  // (zoomed in past 1.25x: the picture again at up to 1920 px, once; back to the screen's size with the next)
  if (zoom * lv_mag > 1.25f && big_item == shown[vk] && big_hi_item != shown[vk] && !vid) { make_room(16u << 20); big_hi = 1; big_hi_item = shown[vk]; ask_big(shown[vk]); big_hi = 0; }
  if (vo_open) { draw_view_options(); return; } // (Viewing: its panel over the picture)
  // --- touch: swipe for the next one, pinch to zoom, drag when zoomed, tap for the controls ---
  if (in.touching && details_y < 0.5f) {
    if (in.tcount >= 2) {
      static float pmx, pmy;
      view_pinch(lv_por, lv_aw, lv_ah, &zoom, &pan_x, &pan_y, &pinch_d0, &zoom0, &pmx, &pmy);
    } else {
      pinch_d0 = 0;
      if (zoom > 1.01f) { pan_x += view_dx(lv_por, in.drag_dx, in.drag_dy); pan_y += view_dy(lv_por, in.drag_dx, in.drag_dy); }
      else if (tall_fit && fabsf((float)(in.ty - in.down_y)) >= fabsf((float)(in.tx - in.down_x))) { pan_y += in.drag_dy; tall_v = 0; }
      else if (!tall_fit && view_drag(lv_por, zoom, lv_mx, lv_my, &pan_x, &pan_y, &swipe_x)) {}
      else if (hero_drag(&vhero, !slideshow && !tall_fit && fabsf(swipe_x) < 4 && details_y < 0.05f)) {}
      else if (fabsf((float)(in.tx - in.down_x)) > fabsf((float)(in.ty - in.down_y))) swipe_x += in.drag_dx;
      else if (in.ty - in.down_y < -60) { details = 1; }
    }
  } else {
    pinch_d0 = 0;
    if (hero_release(&vhero)) { sfx(SFX_BACK); return; }
    if (in.flicked && tall_fit && zoom < 1.01f && fabsf(in.fvy) > fabsf(in.fvx)) tall_v = in.fvy * 1.4f;
    if (swipe_x > 120) { swipe_x = 0; sfx(SFX_TICK); show_item(vk - 1); return; }
    if (swipe_x < -120) { swipe_x = 0; sfx(SFX_TICK); show_item(vk + 1); return; }
    swipe_x *= decay(0.7f);
    if (zoom < 1.01f) zoom = 1;
  }
  int64_t vseek = INT64_MIN;
  if (in.dbl && vid && !vid_clip && details_y < 0.5f && vctl_dbl_side(in.tap_x)) {
    // (a double tap at a side of a video: ten seconds back or on)
    int s = vctl_dbl_side(in.tap_x);
    vseek = (int64_t)lv_video_pos(vid) + s * 10000;
    vflash(s);
  }
  else if (in.dbl && cfg.dbltap && details_y < 0.5f) { sfx(SFX_DONE); char now[40]; now_iso(now, sizeof now); lv_liked(&vault, it, now); heart_at = frame_no ? frame_no : 1; }
  else if (in.dbl) { zoom = zoom > 1.5f ? 1 : 2.5f; float ox, oy; view_tap_off(lv_por, lv_aw, lv_ah, &ox, &oy); pan_x = ox * (zoom - 1); pan_y = oy * (zoom - 1); }
  else if (in.tap && details_y < 0.5f) {
    int chrome_on = frame_no < chrome_until || (vid && !vid_clip && lv_video_paused(vid));
    if (vid && !vid_clip && chrome_on && vctl_tap(vid, in.tap_x, in.tap_y, &vseek)) chrome_until = frame_no + 180;
    else if (frame_no < chrome_until && res_info_tap()) chrome_until = frame_no + 600; // (the controls stay while it's read)
    else if (frame_no < chrome_until && fv_back_hit()) { sfx(SFX_BACK); close_viewer(); return; }
    else if (frame_no < chrome_until && in.tap_x > W - 70 && in.tap_y < 60) { details = 1; }
    else if (frame_no < chrome_until && view_btn_hit(is_video(it), W - 140)) { view_options_open(is_video(it)); return; }
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
  if (in.pressed & SCE_CTRL_START) {
    // (a slideshow goes through its own list: from here, if this one's in it, else from its start)
    if (!slideshow && !show_pool) { int was = shown[vk], k = show_pool_on(was); if (!nshown) { show_pool = 0; refilter(); say("No pictures, GIFs or short clips to show"); return; } if (shown[k] != was) { show_item(k); } else vk = k; }
    sfx(SFX_SLIDESHOW); slideshow = !slideshow; slide_at = frame_no; say(slideshow ? "Slideshow" : "Slideshow stopped");
  }
  if ((in.pressed & SCE_CTRL_CROSS) && !details) {
    if (vid && !vid_clip) lv_video_pause(vid, !lv_video_paused(vid));
    else { zoom = zoom > 1.5f ? 1 : 2.5f; pan_x = pan_y = 0; }
  }
  if (vid && !vid_clip && !details) {
    // ←→ ten seconds; ↑ faster (2×, 4×, back); ↓ the A-B loop (A here, B here, off)
    if (in.pressed & SCE_CTRL_RIGHT) { vseek = (int64_t)lv_video_pos(vid) + 10000; vflash(1); chrome_until = frame_no + 120; }
    if (in.pressed & SCE_CTRL_LEFT) { vseek = (int64_t)lv_video_pos(vid) - 10000; vflash(-1); chrome_until = frame_no + 120; }
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
  } else if (!vid && !details && !tall_fit && zoom < 1.01f && (in.pressed & SCE_CTRL_UP)) {
    view_options_open(0); return; // (↑ on a picture: Viewing)
  } else if (zoom > 1.01f || (!tall_fit && (lv_mx > 1 || lv_my > 1))) {
    pan_x -= view_dx(lv_por, in.lx, in.ly) * 12; pan_y -= view_dy(lv_por, in.lx, in.ly) * 12;
  } else if (tall_fit && !details) {
    // a tall page: the stick scrolls, ↑↓ a screenful (↓ at its foot: the next page)
    pan_y -= in.ly * 16;
    if (in.pressed & SCE_CTRL_UP) tall_v = 22;
    if (in.pressed & SCE_CTRL_DOWN) { if (pan_y <= -tall_my + 2) { sfx(SFX_TICK); show_item(vk + 1); return; } tall_v = -22; }
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

  if (vseek != INT64_MIN && vid && !vid_clip) { lv_video_seek(vid, vseek < 0 ? 0 : vseek); chrome_until = frame_no + 150; }
  // --- the controls on top, fading away ---
  int chrome = (frame_no < chrome_until || details || (vid && !vid_clip && lv_video_paused(vid))) && vhero.drag <= 0;
  if (chrome) {
    int a = frame_no + 30 > chrome_until && !details ? (int)((chrome_until - frame_no) * 8) : 240;
    if (a > 240 || a < 0) a = 240;
    grad(0, 0, W, 64, RGBA(0, 0, 0, 255), 64 * a / 400, 0, 0);
    // back
    fv_back_draw(a);
    res_rel = 1; res_badge(it->width, it->height, a); res_rel = 0; // (its resolution's rank, against the vault's biggest)
    char pos[48];
    snprintf(pos, sizeof pos, "%d of %d", vk + 1, nshown);
    text_center(W / 2, 36, with_alpha(TEXT, a), 17, pos);
    // heart and details
    view_btn_draw(is_video(it), a, W - 136);
    { // (the heart: how many times, and a pop with a +1 rising when it's given another)
      static int last_n = -1, last_it = -1; static unsigned pop_at;
      int nl = lv_likes_of(&vault, it);
      if (last_it == vk && nl > last_n && last_n >= 0) pop_at = frame_no ? frame_no : 1;
      last_it = vk; last_n = nl;
      float pu = pop_at ? (frame_no - pop_at) / 30.0f : 1, sc = 1;
      if (pu < 1) sc = 1 + 0.6f * sinf(pu * 3.14159f) * (1 - pu); else pop_at = 0;
      heart(W - 104, 30, 8 * sc, nl ? with_alpha(ACC, a) : with_alpha(TEXT, a / 2));
      if (nl > 0) { char nb[12]; snprintf(nb, sizeof nb, "%d", nl); text(W - 92, 44, with_alpha(TEXT, a), 11, nb); }
      if (pu < 1) text_center(W - 104, (int)(16 - pu * 22), with_alpha(ACC, (int)(255 * (1 - pu))), 14, "+1");
    }
    circle(W - 38, 30, 13, with_alpha(TEXT, a));
    circle(W - 38, 30, 11, RGBA(0, 0, 0, a));
    text_center(W - 38, 36, with_alpha(TEXT, a), 15, "i");
    if (vid && !vid_clip) {
      vctl_draw(vid, a, -1);
      uint64_t d = lv_video_duration(vid);
      // the loop's ends, the speed, and what the buttons do
      const float bw = W - 80 - 92;
      if (d && ab_a >= 0) rrect(40 + bw * fminf(1, ab_a / (float)d) - 2, H - 36, 4, 18, 2, with_alpha(ACC2, a));
      if (d && ab_b >= 0) rrect(40 + bw * fminf(1, ab_b / (float)d) - 2, H - 36, 4, 18, 2, with_alpha(ACC2, a));
      char hint[96];
      snprintf(hint, sizeof hint, "%s\u2190\u2192 10 s  \u00b7  \u2191 speed  \u00b7  \u2193 A-B loop  \u00b7  L R next",
               lv_video_speed(vid) == 200 ? "2\u00d7  \u00b7  " : lv_video_speed(vid) == 400 ? "4\u00d7  \u00b7  " : "");
      text_center(W / 2, H - 52, with_alpha(DIM, a), 13, hint);
    }
  }

  if (vid && !vid_clip) vctl_flash_draw();
  // --- the filmstrip: the ones either side, to jump to (with the controls; not over a video's bar) ---
  if (chrome && !(vid && !vid_clip) && details_y < 0.05f && nshown > 1 && !slideshow) {
    int a = frame_no + 30 > chrome_until ? (int)((chrome_until - frame_no) * 8) : 240;
    if (a > 240 || a < 0) a = 240;
    grad(0, H - 90, W, 90, RGBA(0, 0, 0, 255), 0, 90 * a / 300, 0);
    // (the strip glides to the new one, the ones near the middle growing as they come to it, and the
    //  highlight gives a little bounce each time it lands on another)
    static float fs_pos = -100;
    static int fs_last = -1;
    static unsigned fs_changed;
    if (fs_pos < -50 || fabsf(fs_pos - vk) > 10) fs_pos = vk;
    if (fs_last != vk) { fs_last = vk; fs_changed = frame_no ? frame_no : 1; }
    fs_pos += (vk - fs_pos) * ease(0.2f);
    if (fabsf(fs_pos - vk) < 0.002f) fs_pos = vk;
    float pop = 0;
    if (fs_changed) {
      float u = (frame_no - fs_changed) / 24.0f;
      if (u >= 1) fs_changed = 0; else pop = sinf(u * 3.14159f) * (1 - u) * 0.12f;
    }
    // the highlight: in the middle, a rounded ring of the accent (a glow behind it), springing
    {
      float hs = 66 * (1 + pop), hx = W / 2.0f - hs / 2, hy = H - 14 - 66 / 2.0f - hs / 2;
      rrect(hx - 7, hy - 7, hs + 14, hs + 14, 16, with_alpha(ACC, a * 50 / 255));
      rrect(hx - 3, hy - 3, hs + 6, hs + 6, 13, with_alpha(ACC, a));
    }
    int c0 = (int)floorf(fs_pos + 0.5f);
    for (int d = -7; d <= 7; d++) {
      int k = c0 + d;
      if (k < 0 || k >= nshown) continue;
      float off = k - fs_pos, dist = fminf(1, fabsf(off));
      float cx = W / 2.0f + off * 70, sz = 66 - 8 * dist;
      if (cx < -40 || cx > W + 40) continue;
      if (k == vk) sz *= 1 + pop;
      float x = cx - sz / 2, y = H - 14 - 66 / 2.0f - sz / 2 - (1 - dist) * 2;
      vita2d_texture *ft = thumb_of(shown[k]);
      int ta = (int)(a * (1 - 0.3f * dist) * (1 - fminf(1, fabsf(off) / 8) * 0.6f));
      if (ft) draw_cover_round(ft, x, y, sz, sz, 9, RGBA(255, 255, 255, ta));
      else rrect(x, y, sz, sz, 9, with_alpha(SURF2, ta));
      if (is_locked(shown[k])) rrect(x, y, sz, sz, 9, RGBA(14, 10, 14, 220));
      if (k != vk && in.tap && in.tap_x >= x && in.tap_x < x + sz && in.tap_y >= y && in.tap_y < y + sz) { sfx(SFX_TICK); show_item(k); return; }
    }
  }

  // --- the details, pulled up from below (as in the phone's full view) ---
  float target = details ? 1 : 0;
  details_y += (target - details_y) * ease(0.25f);
  if (details_y > 0.01f) {
    float top = H - details_y * 300;
    rrect(0, top, W, 330, 20, with_alpha(SURF, 250));
    rrect(W / 2 - 24, top + 8, 48, 5, 3, with_alpha(TEXT, 60));
    int ii = shown[vk];
    text_fit(30, top + 46, TEXT, 22, it->name[0] ? it->name : it->file, W - 420);
    // editing, as the phone does: the stars, the level dial, the category, the tags, delete
    // (↑↓ choose, ←→ change, ✕ for tags and delete; or touch)
    enum { E_STARS, E_LEVEL, E_CAT, E_TAGS, E_DELETE, E_SIMILAR, E_N };
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
    rrect(W - 194, top + 96, 104, 28, 14, efocus == E_SIMILAR ? ACC : SURF2);
    text_center(W - 142, (int)top + 116, efocus == E_SIMILAR ? RGBA(20, 12, 16, 255) : TEXT, 14, "\u2248 Similar");
    int act = 0;
    if (hit(W - 194, (int)top + 96, 104, 28)) { efocus = E_SIMILAR; act = 1; }
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
        else if (efocus == E_SIMILAR) {
          // (the Library searched for what this one's most about)
          char q[160];
          similar_tags(it->tags, NULL, q, sizeof q);
          if (!q[0]) { sfx(SFX_ERROR); say("No tags here to go by"); }
          else {
            details = 0;
            close_viewer();
            // (either of them: "~a ~b")
            char *sp = strchr(q, ' ');
            if (sp) { *sp = 0; snprintf(query, sizeof query, "~%s ~%s", q, sp + 1); *sp = ' '; }
            else snprintf(query, sizeof query, "%s", q);
            refilter();
            sel = 0; scroll = 0;
            char m[200];
            snprintf(m, sizeof m, "Like this: %s", q);
            say(m);
            return;
          }
        }
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
// The conversion services' keys (yours: ux0:data/LWDV00001/convert_keys.txt, "<service>\t<key>" a line;
// never in the app or its sources). Used for offline copies made 480p elsewhere (web.c, conv_main).
static char conv_keys[CV_N][2048];
static int conv_loaded, conv_sel;
static void conv_keys_load(void) {
  conv_loaded = 1;
  FILE *f = fopen(DATA "/convert_keys.txt", "r");
  char ln[300];
  while (f && fgets(ln, sizeof ln, f)) {
    ln[strcspn(ln, "\r\n")] = 0;
    char *t = strchr(ln, '\t');
    if (!t) continue;
    int s = atoi(ln);
    if (s >= 0 && s < CV_N) snprintf(conv_keys[s], sizeof conv_keys[s], "%s", t + 1);
  }
  if (f) fclose(f);
}
static void conv_keys_save(void) {
  FILE *f = fopen(DATA "/convert_keys.txt", "w");
  for (int s = 0; f && s < CV_N; s++) if (conv_keys[s][0]) fprintf(f, "%d\t%s\n", s, conv_keys[s]);
  if (f) fclose(f);
}
static int conv_any_key(void) {
  if (!cfg.conv) return 0; // (off unless turned on in Settings)
  if (!conv_loaded) conv_keys_load();
  for (int s = 0; s < CV_N; s++) if (conv_keys[s][0]) return 1;
  return 0;
}
// ----- the live theme preview: a small Library, in the colours chosen -----
static void theme_preview(float x, float y) {
  const float w = 236, h = 150;
  rrect(x - 3, y - 3, w + 6, h + 6, 18, with_alpha(ACC, 110));
  rrect(x, y, w, h, 16, BG);
  rrect(x + 10, y + 10, 70, 16, 8, SURF2);   // the header's search pill
  circle(x + w - 18, y + 18, 6, ACC);        // its online light (in the accent, here)
  static const char *const M[] = {"All", "Pics", "Vids"};
  for (int k = 0; k < 3; k++) {
    float mx = x + 10 + k * 42;
    rrect(mx, y + 32, 38, 14, 7, k == 0 ? ACC : SURF);
    text_center(mx + 19, y + 43, k == 0 ? BG : DIM, 10, M[k]);
  }
  // six cards: the Library's own first thumbnails when they're loaded, else the theme's colours
  for (int k = 0; k < 6; k++) {
    float cx = x + 10 + (k % 3) * 73, cy = y + 52 + (k / 3) * 38, cw = 69, ch = 34;
    vita2d_texture *t = k < nshown && shown ? thumb_of(shown[k]) : NULL;
    if (t) draw_cover(t, cx, cy, cw, ch, RGBA(255, 255, 255, 255)); // (an atlas cell: draw_cover knows)
    else rrect(cx, cy, cw, ch, 6, with_alpha(k & 1 ? ACC2 : ACC, 60 + 20 * (k % 3)));
    if (k == 1) ring9(cx - 3, cy - 3, cw + 6, ch + 6 < 34 ? 34 : ch + 6, ACC);
  }
  rrect(x + w / 2 - 50, y + h - 22, 100, 16, 8, SURF2);   // the tab bar
  circle(x + w / 2 - 30, y + h - 14, 4, ACC);
  for (int k = 1; k < 4; k++) circle(x + w / 2 - 30 + k * 20, y + h - 14, 4, FAINT);
  text(x + 10, y + h + 18, FAINT, 12, "Preview");
}

// ----- your own accent: a colour wheel (hue round, saturation out) and a brightness slider -----
static int wheel_open;
static vita2d_texture *wheel_tex;
static void draw_wheel(void) {
  const int D = 220;
  if (!wheel_tex && (wheel_tex = tex_new(D, D))) {
    uint32_t *px = vita2d_texture_get_datap(wheel_tex);
    unsigned st = vita2d_texture_get_stride(wheel_tex) / 4;
    for (int y = 0; y < D; y++)
      for (int x = 0; x < D; x++) {
        float dx = x + .5f - D / 2.f, dy = y + .5f - D / 2.f, r = sqrtf(dx * dx + dy * dy) / (D / 2.f);
        float a = fminf(1, fmaxf(0, (1 - r) * D / 2.f)); // (a soft edge)
        unsigned c = hsv(atan2f(dy, dx) * 57.2958f, fminf(1, r), 1);
        px[y * st + x] = with_alpha(c, (int)(a * 255));
      }
    vita2d_texture_set_filters(wheel_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
  }
  text(30, 44, TEXT, 26, "Your colour"); // (the background: the frame's clear colour already)
  text(30 + text_w(26, "Your colour") + 16, 44, FAINT, 13, "Touch the wheel  \u00b7  \u2190\u2192 hue  \u00b7  \u2191\u2193 colourfulness  \u00b7  L R brightness  \u00b7  \u25cb done");
  const float cx = 250, cy = 290, R = D / 2.f;
  if (wheel_tex) vita2d_draw_texture_tint(wheel_tex, cx - R, cy - R, hsv(0, 0, cfg.acc_v / 100.f));
  // the brightness slider
  const float sx = 420, sy = 180, sw = 30, sh = 220;
  for (int k = 0; k < 22; k++) rrect(sx, sy + k * 10, sw, 11, k == 0 || k == 21 ? 6 : 0, hsv(cfg.acc_h, cfg.acc_s / 100.f, 1 - k / 21.f * 0.75f));
  float ky = sy + (1 - (cfg.acc_v - 25) / 75.f) * sh;
  rrect(sx - 6, ky - 4, sw + 12, 8, 4, TEXT);
  // the knob on the wheel
  float a = cfg.acc_h / 57.2958f, rr = cfg.acc_s / 100.f * R;
  circle(cx + cosf(a) * rr, cy + sinf(a) * rr, 11, TEXT);
  circle(cx + cosf(a) * rr, cy + sinf(a) * rr, 8, ACC);
  // the result, on the things it colours
  float px = 520, py = 160;
  rrect(px, py, 380, 260, 20, SURF);
  text(px + 24, py + 40, TEXT, 20, "How it looks");
  rrect(px + 24, py + 64, 150, 40, 20, ACC);
  text_center(px + 99, py + 90, BG, 16, "\u2665 Favorite");
  rrect(px + 190, py + 64, 150, 40, 20, SURF2);
  text_center(px + 265, py + 90, ACC, 16, "\u2193 Offline");
  grad(px + 24, py + 124, 332, 60, ACC, 200, 0, 1);
  grad(px + 24, py + 124, 332, 60, ACC2, 0, 200, 1);
  rrect(px + 24, py + 204, 332, 8, 4, SURF2);
  rrect(px + 24, py + 204, 200, 8, 4, ACC);
  textf(px + 24, py + 244, FAINT, 13, "Hue %d\u00b0  \u00b7  colour %d%%  \u00b7  brightness %d%%", cfg.acc_h, cfg.acc_s, cfg.acc_v);
  // touch: on the wheel, the hue and colourfulness; on the slider, the brightness
  int ch = 0;
  if (in.touching) {
    float tx = in.tx, ty = in.ty, dx = tx - cx, dy = ty - cy, dist = sqrtf(dx * dx + dy * dy);
    if (in.down_x < sx - 20 && dist < R + 30) {
      cfg.acc_h = ((int)(atan2f(dy, dx) * 57.2958f) + 360) % 360;
      cfg.acc_s = (int)fminf(100, dist / R * 100);
      ch = 1;
    } else if (in.down_x >= sx - 20 && in.down_x < sx + sw + 30 && in.down_y > sy - 30 && in.down_y < sy + sh + 30) {
      cfg.acc_v = 25 + (int)(75 * (1 - fminf(1, fmaxf(0, (ty - sy) / sh))));
      ch = 1;
    }
  }
  unsigned held = in.now;
  if (held & SCE_CTRL_RIGHT) { cfg.acc_h = (cfg.acc_h + 2) % 360; ch = 1; }
  if (held & SCE_CTRL_LEFT) { cfg.acc_h = (cfg.acc_h + 358) % 360; ch = 1; }
  if (held & SCE_CTRL_UP) { cfg.acc_s = cfg.acc_s < 100 ? cfg.acc_s + 1 : 100; ch = 1; }
  if (held & SCE_CTRL_DOWN) { cfg.acc_s = cfg.acc_s > 0 ? cfg.acc_s - 1 : 0; ch = 1; }
  if (held & SCE_CTRL_RTRIGGER) { cfg.acc_v = cfg.acc_v < 100 ? cfg.acc_v + 1 : 100; ch = 1; }
  if (held & SCE_CTRL_LTRIGGER) { cfg.acc_v = cfg.acc_v > 25 ? cfg.acc_v - 1 : 25; ch = 1; }
  if (ch) apply_theme();
  if (in.pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS)) {
    sfx(SFX_CLOSE); wheel_open = 0; cfg_save();
    if (wheel_tex) { free_later(wheel_tex); wheel_tex = NULL; } // (its 190 KB of video memory back; made again if it's opened again)
  }
}

static int rw_open;
static void draw_rear_window(void);
static void rear_window_open(void);
static int rear_of(int right);
static const char *const REAR_SHORT[] = {"", "Both", "Scroll", "Swipe", "Volume", "Zoom", "Off"};
static int set_slide; // (a row's slide as Settings opens)
static float sb_snap = -1; // (the scroll bar let go: the section start it glides to)
static void draw_settings(void) {
  if (!conv_loaded) conv_keys_load();
  char vconv[80];
  if (!cfg.conv) snprintf(vconv, sizeof vconv, "Off");
  else snprintf(vconv, sizeof vconv, "%s: %s", lv_conv_name(conv_sel), conv_keys[conv_sel][0] ? "key set" : "no key");
  char vtheme[48];
  snprintf(vtheme, sizeof vtheme, "%s", cfg.theme == NTHEMES + 1 ? "Custom (\u2715 colour)" : cfg.theme > 0 ? THEMES[cfg.theme - 1].name : "The vault's own");
  if (cfg.theme > 0) vtheme[0] = (char)toupper((unsigned char)vtheme[0]);
  if (rw_open) { draw_rear_window(); return; } // (Rear touchpad: its window)
  if (wheel_open) { draw_wheel(); return; }
  if (ro_open) { draw_reading_options(); return; } // (Reading comics: the reader's own panel)
  if (vo_open) { draw_view_options(); return; } // (Viewing: the full views' own panel)
  char vtext[16];
  snprintf(vtext, sizeof vtext, "%d%%", text_pct);
  char vstore[48];
  if (store_state == 0) store_measure();
  if (store_state != 2) snprintf(vstore, sizeof vstore, "Measuring\u2026");
  else { double mb = (store_vault + store_data) / 1048576.0; if (mb >= 1024) snprintf(vstore, sizeof vstore, "%.2f GB", mb / 1024); else snprintf(vstore, sizeof vstore, "%.0f MB", mb); }
  char vrear[64];
  snprintf(vrear, sizeof vrear, "L: %s \u00b7 R: %s", REAR_SHORT[rear_of(0)], REAR_SHORT[rear_of(1)]);
  char vview[48];
  snprintf(vview, sizeof vview, "%s · %s", VF_SHORT[cfg.ifit], VF_SHORT[cfg.vfit]);
  char v3[64];
  snprintf(v3, sizeof v3, cfg.volume > 100 ? "%d%% (louder)" : "%d%%", cfg.volume);
  char v4[16];
  if (cfg.sounds) snprintf(v4, sizeof v4, "%d%%", cfg.sounds); else snprintf(v4, sizeof v4, "Off");
  char vd[96] = "Off";
  if (pin.decoy_hash[0] && !decoy_session) { const char *nm = strrchr(pin.decoy_vault, '/'); snprintf(vd, sizeof vd, "→ %s", pin.decoy_vault[0] ? (nm ? nm + 1 : pin.decoy_vault) : "the vault list"); }
  enum { ST_SEARCH, ST_VOL, ST_SOUNDS, ST_DBL, ST_GLOW, ST_DRIVE, ST_WIFI, ST_PIN, ST_DECOY, ST_COVER, ST_TLOCK, ST_STATS, ST_CLEAN, ST_HEALTH, ST_THEME, ST_BGS, ST_SNDPACK, ST_CONV, ST_VAULT, ST_CACHE, ST_COMICS, ST_VIEW, ST_TEXT, ST_ACCPIC, ST_DUPES, ST_REAR, ST_AUTOROT, ST_DECOR, ST_OTAGS, ST_STORAGE, ST_SAVEQ, ST_N };
  struct { const char *name, *value, *hint; } rows[ST_N] = {
    {"Search", query[0] ? query : "", "Tags, names, categories and collections"},
    {"Video volume", v3, "← → to change; past 100% it's made louder than the video itself"},
    {"Interface sounds", v4, "The Vita's own clicks and chimes"},
    {"Double tap in the full view", cfg.dbltap ? "Heart" : "Zoom", "Zooms in, or gives it a heart"},
    {"Glow behind pictures", cfg.glow ? "On" : "Off", "The picture's own colours fill the screen around it"},
    {"Google Drive", drive_signed_in() ? "Signed in" : "", "Keep this vault in step with your PC's, both ways"},
    {"Wi-Fi transfer", "", "Send pictures and videos from a phone or PC on the same Wi-Fi"},
    {"Lock with a PIN", !pin.on || decoy_session ? "Off" : pin.on_wake ? "On, and when it wakes" : "On, when it starts", pin.on ? "✕ a new PIN · ←→ when · □ turns it off" : "A PIN pad before anything shows"},
    {"Decoy PIN", vd, pin.on ? "Another PIN that opens a harmless vault (←→ which, □ off)" : "Needs the PIN lock on"},
    {"SELECT hides the app behind", cfg.hide ? "A calculator" : "A clock", cfg.hide ? "It calculates; SELECT again gets back (through the PIN pad, if there's a PIN), or your PIN and =" : "A plain clock; SELECT again comes back"},
    {"Touch lock", TL_NAMES[cfg.tlock & 3], "Hold L and R for a second: touch and buttons do nothing (again to unlock)"},
    {"Your stats", "", "What's here and how it's been looked at"},
    {"Clean up", "", "One at a time: swipe away what you don't want any more"},
    {"Health check", "", "What's wrong in the vault, and what can be put right"},
    {"Theme", vtheme, "The accent colour (\u2190\u2192): the vault's own, ten more, or your own"},
    {"Background", BGS[cfg.bgstyle].name, "\u2190\u2192: dark, black (OLED), midnight or plum"},
    {"Sound pack", SND_NAMES[cfg.sndpack], "The interface sounds (\u2190\u2192, a sound plays to hear them)"},
    {"Video conversion", vconv, cfg.conv ? "Big booru videos made 480p by a service for offline (your key; it sees the address) \u00b7 \u2190\u2192 Off or which \u00b7 \u2715 key \u00b7 \u25a1 remove" : "Off: \u2190\u2192 to turn it on (big booru videos made 480p by a service, for offline)"},
    {"Open another vault", "", "The vaults on the memory card"},
    {"Clear the thumbnail cache", "", "They're made again as you scroll"},
    {"Reading comics", cfg.cmode ? "Strip" : "Pages", "Pages or a strip, portrait, page turns, direction, taps, the rear touchpad"},
    {"Viewing pictures and videos", vview, "Fit, fill, original size or fit width; landscape or portrait (in the full view: ↑ or the size button)"},
    {"Text size", vtext, "\u2190\u2192: smaller or bigger, everywhere"},
    {"Accent from the picture", cfg.accpic ? "On" : "Off", "The accent colour takes the picture's own, as you look through them"},
    {"Find duplicates", "", "The same picture more than once: one kept, the rest to the vault's trash"},
    {"Rear touchpad", vrear, "What each half of the back does: scroll, swipe, volume, zoom or nothing (\u2715 to choose)"},
    {"Turn portrait pictures", cfg.autorot ? "On" : "Off", "A tall picture turned to fill the screen, for the Vita held on its side"},
    {"Decorations", cfg.nodecor ? "Off" : "On", "The little anime girls by empty screens and in corners, in the accent's colour (tap one)"},
    {"Your tags when saving", cfg.notags ? "Off" : "On", "After a save: pick tags you've given before or type new ones (yours come first)"},
    {"Save quality", cfg.wfull ? "Original" : "Vita-sized", "Saves kept as the original file, or made Vita-sized (smaller, quicker)"},
    {"Storage used", vstore, "What the app takes on the card \u00b7 \u2715 the Library, largest files first"},
  };
  // (which rows have been changed from how the app starts: their value in the accent, a touch bolder;
  //  the others' in grey, just saying what it is)
  int changed[ST_N] = {0};
  changed[ST_SEARCH] = query[0] != 0; changed[ST_VOL] = cfg.volume != 100; changed[ST_SOUNDS] = cfg.sounds != 70;
  changed[ST_DBL] = cfg.dbltap != 0; changed[ST_GLOW] = cfg.glow != 1; changed[ST_DRIVE] = drive_signed_in();
  changed[ST_PIN] = pin.on && !decoy_session; changed[ST_DECOY] = pin.decoy_hash[0] && !decoy_session; changed[ST_COVER] = cfg.hide != 0;
  changed[ST_TLOCK] = (cfg.tlock & 3) != 0; changed[ST_THEME] = cfg.theme != 0; changed[ST_BGS] = cfg.bgstyle != 0;
  changed[ST_SNDPACK] = cfg.sndpack != 0; changed[ST_CONV] = cfg.conv != 0;
  changed[ST_COMICS] = cfg.cmode || cfg.corient || cfg.canim || cfg.cdir || cfg.ctap || cfg.cdbl || cfg.cback;
  changed[ST_VIEW] = cfg.ifit || cfg.iorient || cfg.vfit || cfg.vorient; changed[ST_TEXT] = text_pct != 100;
  changed[ST_ACCPIC] = cfg.accpic != 0; changed[ST_REAR] = cfg.rear_l || cfg.rear_r; changed[ST_AUTOROT] = cfg.autorot != 0;
  changed[ST_DECOR] = cfg.nodecor != 0; changed[ST_OTAGS] = cfg.notags != 0; changed[ST_SAVEQ] = cfg.wfull != 0;
  // In sections, each with a header: the rows in the order shown (set_sel stays a row's ST_ id)
  static const signed char ORDER[] = {
    -1, ST_THEME, ST_ACCPIC, ST_BGS, ST_DECOR, ST_TEXT, ST_SNDPACK, ST_SOUNDS, ST_GLOW, ST_DBL,
    -2, ST_VIEW, ST_AUTOROT, ST_VOL, ST_REAR,
    -6, ST_COMICS,
    -3, ST_PIN, ST_DECOY, ST_COVER, ST_TLOCK,
    -4, ST_DRIVE, ST_WIFI, ST_CONV,
    -5, ST_SEARCH, ST_OTAGS, ST_SAVEQ, ST_STORAGE, ST_STATS, ST_CLEAN, ST_DUPES, ST_HEALTH, ST_VAULT, ST_CACHE,
  };
  static int set_init;
  if (!set_init) { set_init = 1; set_sel = ST_THEME; } // (the first row shown)
  static const char *const SECT[] = {"Appearance", "Playback", "Privacy", "Sync and sources", "Library", "Comics"};
  static const char *const SECT_ICON[] = {"sliders", "play", "info", "wifi", "photos", "book"};
  enum { NORD = sizeof ORDER / sizeof *ORDER };
  const int rh = 58, hh = 40, top = 112, room = H - BAR - top + 8;
  float ypos[NORD], total = 0;
  int at = 0;
  for (int k = 0; k < NORD; k++) { ypos[k] = total; total += ORDER[k] < 0 ? hh : rh; if (ORDER[k] == set_sel) at = k; }
  // (the chosen row kept in sight, its section's header too when it's the first)
  // (only while going by the D-pad: a touch or the stick scrolls freely, the choice stays where it is)
  static int set_follow = 1;
  float want = ypos[at] - (at > 0 && ORDER[at - 1] < 0 ? hh : 0);
  if (set_follow && ypos[at] + rh - set_scroll > room) set_scroll = ypos[at] + rh - room;
  if (set_follow && want < set_scroll) set_scroll = want;
  float fy = -1;
  // (the preview has a column of its own at the right while Theme or Background is chosen: the rows
  //  narrow smoothly to make room, so it never covers their values)
  static float rw_now = 0;
  int pv = 0; // (no preview column: the rows keep their width, and the screen stays light)
  float rw_want = pv ? W - 60 - 262 : W - 60;
  if (rw_now < 1) rw_now = rw_want;
  rw_now += (rw_want - rw_now) * ease(0.3f);
  if (fabsf(rw_want - rw_now) < 0.5f) rw_now = rw_want;
  const int RW = (int)rw_now - 12; // (room for the scroll bar)
  for (int k = 0; k < NORD; k++) {
    int y = top + (int)ypos[k] - (int)set_scroll;
    if (y + rh < top - 10 || y > H - BAR) continue;
    if (ORDER[k] < 0) {
      int sct = -ORDER[k] - 1;
      icon(SECT_ICON[sct], 44, y + hh - 16, ACC);
      text(62, y + hh - 9, DIM, 15, SECT[sct]);
      vita2d_draw_rectangle(62 + text_w(15, SECT[sct]) + 12, y + hh - 15, 30 + RW - (74 + text_w(15, SECT[sct])), 1, with_alpha(FAINT, 90));
      continue;
    }
    int r = ORDER[k];
    // (opening Settings: the rows slide in from the right, one after another)
    { static unsigned seen_at, opened; if (frame_no - seen_at > 2) opened = frame_no; seen_at = frame_no;
      float u = fminf(1, fmaxf(0, (frame_no - opened - k * 1.2f) / 14.0f)); u = 1 - (1 - u) * (1 - u) * (1 - u);
      (void)u; set_slide = 0; } // (no slide on opening: they start where they belong)
    if (r == set_sel) glass(30 + set_slide, y, RW, 54, 0);
    else rrect(30 + set_slide, y, RW, 54, 14, SURF);
    if (r == set_sel) fy = y;
    int vw = rows[r].value[0] ? text_w(18, rows[r].value) + 24 : 0;
    text_fit(54 + set_slide, y + 24, TEXT, 18, rows[r].name, RW - 48 - (vw ? 60 : 0));
    text_fit(54 + set_slide, y + 45, FAINT, 14, rows[r].hint, RW - 48 - vw); // (cut short of the value)
    if (rows[r].value[0]) {
      int vx = 30 + set_slide + RW - 24 - text_w(18, rows[r].value);
      if (changed[r]) { unsigned vc = mix_col(TEXT, ACC, 0.45f); text(vx + 1, y + 34, with_alpha(vc, 110), 18, rows[r].value); text(vx, y + 34, vc, 18, rows[r].value); }
      else text(vx, y + 34, DIM, 18, rows[r].value);
    }
    if (hit(30, y, RW, 54) && in.tap_y > top && in.tap_y < H - BAR && in.down_x <= W - 42) { set_sel = r; in.pressed |= SCE_CTRL_CROSS; }
  }
  if (fy >= 0) focus_ring(30, fy, RW, 54);
  vita2d_draw_rectangle(0, 0, W, top - 4, BG);
  { // (the title as a logo, filling its row beside the girl: a soft drop in the accent's shade, the
    //  word, a heart for the i's dot, a stroke under it and the count beneath)
    const int ts = 52, ty = 66;
    unsigned shade = mix_col(with_alpha(BG, 255), ACC, 0.45f);
    text(33, ty + 3, shade, ts, "Settings");
    text(31, ty, TEXT, ts, "Settings");
    text(30, ty, TEXT, ts, "Settings");
    float ix = 30 + (text_w(ts, "Sett") + text_w(ts, "Setti")) / 2.0f;
    float beat = 1 + 0.08f * fmaxf(0, sinf(frame_no * 0.12f)) * (frame_no % 150 < 26); // (a beat now and then)
    rrect(ix - 9, ty - 48, 19, 19, 2, BG); // (the i's own dot hidden)
    heart(ix + 1, ty - 35, 15 * beat, ACC);
    rrect(30, ty + 13, text_w(ts, "Settings") * 0.55f, 5, 2, with_alpha(ACC, 200)); // (a stroke under it)
    textf(31, ty + 36, FAINT, 14, "%d items%s", vault.n, vault.untraceable ? "  ·  Untraceable" : "");
  }
  // (the girl with the peace sign, all of her in the title row at the right, her animation playing)
  vita2d_set_clip_rectangle(0, 0, W, top - 4);
  vita2d_enable_clipping();
  decor_draw(DEC_PEACE, W - 104, top - 4, 106, 235);
  vita2d_disable_clipping();
  // (a scroll bar beside the rows, the sections' starts marked on it. A finger on it drags it, a light
  //  pull onto each section's mark as it passes, the section's name in a bubble beside it; let go, it
  //  settles on the nearest section)
  static int sb_drag, sb_last_sec = -1;
  if (total > room) {
    const float tw2 = 25, x = W - 33, bt = top + 20, bh2 = H - 28 - bt; // (as long as it can be: the title row to the screen's foot, beside the tab bar; fixed whether that's tucked or not)
    float span = total - room;
    if (in.touching && !sb_drag && in.down_x > W - 42 && in.down_y > top && frame_no - in.down_frame < 2) { sb_drag = 1; sb_last_sec = -1; }
    int sec_k = -1; // (the section the bar is in)
    if (sb_drag && in.touching) {
      float f = fminf(fmaxf((in.ty - bt - 15) / (bh2 - 30), 0), 1), want = f * span;
      { float pull = -1, pd = 7; // (the pull: onto the nearest mark within 7 px, only that one)
        for (int k = 0; k < NORD; k++) if (ORDER[k] < 0) { float t = fminf(ypos[k], span), d = fabsf((t - want) / span * (bh2 - 30)); if (d < pd) { pd = d; pull = t; } }
        if (pull >= 0) want = pull; }
      set_scroll = want;
    }
    if (sb_drag && !in.touching) {
      sb_drag = 0;
      float best = 0, bd = 1e9f;
      for (int k = 0; k < NORD; k++) if (ORDER[k] < 0) { float t = fminf(ypos[k], span), d = fabsf(t - set_scroll); if (d < bd) { bd = d; best = t; } }
      sb_snap = best; sfx(SFX_TICK);
    }
    for (int k = 0; k < NORD; k++) if (ORDER[k] < 0 && fminf(ypos[k], span) <= set_scroll + 1) sec_k = k;
    if (sb_drag && sec_k != sb_last_sec) { if (sb_last_sec >= 0) sfx(SFX_TICK); sb_last_sec = sec_k; }
    // the track: a dark rounded tube, lit faintly down its middle (a cylinder: shaded side to side)
    {
      unsigned dk = with_alpha(mix_col(with_alpha(BG, 255), RGBA(0, 0, 0, 255), 0.35f), 255);
      rrect(x - 1, bt - 21, tw2 + 2, bh2 + 42, 13, RGBA(0, 0, 0, 140));
      rrect(x, bt - 20, tw2, bh2 + 40, 12, mix_col(dk, TEXT, 0.10f));
      for (int j = 1; j <= 4; j++) rrect(x + 1 + j * 2, bt - 19 + j, tw2 - 2 - j * 4, bh2 + 38 - j * 2, 11 - j, RGBA(255, 255, 255, 7));
      rrect(x + 1, bt - 19, tw2 - 2, 10, 9, RGBA(0, 0, 0, 60)); // (shade at its top: it's hollow)
      { float cx = x + tw2 / 2, cy = bt - 13; for (int r = 0; r < 5; r++) vita2d_draw_rectangle(cx - 0.5f - r, cy + r, 1 + r * 2, 1, with_alpha(TEXT, 130)); } // (▲)
      { float cx = x + tw2 / 2, cy = bt + bh2 + 9; for (int r = 0; r < 5; r++) vita2d_draw_rectangle(cx - 4.5f + r, cy + r, 9 - r * 2, 1, with_alpha(TEXT, 130)); } // (▼)
    }
    // the handle: short, a cylinder of the accent (darker at its sides, lit down the middle)
    float th2 = 30, ty2 = bt + (bh2 - th2) * (set_scroll / span);
    float g = sb_drag ? 2 : 0, hx = x + 3 - g, hw = tw2 - 6 + g * 2;
    rrect(hx, ty2 + 2, hw, th2, 9, RGBA(0, 0, 0, 100));
    rrect(hx, ty2, hw, th2, 9, mix_col(ACC, RGBA(0, 0, 0, 255), 0.35f));
    for (int j = 1; j <= 4; j++) rrect(hx + j * 1.5f, ty2 + 1, hw - j * 3, th2 - 2, 9 - j, with_alpha(mix_col(ACC, RGBA(0, 0, 0, 255), fmaxf(0, 0.35f - j * 0.085f)), 255));
    rrect(hx + hw * 0.36f, ty2 + 3, hw * 0.16f, th2 - 6, 2, RGBA(255, 255, 255, 70)); // (the light along it)
    for (int r = -1; r <= 1; r++) vita2d_draw_rectangle(hx + 5, ty2 + th2 / 2 + r * 4, hw - 10, 1, with_alpha(mix_col(ACC, RGBA(0, 0, 0, 255), 0.5f), 200)); // (ridges)
    // the section's name in a see-through bubble beside it, while dragged
    if (sb_drag && sec_k >= 0) {
      const char *nm = SECT[-ORDER[sec_k] - 1];
      int bw2 = text_w(15, nm) + 28, bx2 = (int)x - bw2 - 10, by2 = (int)(ty2 + th2 / 2 - 17);
      rrect(bx2, by2, bw2, 34, 12, RGBA(0, 0, 0, 160));
      for (int r = 0; r < 6; r++) vita2d_draw_rectangle(bx2 + bw2, by2 + 11 + r, 6 - r, 1, RGBA(0, 0, 0, 160)); // (its point)
      text(bx2 + 14, by2 + 23, TEXT, 15, nm);
    }
  }
  // touch and the left stick scroll, whatever row they're over; a flick keeps going
  static float set_v;
  if (sb_drag || sb_snap >= 0) set_follow = 0;
  if (sb_snap >= 0) { set_scroll += (sb_snap - set_scroll) * ease(0.25f); if (fabsf(sb_snap - set_scroll) < 0.5f) { set_scroll = sb_snap; sb_snap = -1; } }
  if (in.touching && in.down_y > top && in.down_x <= W - 42) { set_scroll -= in.drag_dy; set_v = 0; sb_snap = -1; if (in.moved) set_follow = 0; }
  if (in.flicked && in.down_y > top && in.down_x <= W - 42) { set_v = -in.fvy * 1.4f; set_follow = 0; }
  if (fabsf(in.ly) > 0.05f) { set_scroll += in.ly * 14 * ft; set_follow = 0; }
  set_scroll += set_v * ft;
  set_v *= decay(0.93f);
  { static float last_ss; chrome_follow(set_scroll - last_ss, set_scroll); last_ss = set_scroll; } // (the tab bar tucks as it scrolls down)
  if (set_scroll < 0) { set_scroll = 0; set_v = 0; }
  if (set_scroll > total - room) { set_scroll = total - room > 0 ? total - room : 0; set_v = 0; }
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) {
    int step = (in.pressed & SCE_CTRL_DOWN) ? 1 : -1, k = at;
    if (!set_follow) {
      // (scrolled away: the D-pad picks up from the first row in sight)
      k = -1;
      for (int j = 0; j < NORD && k < 0; j++) if (ORDER[j] >= 0 && ypos[j] >= set_scroll - 4) k = j;
      if (k < 0) k = at;
      set_sel = ORDER[k];
    } else {
      do k = (k + step + NORD) % NORD; while (ORDER[k] < 0);
      set_sel = ORDER[k];
    }
    set_follow = 1;
  }
  if (in.tap && in.tap_y > top) set_follow = 0; // (a row tapped: it's where it is, no jump)
  // the theme (or background) chosen: a little Library, in its colours, to see it before leaving
  int d = (in.pressed & SCE_CTRL_RIGHT) ? 1 : (in.pressed & SCE_CTRL_LEFT) ? -1 : 0;
  int cross = (in.pressed & SCE_CTRL_CROSS) != 0, square = (in.pressed & SCE_CTRL_SQUARE) != 0;
  if (cross) d = d ? d : 1;
  if (decoy_session && (set_sel == ST_PIN || set_sel == ST_DECOY) && (d || square || cross)) { sfx(SFX_ERROR); say("Not available"); d = square = cross = 0; }
  if (square && set_sel == ST_PIN && pin.on) { sfx(SFX_CLOSE); pin.on = 0; pin.decoy_hash[0] = 0; pin_save(); say("PIN lock off"); }
  if (square && set_sel == ST_CONV && conv_keys[conv_sel][0]) { conv_keys[conv_sel][0] = 0; conv_keys_save(); sfx(SFX_CLOSE); say("Key removed"); }
  if (square && set_sel == ST_DECOY && pin.decoy_hash[0]) { sfx(SFX_CLOSE); pin.decoy_hash[0] = 0; pin_save(); say("Decoy PIN off"); }
  if (d) {
    switch (set_sel) {
      case ST_SEARCH: if (cross && !ime_open) ime_start(2, "Search tags, names, categories", query, 0); break;
      case ST_VOL: cfg.volume += d * 10; if (cfg.volume > 150) cfg.volume = 150; if (cfg.volume < 0) cfg.volume = 0; break;
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
      case ST_STATS: if (cross) { sfx(SFX_OPEN); stats_back = S_SETTINGS; screen = S_STATS; return; } break;
      case ST_WIFI: if (cross) { sfx(SFX_OPEN); wf_return = S_SETTINGS; wf_last[0] = 0; screen = S_WIFI; return; } break;
      case ST_VAULT: if (cross) { views_save_async(); resume_save(); find_vaults(); screen = S_PICK; return; } break;
      case ST_THEME:
        if (cross && cfg.theme == NTHEMES + 1) { sfx(SFX_OPEN); wheel_open = 1; break; }
        cfg.theme = (cfg.theme + d + NTHEMES + 2) % (NTHEMES + 2); apply_theme(); cfg_save(); sfx(SFX_TOGGLE); break;
      case ST_BGS: cfg.bgstyle = (cfg.bgstyle + d + 4) % 4; apply_theme(); cfg_save(); sfx(SFX_TOGGLE); break;
      case ST_SNDPACK: cfg.sndpack = (cfg.sndpack + d + 4) % 4; sfx_pack(SND_PACKS[cfg.sndpack]); cfg_save(); sfx(SFX_SAVED); break;
      case ST_CONV:
        // (←→ go round Off and the services; ✕ on a service asks for its key)
        if (cross && cfg.conv && !ime_open) { char t[120]; snprintf(t, sizeof t, "Your %s API key (from %s)", lv_conv_name(conv_sel), lv_conv_site(conv_sel)); ime_start(17, t, "", 0); }
        else {
          int at = cfg.conv ? conv_sel + 1 : 0;
          at = (at + d + CV_N + 1) % (CV_N + 1);
          cfg.conv = at > 0;
          if (at) conv_sel = at - 1;
          cfg_save();
          sfx(SFX_TOGGLE);
        }
        break;
      case ST_COMICS: if (cross || d) { sfx(SFX_OPEN); ro_open = 1; ro_sel = 0; } break;
      case ST_VIEW: if (cross || d) { view_options_open(0); } break;
      case ST_TEXT: {
        static const int P[] = {90, 100, 112, 125};
        int at = 1;
        for (int k = 0; k < 4; k++) if (P[k] == text_pct) at = k;
        if (!d && cross) d = 1;
        at = (at + d + 4) % 4;
        cfg.tscale = text_pct = P[at];
        cfg_save(); sfx(SFX_TOGGLE);
        break;
      }
      case ST_DUPES: if (cross) { sfx(SFX_OPEN); screen = S_DUPES; return; } break;
      case ST_REAR: if (cross || d) { rear_window_open(); return; } break;
      case ST_AUTOROT: if (cross || d) { cfg.autorot = !cfg.autorot; sfx(SFX_TOGGLE); cfg_save(); } break;
      case ST_DECOR: if (cross || d) { cfg.nodecor = !cfg.nodecor; sfx(SFX_TOGGLE); cfg_save(); } break;
      case ST_OTAGS: if (cross || d) { cfg.notags = !cfg.notags; sfx(SFX_TOGGLE); cfg_save(); } break;
      case ST_SAVEQ: if (cross || d) { cfg.wfull = !cfg.wfull; sfx(SFX_TOGGLE); cfg_save(); } break;
      case ST_STORAGE: if (cross) { // (the Library, its largest files first; measured again for it)
        sfx(SFX_OPEN); store_state = 0; store_measure(); cfg.sort = 10; cfg_save(); refilter(); screen = S_LIBRARY; say("Largest files first");
      } break;
      case ST_ACCPIC: if (cross || d) { cfg.accpic = !cfg.accpic; if (!cfg.accpic) apply_theme(); cfg_save(); sfx(SFX_TOGGLE); } break;
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

// ----- the first start: three cards (the vault, the gestures, Wi-Fi), then the vault list -----
// Lines of text, broken at spaces to fit w; returns the y after the last
static int text_wrap(int x, int y, unsigned c, int size, const char *s, int w, int lh) {
  char line[256];
  int n = 0;
  const char *p = s;
  while (*p) {
    const char *e = p;
    while (*e && *e != ' ' && *e != '\n') e++;
    int wl = (int)(e - p);
    char trial[256];
    snprintf(trial, sizeof trial, "%.*s%s%.*s", n, line, n ? " " : "", wl, p);
    if (n && text_w(size, trial) > w) { line[n] = 0; text(x, y, c, size, line); y += lh; n = 0; snprintf(trial, sizeof trial, "%.*s", wl, p); }
    n = snprintf(line, sizeof line, "%s", trial);
    if (n >= (int)sizeof line) n = sizeof line - 1;
    if (*e == '\n') { line[n] = 0; text(x, y, c, size, line); y += lh; n = 0; }
    p = *e ? e + 1 : e;
  }
  if (n) { line[n] = 0; text(x, y, c, size, line); y += lh; }
  return y;
}
// the three cards' pictures, drawn (no image files): k the card, in the panel at (x, y, w, h)
static void onboard_art(int k, float x, float y, float w, float h) {
  float cx = x + w / 2, cy = y + h / 2, t = frame_no * 0.03f;
  // the panel: the accent's two colours, soft circles floating in it
  rrect(x, y, w, h, 22, with_alpha(ACC, 255));
  grad(x, y, w, h, ACC2, 0, 230, 1);
  grad(x, y + h * 0.4f, w, h * 0.6f, RGBA(0, 0, 0, 255), 0, 90, 0);
  circle(x + w * 0.18f + sinf(t) * 6, y + h * 0.22f, 46, RGBA(255, 255, 255, 26));
  circle(x + w * 0.85f, y + h * 0.8f + cosf(t * 0.8f) * 6, 64, RGBA(255, 255, 255, 20));
  unsigned ink = RGBA(255, 255, 255, 255), shade = RGBA(0, 0, 0, 60);
  if (k == 0) {
    // a vault: three cards fanned, a heart on the front one, a lock on its corner
    for (int c = 2; c >= 0; c--) {
      float ox = (c - 1) * 34, oy = c == 1 ? -10 : 6;
      rrect(cx - 60 + ox + 4, cy - 70 + oy + 6, 120, 150, 16, shade);
      rrect(cx - 60 + ox, cy - 70 + oy, 120, 150, 16, c == 1 ? ink : RGBA(255, 255, 255, 150 + c * 20));
    }
    rrect(cx - 46, cy - 66, 92, 86, 10, with_alpha(ACC, 90));
    circle(cx - 18, cy - 40, 9, RGBA(255, 255, 255, 220));
    heart(cx - 34, cy + 54, 8, ACC);
    rrect(cx - 10, cy + 46, 50, 8, 4, with_alpha(ACC, 120));
    circle(cx + 52, cy - 70, 22, RGBA(20, 12, 16, 230));
    rrect(cx + 44, cy - 72, 16, 13, 3, ink);
    circle(cx + 52, cy - 74, 6, ink);
    circle(cx + 52, cy - 74, 3, RGBA(20, 12, 16, 230));
  } else if (k == 1) {
    // a Vita: its screen with a finger swiping across, and three dots for the three-finger tap
    rrect(cx - 120 + 4, cy - 62 + 6, 240, 124, 40, shade);
    rrect(cx - 120, cy - 62, 240, 124, 40, RGBA(20, 12, 16, 235));
    rrect(cx - 78, cy - 46, 156, 92, 8, with_alpha(ink, 40));
    circle(cx - 100, cy - 10, 8, with_alpha(ink, 90));
    circle(cx + 100, cy - 10, 8, with_alpha(ink, 90));
    float sw = fmodf(t * 30, 90);
    circle(cx - 40 + sw, cy - 8, 13, with_alpha(ink, 230));
    for (int d = 1; d < 4; d++) circle(cx - 40 + sw - d * 12, cy - 8, 6 - d, with_alpha(ink, 90 - d * 20));
    for (int d = 0; d < 3; d++) circle(cx - 28 + d * 28, cy + 26, 7, with_alpha(ink, 160 + 30 * (((int)(t * 2) + d) % 3 == 0)));
  } else {
    // a phone sending to the Vita over Wi-Fi
    rrect(cx - 128 + 3, cy - 64 + 5, 74, 128, 14, shade);
    rrect(cx - 128, cy - 64, 74, 128, 14, ink);
    rrect(cx - 120, cy - 52, 58, 96, 6, with_alpha(ACC, 120));
    rrect(cx + 20 + 4, cy - 40 + 6, 120, 70, 24, shade);
    rrect(cx + 20, cy - 40, 120, 70, 24, RGBA(20, 12, 16, 235));
    rrect(cx + 44, cy - 30, 72, 50, 5, with_alpha(ink, 50));
    // the waves between, moving
    for (int d = 0; d < 3; d++) {
      float p = fmodf(t * 0.6f + d / 3.0f, 1.0f);
      circle(cx - 36 + p * 50, cy - 4, 5, with_alpha(ink, (int)(230 * (1 - p))));
    }
    icon("wifi", cx - 16, cy - 56, ink);
  }
}
static void draw_onboarding(void) {
  static int page;
  static float slide;
  static const struct { const char *kicker, *title, *body; } CARDS[3] = {
    {"WELCOME", "Your vault",
     "Pictures, videos and comics, kept in a folder on the memory card: the same vault as on the PC and the phones, so it moves between them as it is.\n"
     "Next, pick one or start a new one. A PIN can lock it, and a decoy PIN opens a harmless one."},
    {"HOW IT MOVES", "Gestures", NULL},
    {"FROM ANYWHERE", "Send from a phone or PC",
     "Settings · Wi-Fi transfer shows an address. Open it in a browser on the same Wi-Fi and drop pictures and videos in: they land in the vault.\n"
     "The Browser finds more on the sites you choose; Favourites keeps long videos as links."},
  };
  static const struct { const char *icon, *text; } MOVES[] = {
    {"chev_r", "Swipe across: the next one. Swipe down: close it"},
    {"photos", "Pinch the Library: bigger or smaller cards"},
    {"refresh", "Double tap a video's side: ten seconds back or on"},
    {"info", "Three fingers, or SELECT: the calculator covers it"},
    {"tag", "Hold L and R: touch lock"},
  };
  static int was_touching;
  if (in.touching) slide -= in.drag_dx / (float)(W - 120);
  else {
    if (was_touching) { if (in.flicked && fabsf(in.fvx) > 6) page += in.fvx < 0 ? 1 : -1; else page = (int)roundf(slide); }
    if (page < 0) page = 0;
    if (page > 2) page = 2;
    slide += (page - slide) * ease(0.2f);
  }
  was_touching = in.touching;
  int done = 0;
  if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS | SCE_CTRL_RTRIGGER)) {
    if (page == 2 && (in.pressed & SCE_CTRL_CROSS)) done = 1;
    else if (page < 2) { page++; sfx(SFX_TICK); }
  }
  if ((in.pressed & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER | SCE_CTRL_CIRCLE)) && page > 0) { page--; sfx(SFX_TICK); }
  const int cw = W - 120, ch = 400, cy = 34;
  for (int k = 0; k < 3; k++) {
    float x = W / 2 - cw / 2 + (k - slide) * (cw + 40);
    if (x > W || x + cw < 0) continue;
    rrect(x + 4, cy + 8, cw, ch, 28, RGBA(0, 0, 0, 70));
    rrect(x, cy, cw, ch, 28, SURF);
    float pw = cw * 0.44f;
    onboard_art(k, x + 14, cy + 14, pw, ch - 28);
    float tx = x + pw + 44, tw = cw - pw - 76;
    text(tx, cy + 58, ACC, 13, CARDS[k].kicker);
    text(tx, cy + 100, TEXT, 32, CARDS[k].title);
    rrect(tx, cy + 116, 40, 4, 2, ACC);
    if (CARDS[k].body) text_wrap(tx, cy + 152, DIM, 16, CARDS[k].body, tw, 25);
    else
      for (int m = 0; m < 5; m++) {
        float ry = cy + 136 + m * 48;
        rrect(tx, ry, 34, 34, 11, with_alpha(ACC, 50));
        icon(MOVES[m].icon, tx + 17, ry + 17, ACC);
        text_wrap(tx + 48, ry + 22, DIM, 15, MOVES[m].text, tw - 48, 20);
      }
  }
  // the dots, Skip, and the button
  int by = cy + ch + 22;
  for (int k = 0; k < 3; k++) {
    float on = 1 - fminf(1, fabsf(slide - k));
    float w = 10 + 18 * on;
    float x = W / 2 - 40 + k * 28 - (w - 10) / 2;
    rrect(x, by + 17, w, 10, 5, on > 0.5f ? ACC : with_alpha(TEXT, 60));
  }
  const char *btn = page == 2 ? "Get started" : "Next";
  int bw = text_w(18, btn) + 64, bx = W - 60 - bw;
  rrect(bx, by, bw, 44, 22, ACC);
  text_center(bx + bw / 2 - 8, by + 29, RGBA(20, 12, 16, 255), 18, btn);
  text(bx + bw - 30, by + 29, RGBA(20, 12, 16, 255), 16, "✕");
  if (hit(bx, by, bw, 44)) { if (page == 2) done = 1; else { page++; sfx(SFX_TICK); } }
  if (page < 2) { text(64, by + 29, FAINT, 15, "Skip"); if (hit(44, by, 90, 44)) done = 1; }
  if (done) { sfx(SFX_DONE); cfg.onboarded = 1; cfg_save(); }
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
static volatile int woke, going_away;
static pthread_mutex_t settings_mx = PTHREAD_MUTEX_INITIALIZER;
// (the settings and the slideshow's plan written now if they're waiting: the app may be frozen, or
//  closed from the LiveArea, any moment after the PS button)
static void prog_flush(int now); // (manga_ui.c: where each comic was left)
static void flush_settings(void) {
  pthread_mutex_lock(&settings_mx);
  if (cfg_dirty_at) cfg_write();
  if (show_dirty_at) show_write();
  prog_flush(1);
  pthread_mutex_unlock(&settings_mx);
}
static int power_cb(int notify_id, int count, int arg, void *user) {
  if (arg & (SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_APP_RESUME)) woke = 1;
  // the PS button, or the app or the system about to be suspended: what's unsaved written at once,
  // and the main loop pauses the videos and saves the rest (it may still run a few frames)
  if (arg & (SCE_POWER_CB_BUTTON_PS_PRESS | SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_SYSTEM_SUSPEND)) {
    flush_settings();
    going_away = 1;
  }
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
  if (screen != S_LIBRARY && vault.n > before) lib_new += vault.n - before;
  pthread_mutex_lock(&mx);
  signed char *t = realloc(tstate, vault.n + 1);
  short *so = realloc(slot_of, (vault.n + 1) * sizeof *so);
  unsigned short *zw = realloc(tsize_w, (vault.n + 1) * sizeof *zw), *zh = zw ? realloc(tsize_h, (vault.n + 1) * sizeof *zh) : NULL;
  if (t) tstate = t;
  if (so) slot_of = so;
  if (zw) tsize_w = zw;
  if (zh) tsize_h = zh;
  if (t && so) for (int i = before; i <= vault.n; i++) { tstate[i] = 0; slot_of[i] = -1; }
  if (zw && zh) for (int i = before; i <= vault.n; i++) tsize_w[i] = tsize_h[i] = 0;
  pthread_mutex_unlock(&mx);
  if (picked) { unsigned char *pk = realloc(picked, vault.n + 64); if (pk) { picked = pk; memset(picked + before, 0, vault.n + 64 - before); } }
  // (filtered and sorted again once at the next frame, not for each item: a chapter or a Wi-Fi
  //  batch adds dozens in a row)
  refilter_wanted = 1;
}

#include "gifplay.c"
#include "web.c"
#include "rear.c"
// ----- your own tags, asked for after a save -----
// The tags you've given before (newest first, DATA/own_tags.txt) as chips to pick, + New to type one,
// Done puts the picked ones at the front of the item's tags (yours before the site's: they're what
// shows and is searched first), Skip leaves it.
static char otag_uid[16], otag_list[40][48], otag_pick[40];
static int otag_open, otag_n, otag_sel;
static unsigned otag_at;
static void otag_load(void) {
  otag_n = 0;
  FILE *f = fopen(DATA "/own_tags.txt", "r");
  char l[64];
  while (f && otag_n < 40 && fgets(l, sizeof l, f)) { l[strcspn(l, "\r\n")] = 0; if (l[0]) snprintf(otag_list[otag_n++], 48, "%s", l); }
  if (f) fclose(f);
}
static void otag_save(void) {
  FILE *f = fopen(DATA "/own_tags.txt", "w");
  if (!f) return;
  for (int k = 0; k < otag_n; k++) fprintf(f, "%s\n", otag_list[k]);
  fclose(f);
}
static void otag_add(const char *t) { // (a new one: picked, at the front)
  char c[48]; size_t j = 0;
  for (const char *p = t; *p && j < sizeof c - 1; p++) c[j++] = *p == ' ' ? '_' : (char)tolower((unsigned char)*p);
  c[j] = 0;
  if (!c[0]) return;
  for (int k = 0; k < otag_n; k++) if (!strcmp(otag_list[k], c)) { otag_pick[k] = 1; return; }
  if (otag_n == 40) otag_n--;
  memmove(otag_list[1], otag_list[0], sizeof otag_list[0] * otag_n); memmove(otag_pick + 1, otag_pick, otag_n);
  snprintf(otag_list[0], 48, "%s", c); otag_pick[0] = 1; otag_n++;
}
static void otag_begin(const char *uid) {
  snprintf(otag_uid, sizeof otag_uid, "%s", uid);
  otag_load(); memset(otag_pick, 0, sizeof otag_pick);
  otag_open = 1; otag_sel = 0; otag_at = frame_no;
}
static void otag_done(int apply) {
  otag_open = 0;
  if (!apply) { sfx(SFX_CLOSE); return; }
  int idx = -1;
  for (int i = 0; i < vault.n; i++) if (vault.items[i].uid && !strcmp(vault.items[i].uid, otag_uid)) { idx = i; break; }
  char t[4096] = "";
  size_t n = 0;
  for (int k = 0; k < otag_n; k++) if (otag_pick[k]) n += snprintf(t + n, sizeof t - n, "%s%s", n ? " " : "", otag_list[k]);
  if (!n) { sfx(SFX_CLOSE); return; }
  // (the picked ones to the front of the list kept, so the next time they come first)
  char keep[40][48]; int nk = 0;
  for (int k = 0; k < otag_n; k++) if (otag_pick[k]) snprintf(keep[nk++], 48, "%s", otag_list[k]);
  for (int k = 0; k < otag_n; k++) if (!otag_pick[k]) snprintf(keep[nk++], 48, "%s", otag_list[k]);
  memcpy(otag_list, keep, sizeof keep); otag_save();
  if (idx >= 0) {
    const char *old = vault.items[idx].tags ? vault.items[idx].tags : "";
    if (old[0] && n < sizeof t - 2) snprintf(t + n, sizeof t - n, " %s", old);
    if (lv_item_set(&vault, idx, "tags", t) == 0) edited();
  }
  sfx(SFX_DONE); say("Your tags added");
}
static void draw_otag(void) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 150));
  float rise = 1 - fminf(1, (frame_no - otag_at) / 12.0f);
  const int ww = 640, wh = 300, wx = (W - ww) / 2, wy = (H - wh) / 2 + (int)(rise * rise * 40);
  rrect(wx, wy + 4, ww, wh, 24, RGBA(0, 0, 0, 90));
  rrect(wx, wy, ww, wh, 24, SURF);
  rrect(wx + 2, wy + 2, ww - 4, 56, 22, with_alpha(TEXT, 8));
  icon("tag", wx + 30, wy + 30, ACC);
  text(wx + 52, wy + 38, TEXT, 20, "Your tags for it");
  text(wx + ww - 24 - text_w(12, "yours come before the site's"), wy + 36, FAINT, 12, "yours come before the site's");
  // the chips: + New, then the ones given before (places: 0 New, 1.. the tags, then Skip, Done)
  int cx = wx + 20, cy = wy + 72, nplaces = otag_n + 3;
  for (int k = -1; k < otag_n; k++) {
    const char *lab = k < 0 ? "+ New" : otag_list[k];
    int cw = text_w(15, lab) + 28;
    if (cx + cw > wx + ww - 20) { cx = wx + 20; cy += 42; }
    if (cy > wy + wh - 100) break;
    int place = k + 1, on = place == otag_sel, picked = k >= 0 && otag_pick[k];
    rrect(cx, cy, cw, 34, 17, picked ? ACC : k < 0 ? with_alpha(ACC, 50) : SURF2);
    text(cx + 14, cy + 23, picked ? RGBA(20, 12, 16, 255) : TEXT, 15, lab);
    if (on) focus_ring(cx, cy, cw, 34);
    if (hit(cx, cy, cw, 34)) { otag_sel = place; in.pressed |= SCE_CTRL_CROSS; }
    cx += cw + 8;
  }
  if (!otag_n) text(wx + 130, wy + 95, FAINT, 14, "None yet: + New to give it your first");
  const char *bl[2] = {"Skip", "Done"};
  for (int b = 0; b < 2; b++) {
    int bx = wx + ww - 20 - (2 - b) * 130 + 10, by = wy + wh - 60, place = otag_n + 1 + b;
    rrect(bx, by, 120, 42, 21, b ? ACC : SURF2);
    text_center(bx + 60, by + 28, b ? RGBA(20, 12, 16, 255) : TEXT, 17, bl[b]);
    if (place == otag_sel) focus_ring(bx, by, 120, 42);
    if (hit(bx, by, 120, 42)) { otag_sel = place; in.pressed |= SCE_CTRL_CROSS; }
  }
  if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_DOWN)) { otag_sel = (otag_sel + 1) % nplaces; sfx(SFX_TICK); }
  if (in.pressed & (SCE_CTRL_LEFT | SCE_CTRL_UP)) { otag_sel = (otag_sel + nplaces - 1) % nplaces; sfx(SFX_TICK); }
  if (in.pressed & SCE_CTRL_CROSS) {
    if (otag_sel == 0) { if (!ime_open) ime_start(22, "A tag of your own", "", 0); }
    else if (otag_sel <= otag_n) { otag_pick[otag_sel - 1] ^= 1; sfx(SFX_TOGGLE); }
    else otag_done(otag_sel == otag_n + 2);
  }
  if (in.pressed & SCE_CTRL_CIRCLE) otag_done(0);
  if (in.pressed & SCE_CTRL_START) otag_done(1);
}
#include "wifi.c"
#include "vbrowse.c"
#include "manga_ui.c"
#include "favs.c"

// ---------- the accent from the picture (Settings: Accent from the picture) ----------
// The picture's own colour: its thumbnail looked at in a 12 x 12 grid, each point weighed by how
// vivid it is (saturation times brightness), the hues averaged round the wheel. Kept bright and
// saturated enough to read on the dark background; the accent glides to it.
static int vivid_of(vita2d_texture *t, float *ph, float *ps) {
  if (!t) return 0;
  const uint8_t *d = sceGxmTextureGetData(&t->gxm_tex);
  int w = vita2d_texture_get_width(t), h = vita2d_texture_get_height(t);
  unsigned stride = sceGxmTextureGetStride(&t->gxm_tex);
  if (!d || w < 4 || h < 4) return 0;
  if (!stride) stride = ((w + 7) & ~7) * 4;
  float sx = 0, sy = 0, wsum = 0, ssum = 0;
  for (int gy = 0; gy < 12; gy++)
    for (int gx = 0; gx < 12; gx++) {
      const uint8_t *p = d + (size_t)(gy * h / 12) * stride + (size_t)(gx * w / 12) * 4;
      float r = p[0] / 255.f, g = p[1] / 255.f, b = p[2] / 255.f;
      float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), c = mx - mn;
      if (mx < 0.2f || c < 0.08f) continue; // (dark or grey: no colour to speak of)
      float s = c / mx, hue;
      if (mx == r) hue = fmodf((g - b) / c, 6); else if (mx == g) hue = (b - r) / c + 2; else hue = (r - g) / c + 4;
      hue *= 60; if (hue < 0) hue += 360;
      float wgt = s * mx; wgt *= wgt;
      sx += cosf(hue * 0.0174533f) * wgt; sy += sinf(hue * 0.0174533f) * wgt; wsum += wgt; ssum += s * wgt;
    }
  if (wsum < 0.02f) return 0;
  float hue = atan2f(sy, sx) / 0.0174533f;
  *ph = hue < 0 ? hue + 360 : hue;
  *ps = ssum / wsum;
  return 1;
}
static void accent_follow(void) {
  static int last_item = -2, has;
  static unsigned target;
  if (!cfg.accpic) return;
  int item = -1;
  if (screen == S_VIEWER && vk >= 0 && vk < nshown) item = shown[vk];
  else if (screen == S_LIBRARY && sel >= 0 && sel < nshown) item = shown[sel];
  if (item != last_item && frame_no % 4 == 0) {
    float h, sat;
    vita2d_texture *t = item >= 0 ? thumb_of(item) : NULL;
    if (item < 0) { has = 0; last_item = item; }
    else if (t) {
      has = vivid_of(t, &h, &sat);
      if (has) target = hsv(h, fminf(0.9f, fmaxf(0.5f, sat)), 0.96f); // (bright and coloured enough to read)
      last_item = item;
    }
  }
  unsigned want = has && item >= 0 ? target : acc_theme;
  if (ACC != want) {
    ACC = mix_col(ACC, want, ease(0.1f)) | 0xFF000000u;
    unsigned dr = abs((int)(ACC & 255) - (int)(want & 255)) + abs((int)((ACC >> 8) & 255) - (int)((want >> 8) & 255)) + abs((int)((ACC >> 16) & 255) - (int)((want >> 16) & 255));
    if (dr < 4) ACC = want;
  }
}
static void warm_icons(void) {
  static SceUID dir = -2;
  static int site = 0;
  for (int n = 0; n < 2; n++) {
    if (dir == -2) dir = sceIoDopen("app0:ui");
    if (dir >= 0) {
      SceIoDirent e;
      memset(&e, 0, sizeof e);
      if (sceIoDread(dir, &e) > 0) {
        char *dot = strrchr(e.d_name, '.');
        if (dot && !strcmp(dot, ".png")) { *dot = 0; char *nm = strdup(e.d_name); if (nm) ui_icon(nm); } // (kept: the table points at it)
        continue;
      }
      sceIoDclose(dir); dir = -1;
    }
    // then the sites': pictures, video sources, comics (their slots as the sites window has them)
    int nv = nvsources();
    if (site < LV_NSITES) { site_icon_named(site, LV_SITES[site].icon); site++; }
    else if (site < LV_NSITES + nv && site - LV_NSITES < 32) { int k = site - LV_NSITES; site_icon_named(32 + k, vsource_icon(k)); site++; }
    else if (site < LV_NSITES + nv + MS_N) { int k = site - LV_NSITES - nv; site_icon_named(64 + k, LV_MSRC[k].icon); site++; }
    else return;
  }
}

// ---------- each part of the app gives its memory back once it's been left ----------
// The parts each hold textures while they're used (the Browser's previews, 16 MB of atlases; the
// video browser's cards; the comics' covers and pages; Favourites' covers; the full views' screen
// buffers; Settings' colour wheel). Moving within a part keeps them; a part not seen for a minute,
// or sooner when memory runs short, lets them go (its lists stay: what's on screen is asked for
// again when it's back). So the parts don't add up: each is about what's on screen now.
enum { A_NONE, A_LIB, A_VIEWER, A_WEB, A_WEBVIEW, A_VIDEOS, A_COMICS, A_FAVS, A_SETTINGS, A_N };
static const char *const A_NAME[A_N] = {"", "library", "viewer", "browser", "full view", "videos", "comics", "favourites", "settings"};
static int area_of(int s) {
  switch (s) {
    case S_LIBRARY: case S_SHOW: case S_STATS: case S_CLEAN: case S_HEALTH: case S_DUPES: return A_LIB;
    case S_VIEWER: return A_VIEWER;
    case S_WEB: return A_WEB;
    case S_WEBVIEW: return A_WEBVIEW;
    case S_VIDEOS: return A_VIDEOS;
    case S_MANGA: case S_BOOK: case S_READER: return A_COMICS;
    case S_FAVS: return A_FAVS;
    case S_SETTINGS: return A_SETTINGS;
  }
  return A_NONE;
}
static unsigned char area_held[A_N];
static void scr_set_free(int set) {
  for (int k = set * 3; k < set * 3 + 3; k++) if (scr_buf[k]) { free_later(scr_buf[k]); scr_buf[k] = NULL; }
}
static void area_release(int a) {
  switch (a) {
    case A_LIB: { // (only when memory is short: the Library's thumbnails, made again as it's scrolled)
      pthread_mutex_lock(&mx);
      for (int k = 0; k < SLOTS; k++) {
        int i = slots[k].item;
        if (i >= 0 && i < vault.n && tstate && tstate[i] == 2) { tstate[i] = 0; slot_of[i] = -1; }
        slots[k].tex = NULL; slots[k].item = -1; slots[k].used = 0;
      }
      pthread_mutex_unlock(&mx);
      for (int k = 0; k < SLOTS / CELLS_PER_ATLAS; k++) if (atlas[k]) { free_later(atlas[k]); atlas[k] = NULL; }
      break;
    }
    case A_VIEWER: // (the Library's full view: the last picture, the one before it, a comic's pages, the screen buffers)
      release_tex(big_tex); big_tex = NULL; big_item = -1; big_hi_item = -1;
      release_tex(fade_tex); fade_tex = NULL;
      lc_forget();
      scr_set_free(0);
      break;
    case A_WEBVIEW: // (the Browser's full view: its picture went when it closed; the screen buffers)
      if (!wv_tex) scr_set_free(1);
      break;
    case A_WEB: {
      // (the previews' four atlases: who had a cell is forgotten, so what's on screen is asked again)
      pthread_mutex_lock(&wmx);
      for (int k = 0; k < WSLOTS; k++) {
        int p = wslots[k].post;
        if (p >= 0 && p < nposts && wstate && wstate[p] == 2) { wstate[p] = 0; wslot_of[p] = -1; }
      }
      web_clear_textures();
      pthread_mutex_unlock(&wmx);
      for (int k = SLOTS / CELLS_PER_ATLAS; k < ATLASES; k++) if (atlas[k]) { free_later(atlas[k]); atlas[k] = NULL; }
      break;
    }
    case A_VIDEOS:
      pthread_mutex_lock(&vbmx);
      for (int r = 0; r < VB_ROWS; r++)
        for (int k = 0; k < VB_PER; k++) {
          if (vrows[r].tex[k]) { release_tex(vrows[r].tex[k]); vrows[r].tex[k] = NULL; }
          if (vrows[r].ts[k] == 2) vrows[r].ts[k] = 0;
        }
      pthread_mutex_unlock(&vbmx);
      if (vb_hero) { release_tex(vb_hero); vb_hero = NULL; vb_hero_state = 0; }
      pool_release(&pool_cards);
      break;
    case A_COMICS:
      for (int k = 0; k < MG_TEX; k++) {
        if (!mg.tex[k]) continue;
        int b = mg.tex_book[k];
        if (mg.cstate && b >= 0 && b < mg.nbooks && mg.cstate[b] == 2) mg.cstate[b] = 0;
        release_tex(mg.tex[k]); mg.tex[k] = NULL; mg.tex_book[k] = -1;
      }
      for (int k = 0; k < 3; k++) { release_tex(mg.ptex[k]); mg.ptex[k] = NULL; mg.ptex_page[k] = -1; }
      pool_release(&pool_covers);
      // (the strips' bands: a few MB of the heap; not while one's being cut: never a wait for it here)
      if (pthread_mutex_trylock(&strip_cut_mx) == 0) {
        pthread_mutex_lock(&strip_mx);
        for (int k = 0; k < 2; k++) strip_free(&strips[k]);
        pthread_mutex_unlock(&strip_mx);
        pthread_mutex_unlock(&strip_cut_mx);
      }
      break;
    case A_FAVS: fav_drop_textures(); pool_release(&pool_favs); break;
    case A_SETTINGS: if (wheel_tex) { free_later(wheel_tex); wheel_tex = NULL; } break;
  }
}
static unsigned area_frame[A_N]; // (the frame each part was last on screen)
#ifdef LV_TESTKIT
static int test_lowmem;      // (the test kit's memory test: memory said to be short everywhere)
static unsigned tk_released; // (parts that gave their memory back, counted for its report)
#endif
static SceUID main_thread;
// Every part not on screen gives its memory back. Off screen 3 frames or more, its textures go at once
// (the GPU's done with them); else as usual, three frames on.
static int release_hidden(const char *why, int at_once) {
  int a = area_of(screen), n = 0;
  int linked = a == A_WEBVIEW ? (wv_back == S_VIDEOS ? A_VIDEOS : A_WEB) : (a == A_VIEWER || a == A_SETTINGS) ? A_LIB : -1;
  for (int k = A_LIB; k < A_N; k++) {
    if (k == a || k == linked || !area_held[k]) continue;
    free_backdate = at_once && frame_no - area_frame[k] >= 3 ? 3 : 0;
    area_release(k);
    free_backdate = 0;
    area_held[k] = 0;
    n++;
    LOG("released %s (%s)\n", A_NAME[k], why);
  }
  if (n && at_once) free_due();
#ifdef LV_TESTKIT
  tk_released += n;
#endif
  return n;
}
static int release_hidden_now(void) { return sceKernelGetThreadId() == main_thread && release_hidden("no room for a texture", 1) > 0; }
// whether memory is short: main memory, video memory or the heap within reach of their reserves
static int mem_short(size_t need) {
#ifdef LV_DEBUG
  // (debug builds: a file that says memory is always short, to test letting go on every screen)
  static int forced = -1;
  if (forced < 0) { SceIoStat st; forced = sceIoGetstat(DATA "/debug_lowmem.txt", &st) >= 0; }
  if (forced) return 1;
#endif
#ifdef LV_TESTKIT
  if (test_lowmem) return 1;
#endif
  int fu = 0, fc = 0;
  return (mem_free(&fu, &fc) == 0 && ((size_t)fu < RESERVE_USER + need || (size_t)fc < RESERVE_CDRAM + need)) || !heap_room(need);
}
// Room for something big about to be made (a video's player, a picture at twice the screen): the
// parts not on screen let go first, if it's needed
static void make_room(size_t need) { if (mem_short(need)) release_hidden("making room", 1); }
// Every frame: the part on screen is noted. The others keep what they hold (held, not drawn, it costs
// nothing) until memory is short; then they let it go, and get it again when they're back.
static void janitor(void) {
  int a = area_of(screen);
  area_held[a] = 1; area_frame[a] = frame_no;
  if (frame_no % 10) return; // (six times a second)
  drive_reap();
  if (mem_short(24u << 20)) release_hidden("memory short", 1);
}
#ifdef LV_TESTKIT
#include "test.c"
#endif
#ifdef LV_DEBUG
// (debug builds, at each change of screen: what each part still holds, to find what a screen leaves behind)
static const char *res_extra(void) {
  static char out[256];
  int at = 0, sb = 0, ui = 0, st = 0, mt = 0, vt = 0, ft = 0;
  for (int k = 0; k < ATLASES; k++) at += atlas[k] != NULL;
  for (int k = 0; k < 6; k++) sb += scr_buf[k] != NULL;
  for (int k = 0; k < UI_ICONS; k++) ui += ui_icons[k].t != NULL;
  for (int k = 0; k < 96; k++) st += site_tex[k] != NULL;
  for (int k = 0; k < MG_TEX; k++) mt += mg.tex[k] != NULL;
  for (int k = 0; k < 3; k++) mt += mg.ptex[k] != NULL;
  for (int r = 0; r < VB_ROWS; r++) for (int k = 0; k < VB_PER; k++) vt += vrows[r].tex[k] != NULL;
  for (int k = 0; k < FAV_MAX; k++) ft += fav_tex[k] != NULL;
  snprintf(out, sizeof out, "atlas=%d scrbuf=%d big=%d fade=%d wv=%d icons=%d sites=%d manga=%d vb=%d+%d favs=%d wheel=%d posts=%d books=%d",
           at, sb, big_tex != NULL, fade_tex != NULL, wv_tex != NULL, ui, st, mt, vt, vb_hero != NULL, ft, wheel_tex != NULL, nposts, mg.nbooks);
  return out;
}
#endif

#ifdef LV_DEBUG
static const char *res_extra(void);
#endif
static void janitor(void);
static void warm_icons(void);
static void accent_follow(void);
int main(void) {
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir(DATA, 0777);
#ifdef LV_TESTKIT
  tk_boot(); // (the last run's log kept, and a crash report if it ended without a goodbye)
#endif
  sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
  SceAppUtilInitParam ip;
  SceAppUtilBootParam bp;
  memset(&ip, 0, sizeof ip);
  memset(&bp, 0, sizeof bp);
  sceAppUtilInit(&ip, &bp);
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
  pin_to_core(0); // (the drawing thread on the first core; the work on the other two)
  main_thread = sceKernelGetThreadId();
  tex_no_room = release_hidden_now;
  // (the videos played last time, still on the card: they were only for playing, and can be large)
  {
    static const char *const tmp[] = {"play.mp4", "play.webm", "play0.mp4", "play0.webm", "play1.mp4", "play1.webm", "conv480.mp4", "play.mp4.part", "play.webm.part", NULL};
    for (int k = 0; tmp[k]; k++) { char p[96]; snprintf(p, sizeof p, DATA "/%s", tmp[k]); sceIoRemove(p); }
  }
  // (a bigger per-frame vertex pool than vita2d's 1 MB: room to spare on the busiest screen)
  vita2d_init_advanced(POOL_SIZE);
  make_disc();
  make_sqdisc();
  make_ring();
  splash_tex = vita2d_load_PNG_file("app0:splash.png");
  sfx_init(); // (no sounds read yet: the first one played reads the pack chosen, below)
  lv_video_release_texture = free_later;
  vita2d_set_clear_color(BG);
  vita2d_set_vblank_wait(0); // (main's loop waits with sceDisplayWaitVblankStartCB)
  font_at(16); // (the font read in, and its commonest size made)
  pgf = vita2d_load_default_pgf();
  rnd_state ^= (unsigned)time(NULL);
  cfg_load();
  text_pct = cfg.tscale ? cfg.tscale : 100;
  apply_theme(); // (the background and accent chosen, from the first screen on)
  vita2d_set_clear_color(BG);
  sfx_pack(SND_PACKS[cfg.sndpack]); // (just where to read from: nothing's read until a sound plays)
  show_load();
  drive_load();
  sfx_volume(cfg.sounds);
  sfx(SFX_START);

  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024); // (it decodes pictures and makes JPEGs)
  if (pthread_create(&th, &at, worker, NULL) != 0) LOG("the thumbnail thread didn't start\n");
  SceUID pt = sceKernelCreateThread("lv_power", power_main, 0x10000100, 0x10000, 0, 0, NULL);
  if (pt >= 0) sceKernelStartThread(pt, 0, NULL);
  atlas_init();
  saver_start();
  closer_start();
  // (no network at start: libcurl and OpenSSL are started by the first thread that fetches, when the
  //  Browser, Drive or the rest is first used; a Library-only session never starts them)

  find_vaults();
  pin_load();
  // started from a frame of the LiveArea: that vault (after the PIN, if there is one)
  {
    char param[1024] = "";
    if (sceAppMgrGetAppParam(param) >= 0) launch_to = la_target(param);
    la_from_frame = launch_to;
#ifdef LV_TESTKIT
    snprintf(tk_boot_param, sizeof tk_boot_param, "%s", param);
#endif
    if (launch_to) LOG("started from the LiveArea: %s\n", param);
  }
  if (pin.on) pin_lock(-1);
  else open_start_vault();
  lv_net_warm(); // (the network's base, on a thread of its own: the online light and the Browser never wait for it)

  unsigned idle_save = 0;
  for (;;) {
    frame_no++;
    {
      static uint64_t last;
      uint64_t t = sceKernelGetProcessTimeWide();
      float f = last ? (t - last) / 16666.7f : 1;
      ft = f < 0.75f ? 1 : f > 4 ? 4 : f; // (a long pause, a suspend: as one slow frame, not a jump)
      frame_len_ms = last ? (t - last) / 1000.0f : 16.667f; // (as it was: the test kit's measures)
      if (ft < 1.15f) ft = 1; // (60 fps, give or take: exactly the same as before)
      last = t;
      frame_t0 = t;
    }
#ifdef LV_DEBUG
    uint64_t perf_t0 = sceKernelGetProcessTimeWide();
#endif
    PT0();
    read_input();
    rear_apply(); // (the rear touchpad: what each half does)
    PT("read_input");
    if (frame_no < splash_until) { in.pressed = 0; in.tap = 0; in.touching = 0; } // (nothing happens under the loading screen)
    // (under the loading screen, a size a frame: the letters the screens use most drawn into the font's
    //  cache now, so the first time a screen shows text at that size it isn't drawing them then)
    {
      static const unsigned char WARM[] = {13, 14, 15, 16, 17, 18, 20};
      static unsigned warmed;
      if (warmed < sizeof WARM && frame_no < splash_until && frame_no > 2)
        text_w_raw(WARM[warmed++], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,:;!?'\"()-+/%&");
    }
    // (and the icons: the app's own, then the sites', two a frame, read from the card now rather than
    //  the first time a screen shows them)
    if (frame_no < splash_until && frame_no > 10) warm_icons();
#ifdef LV_TESTKIT
    if (frame_no >= splash_until) tk_frame_input();
#endif
    if (in.touching || in.tap) touch_mode = 1;
    else if (in.pressed || fabsf(in.lx) > 0.5f || fabsf(in.ly) > 0.5f) touch_mode = 0;
    // touch lock: nothing gets through but SELECT (the disguise still works)
    if (screen != S_PIN && screen != S_CALC && touch_lock_filter()) {
      in.pressed &= SCE_CTRL_SELECT;
      in.now = 0;
      in.tap = in.dbl = in.touching = in.flicked = 0;
      in.lx = in.ly = in.bdx = in.bdy = in.drag_dx = in.drag_dy = 0;
      in.back_on = 0;
    }
    // textures let go three frames ago or more: the GPU is done with them (vita2d has two frames in
    // flight at most), so they're freed without waiting for it
    if (npending) { free_due(); PT("textures freed"); }
    videos_close_now();
    PT("videos closed");
    // (a backup for the sleep callback: the wall clock jumped, but frames don't take seconds;
    //  looked at twice a second, not every frame)
    if (frame_no % 30 == 0) {
      static time_t last_wall;
      time_t now_wall = time(NULL);
      if (last_wall && now_wall - last_wall > 6) woke = 1;
      last_wall = now_wall;
    }
    if (going_away) {
      // (the PS button: nothing keeps playing behind the LiveArea, and nothing is left unsaved)
      going_away = 0;
#ifdef LV_TESTKIT
      tk_put_away();
#endif
      if (vid && !lv_video_paused(vid)) lv_video_pause(vid, 1);
      if (wv_vid && !lv_video_paused(wv_vid)) lv_video_pause(wv_vid, 1);
      slideshow = 0;
      if (loaded_vault) { save_edits_now(); views_save_async(); resume_save(); }
      flush_settings();
      plain_wipe_ask(); plain_wipe_try();
    }
    if (woke) {
      woke = 0;
      lv_net_reset();
      if (vid) { stop_video(); say("Paused while the Vita slept"); }
      if (wv_vid) { video_close_later(wv_vid); wv_vid = NULL; }
      // (the Wi-Fi transfer's socket doesn't outlive a sleep: a fresh one)
      if (wf_run) { wf_stop(); wf_last[0] = 0; if (screen == S_WIFI) wf_start(); }
      if (pin.on && pin.on_wake && screen != S_PIN) { slideshow = 0; pin_lock(screen == S_COVER ? cover_return : screen); }
    }
    // (a video playing, or a slideshow: the Vita doesn't dim or sleep)
    if ((vid && !lv_video_paused(vid)) || (wv_vid && !lv_video_paused(wv_vid)) || (screen == S_VIEWER && slideshow) || drive.state == DRV_SYNCING || drive.state == DRV_CODE || wf_run ||
        dl_post >= 0 || msave_on || conv_on) { // (and while a video downloads or a chapter is saved: sleep would cut the Wi-Fi)
      // (once a second: the dimming and sleep timers are minutes long, a call every frame is wasted)
      static unsigned tick_at;
      if (frame_no - tick_at >= 60) { tick_at = frame_no; sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); }
    }
    // (an offline copy being saved, a chapter, a conversion: the PS button is held until it's done,
    //  since leaving suspends the app and cuts the download; it works again as soon as it finishes)
    {
      static int locked, events;
      int want = 0; // (never held: leaving the app is always allowed; what was downloading is stopped)
      if (want != locked) {
        if (!events) { sceShellUtilInitEvents(0); events = 1; }
        if (want) sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN);
        else sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN);
        locked = want;
      }
    }
    PT("frame start");
    if (loaded_vault) take_results();
    PT("take_results");
    if (web_started) web_take_results();
    PT("web_take_results");
    if (loaded_vault) wf_take();
    msave_take();
    conv_take();
    favs_drive_take();
    if (frame_no % 30 == 0 && screen != S_WEBVIEW) plain_wipe_try();
    if (refilter_wanted && loaded_vault) {
      refilter_wanted = 0;
      int keep = sel >= 0 && sel < nshown ? shown[sel] : -1; // (the card chosen stays chosen)
      int keep_v = vk >= 0 && vk < nshown ? shown[vk] : -1;  // (and the one open in the full view)
      refilter();
      for (int k = 0; k < nshown; k++) { if (shown[k] == keep) sel = k; if (shown[k] == keep_v) vk = k; }
      if (vk >= nshown) vk = nshown ? nshown - 1 : 0;
      if (sel >= nshown) sel = nshown ? nshown - 1 : 0;
    }
    if (screen == S_WEB || screen == S_VIDEOS || screen == S_MANGA) browser_screen = screen;
    { static int was_store; if (store_state == 2 && was_store != 2 && cfg.sort == 10) refilter(); was_store = store_state; }
    // (the header and the tab bar: tucked only while the Library or the Browser scrolls down)
    if (screen != S_LIBRARY && screen != S_WEB && screen != S_SETTINGS) chrome_target = 0;
    chrome_hide += (chrome_target - chrome_hide) * ease(0.25f);
    if (fabsf(chrome_target - chrome_hide) < 0.004f) chrome_hide = chrome_target;
    if (frame_no % 30 == 0) la_poll_events();
    if (wf_run && screen != S_WIFI && screen != S_COVER) wf_stop(); // (the server only while its screen is open)

    // the keyboard's answer
    if (ime_open) {
      static char got[2100]; // (an API key can be long)
      int r = ime_poll(got, sizeof got);
      if (r == 1 && ime_for == 1) { snprintf(password, sizeof password, "%s", got); load_state = 0; screen = S_UNLOCKING; }
      if (r == 1 && ime_for == 3) { char clean[128]; snprintf(cfg.wquery, sizeof cfg.wquery, "%s", lv_booru_clean(got, clean, sizeof clean)); ac_build(cfg.wquery); sheet = SH_NONE; screen = S_WEB; web_search(); }
      if (r == 1 && ime_for == 4 && screen == S_VIEWER && vk < nshown) { if (lv_item_set(&vault, shown[vk], "tags", got) == 0) { edited(); sfx(SFX_DONE); } }
      if (r == 1 && ime_for == 5 && got[0]) saved_add_current(got);
      if (r == 1 && ime_for == 6) batch_add_tag(got);
      if (r == 1 && ime_for == 18) batch_collection(got);
      if (r == 1 && ime_for == 19) mypre_save(got);
      if (r == 1 && ime_for == 7 && got[0]) { snprintf(drive_new_id, sizeof drive_new_id, "%s", got); drive_ask_secret = 1; }
      if (r == 1 && ime_for == 14 && got[0]) add_site_start(got);
      if (r == 1 && ime_for == 20) snprintf(own_tags, sizeof own_tags, "%s", got);   // (your own source's tags)
      if (r == 1 && ime_for == 22 && got[0]) otag_add(got); // (a tag of your own, for a save)
      if (r == 1 && ime_for == 21) snprintf(own_title, sizeof own_title, "%s", got); // (and its name)
      if (r == 1 && ime_for == 15) { snprintf(mg.query, sizeof mg.query, "%s", got); mg_search(); }
      if (r == 1 && ime_for == 17 && got[0]) { snprintf(conv_keys[conv_sel], sizeof conv_keys[conv_sel], "%s", got); conv_keys_save(); sfx(SFX_DONE); say("Key saved"); }
      if (r == 1 && ime_for == 16 && got[0]) { for (char *c = got; *c; c++) if (*c == ',' || *c == '"') *c = ' '; snprintf(sw_cat, sizeof sw_cat, "%.63s", got); sw_catsel = sw_ncats() - 1; }
      if (r == 1 && ime_for == 9) { snprintf(whidden, sizeof whidden, "%s", got); hidden_save(); web_search(); }
      if (r == 1 && ime_for == 10 && got[0]) { snprintf(wkey_user, sizeof wkey_user, "%s", got); wkey_ask = 1; }
      if (r == 1 && ime_for == 11 && got[0]) { snprintf(wkeys[cfg.site], sizeof wkeys[cfg.site], "%s %s", wkey_user, got); wkeys_save(); sfx(SFX_DONE); if (vb_key_src >= 0) { int k = vb_key_src; vb_key_src = -1; screen = S_VIDEOS; vb_open(k); } else web_search(); }
      if (r != 0 && ime_for == 11 && !got[0]) vb_key_src = -1;
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
    // (a screen left: the pictures it had asked for and no other screen shows are dropped, not
    //  fetched and decoded for nothing in the background)
    {
      static int last_screen = -1;
      if (screen != last_screen && last_screen >= 0) {
        int was = last_screen, now = screen;
        int lib_was = was == S_LIBRARY || was == S_VIEWER || was == S_SHOW || was == S_SETTINGS;
        int lib_now = now == S_LIBRARY || now == S_VIEWER || now == S_SHOW || now == S_SETTINGS;
        if (lib_was && !lib_now && loaded_vault) ask_thumbs(NULL, 0);
        if ((was == S_WEB || was == S_WEBVIEW) && now != S_WEB && now != S_WEBVIEW && web_started) ask_previews(NULL, 0);
        if (was == S_MANGA && now != S_MANGA) mg_drop_covers();
        if (was == S_VIDEOS && now != S_VIDEOS) vb_drop_wants();
        if ((was == S_FAVS || was == S_VIDEOS) && now != S_FAVS && now != S_VIDEOS) fav_drop_wants();
        if (shown_custom && now != S_VIEWER) { shown_custom = 0; refilter(); } // (a slideshow of the chosen ones, done)
        // (the slideshow's list on its page and through a slideshow; the Library's own back after)
        if (loaded_vault && now == S_SHOW && !show_pool) { show_pool = 1; refilter(); }
        else if (loaded_vault && show_pool && now != S_SHOW && now != S_VIEWER) {
          int keep = sel >= 0 && sel < nshown ? shown[sel] : -1; // (the Library's choice kept on the same one)
          show_pool = 0; refilter();
          for (int k = 0; k < nshown; k++) if (shown[k] == keep) { sel = k; break; }
          if (sel >= nshown) sel = nshown ? nshown - 1 : 0;
        }
      }
      last_screen = screen;
    }
    janitor(); // (the parts not on screen give their memory back)
    accent_follow();
    // three fingers tapped together: the calculator, from anywhere, at once
    if (in.panic && screen != S_CALC && screen != S_UNLOCKING && screen != S_PICK) {
      if (vid) lv_video_pause(vid, 1);
      if (wv_vid) lv_video_pause(wv_vid, 1);
      slideshow = 0;
      tl_on = 0;
      if (screen == S_COVER) screen = cover_return;
      calc_open(screen);
      memset(&in, 0, sizeof in);
    }
    // SELECT: the cover, from anywhere (and back)
    if (in.pressed & SCE_CTRL_SELECT) {
      if (screen == S_COVER) screen = cover_return;
      // (the calculator: SELECT again gets back, past the PIN pad when there's a PIN)
      else if (screen == S_CALC) { if (pin.on) { int back = calc_return; calc_reset(); pin_lock(back); } else calc_leave(); }
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

    PT("before drawing");
    vita2d_start_drawing();
    // (vita2d starts every frame's vertices at the beginning of one pool, while the GPU may still be
    //  drawing the frame before from it (up to two frames are queued): its shapes came out garbled.
    //  So each frame starts in its own third of the pool, in turn: a third isn't written again until
    //  the frame drawn from it is done. A frame uses ~100 KB of the ~1.3 MB a third has.)
    if (frame_no % 3) vita2d_pool_malloc((frame_no % 3) * (POOL_SIZE / 3));
    vita2d_clear_screen();
    Input otag_keep = in; if (otag_open) { memset(&in, 0, sizeof in); in.now = otag_keep.now; } // (the window over a save: the screen under it gets no input)
    switch (screen) {
      case S_PICK: if (!cfg.onboarded) draw_onboarding(); else draw_pick(); break;
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
      case S_DUPES: draw_dupes(); break;
#ifdef LV_TESTKIT
      case S_TEST: draw_test_menu(); break;
#endif
      case S_WIFI: draw_wifi(); break;
      case S_CALC: draw_calc(); break;
      case S_CLEAN: draw_clean(); break;
      case S_HEALTH: draw_health(); break;
      case S_VIDEOS: draw_videos(); break;
      case S_MANGA: draw_manga(); break;
      case S_BOOK: draw_book(); break;
      case S_READER: draw_reader(); break;
      case S_FAVS: draw_favs(); break;
      case S_WEBVIEW: draw_webview(); break;
      case S_COVER: draw_cover_screen(); if (in.tap) { screen = cover_return; } break;
    }
    if (otag_open) { in = otag_keep; draw_otag(); } // (your tags for a save: over everything, its input its own)
    // the loading screen at start, fading into whatever is behind it once the vault is open
    if (frame_no < splash_until + 24) {
      if (frame_no >= splash_until && (screen == S_UNLOCKING || load_state == 1)) splash_until = frame_no + 1; // (still opening)
      int a = frame_no < splash_until ? 255 : (int)(255 * (splash_until + 24 - frame_no) / 24.0f);
      if (screen != S_LOCKED || frame_no < splash_until) draw_splash(screen == S_LOCKED ? "Untraceable vault" : "Opening the vault\u2026", a);
    }
    if (frame_no < message_until && screen != S_COVER && screen != S_CALC) {
      int w = text_w(16, message) + 44;
      int a = message_until - frame_no < 20 ? (message_until - frame_no) * 12 : 240;
      // (it rises in over a few frames, a glass pill, and fades as it goes)
      float in_t = fminf(1, (frame_no - (message_until - 150)) / 12.0f);
      in_t = 1 - (1 - in_t) * (1 - in_t);
      int ty = H - BAR - 58 + (int)((1 - in_t) * 18);
      a = (int)(a * in_t);
      rrect_round(W / 2 - w / 2, ty + 3, w, 40, 20, RGBA(0, 0, 0, a / 3));
      rrect_round(W / 2 - w / 2, ty, w, 40, 20, with_alpha(mix_col(SURF2, RGBA(0, 0, 0, 255), 0.35f), a * 235 / 255));
      rrect_round(W / 2 - w / 2 + 1, ty, w - 2, 40, 20, with_alpha(TEXT, a / 9));
      rrect_round(W / 2 - w / 2 + 2, ty + 1, w - 4, 38, 19, with_alpha(mix_col(SURF2, ACC, 0.2f), a * 235 / 255));
      rrect_round(W / 2 - w / 2 + 8, ty + 2, w - 16, 15, 7.5f, with_alpha(TEXT, a / 16));
      text_center(W / 2, ty + 26, with_alpha(TEXT, a), 16, message);
    }
    ev_watch();
    sheet_cover_draw();
    speed_poll();
    rear_sign_draw();
    ev_sign_draw();
    PT("the screen");
    if (screen != S_CALC && screen != S_COVER) draw_touch_lock();
#ifdef LV_DEBUG
    // (debug builds: how long each frame's work took, per screen, written to the log every 5 s)
    {
      static uint64_t sum, worst;
      static unsigned n, slow, last_screen = 999, pool_min = 0xFFFFFFFFu;
      unsigned pf = vita2d_pool_free_space();
      if (pf < pool_min) pool_min = pf;
      uint64_t dt = sceKernelGetProcessTimeWide() - perf_t0;
      if ((unsigned)screen != last_screen || n >= 300) {
        if (n && last_screen != 999) {
          struct mallinfo mi = mallinfo(); // (the heap: in use now, and the most it has grown to)
          LOG("perf screen=%u frames=%u avg=%.2fms worst=%.2fms over16ms=%u heap=%.1fMB arena=%.1fMB pool_free_min=%uKB\n", last_screen, n, sum / 1000.0 / n, worst / 1000.0, slow,
              mi.uordblks / 1048576.0, mi.arena / 1048576.0, pool_min / 1024);
        }
        if ((unsigned)screen != last_screen) {
          // (what's held as each screen is left: the heap, and main and video memory outside it, so a
          //  screen's leftovers show as the next one starts)
          struct mallinfo mi = mallinfo();
          int fu = 0, fc = 0;
          mem_free(&fu, &fc);
          LOG("res %u->%u heap=%.1fMB user_free=%.1fMB cdram_free=%.1fMB %s\n", last_screen, (unsigned)screen, mi.uordblks / 1048576.0,
              fu / 1048576.0, fc / 1048576.0, res_extra());
        }
        pool_min = 0xFFFFFFFFu;
        sum = worst = 0; n = slow = 0; last_screen = (unsigned)screen;
      }
      sum += dt; n++;
      if (dt > worst) worst = dt;
      if (dt > 16600) slow++;
    }
#endif
#ifdef LV_TESTKIT
    tk_frame_end((sceKernelGetProcessTimeWide() - frame_t0) / 1000.0f, frame_len_ms);
#endif
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    sceDisplayWaitVblankStartCB(); // (the frame's wait, letting callbacks run: vita2d's own is off)
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
    // (views.csv: every two minutes while it has changed, not every thirty seconds; and when the
    //  viewer closes, a vault is left or the app is put away)
    if (loaded_vault && vault.views_dirty && ++idle_save > 60 * 120) { views_save_async(); idle_save = 0; }
    if ((cfg_dirty_at && frame_no - cfg_dirty_at > 60) || (show_dirty_at && frame_no - show_dirty_at > 60)) flush_settings();
    if (saver_failed) { saver_failed = 0; sfx(SFX_ERROR); say("The changes couldn't be written to the card"); }
  }
  return 0;
}
