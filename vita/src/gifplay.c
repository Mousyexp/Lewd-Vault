// Animated GIFs in the Browser's full view: gifdec (third_party/gifdec.c, public domain) reading the
// downloaded bytes from memory (its read/lseek/open/close mapped onto them here: a Vita file read per
// byte would be a system call each), frames decoded on a thread of their own, at the GIF's own pace,
// looping. Three textures in turn: the thread writes the one the GPU finished with two frames ago, then
// shows it. Compiled inside main.c (after the texture helpers, before web.c).
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

// ----- gifdec over memory -----
typedef struct { const uint8_t *b; size_t n, pos; int used; } GdMem;
static GdMem gd_mem[4];
static pthread_mutex_t gd_mx = PTHREAD_MUTEX_INITIALIZER;
static int gd_mopen(const void *desc) {
  const GdMem *m = desc;
  pthread_mutex_lock(&gd_mx);
  int fd = -1;
  for (int k = 0; k < 4 && fd < 0; k++) if (!gd_mem[k].used) { gd_mem[k] = *m; gd_mem[k].pos = 0; gd_mem[k].used = 1; fd = k; }
  pthread_mutex_unlock(&gd_mx);
  return fd;
}
static ssize_t gd_mread(int fd, void *buf, size_t n) {
  if (fd < 0 || fd >= 4) return -1;
  GdMem *m = &gd_mem[fd];
  size_t left = m->pos < m->n ? m->n - m->pos : 0;
  if (n > left) { memset((uint8_t *)buf + left, 0, n - left); n = left; } // (past the end: zeros, as an ended GIF)
  memcpy(buf, m->b + m->pos, n);
  m->pos += n;
  return (ssize_t)n;
}
static off_t gd_mseek(int fd, off_t off, int whence) {
  if (fd < 0 || fd >= 4) return -1;
  GdMem *m = &gd_mem[fd];
  off_t at = whence == SEEK_SET ? off : whence == SEEK_CUR ? (off_t)m->pos + off : (off_t)m->n + off;
  if (at < 0) at = 0;
  m->pos = (size_t)at;
  return at;
}
static int gd_mclose(int fd) { if (fd >= 0 && fd < 4) { pthread_mutex_lock(&gd_mx); gd_mem[fd].used = 0; pthread_mutex_unlock(&gd_mx); } return 0; }
#define open(f, fl) gd_mopen((const void *)(f))
#define read(fd, b, n) gd_mread((fd), (b), (n))
#define lseek(fd, o, w) gd_mseek((fd), (o), (w))
#define close(fd) gd_mclose(fd)
#include "third_party/gifdec.c"
#undef open
#undef read
#undef lseek
#undef close

// ----- the player -----
typedef struct {
  uint8_t *bytes;
  GdMem mem;
  gd_GIF *gif;
  vita2d_texture *tex[3];
  uint8_t *rgb;
  volatile int run, front, frames; // (front: the texture to show, -1 none yet)
  int w, h;
  pthread_t th;
  int th_on;
} GifPlay;
static void *gif_main(void *arg) {
  GifPlay *g = arg;
  pin_to_core(2);
  uint64_t due = sceKernelGetProcessTimeWide();
  int back = 0, any = 0;
  while (g->run) {
    int r = gd_get_frame(g->gif);
    if (r == 0) { if (!any) break; gd_rewind(g->gif); continue; } // (the end: round again)
    if (r < 0) break;
    any = 1;
    gd_render_frame(g->gif, g->rgb);
    vita2d_texture *t = g->tex[back];
    uint8_t *d = vita2d_texture_get_datap(t);
    unsigned stride = vita2d_texture_get_stride(t);
    for (int y = 0; y < g->h; y++) {
      const uint8_t *s = g->rgb + (size_t)y * g->w * 3;
      uint8_t *o = d + (size_t)y * stride;
      for (int x = 0; x < g->w; x++) { o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; o += 4; s += 3; }
    }
    // (its own pace: a delay of 0 or 1 hundredths is shown as browsers do, a tenth of a second)
    int cs = g->gif->gce.delay;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (due > now) sceKernelDelayThread((SceUInt)(due - now));
    else if (now - due > 500000) due = now; // (far behind, a pause: from now)
    g->front = back;
    g->frames++;
    back = (back + 1) % 3;
    due += (uint64_t)(cs < 2 ? 10 : cs) * 10000;
  }
  return NULL;
}
// Plays the GIF in bytes (copied); NULL when it isn't one, or is too big for the Vita to animate
static GifPlay *gif_play_open(const uint8_t *bytes, size_t n) {
  if (!bytes || n < 13 || memcmp(bytes, "GIF8", 4)) return NULL;
  int w = bytes[6] | bytes[7] << 8, h = bytes[8] | bytes[9] << 8;
  if (w < 1 || h < 1 || w > 1600 || h > 1600 || (long)w * h > 1600L * 1000) return NULL;
  GifPlay *g = calloc(1, sizeof *g);
  if (!g) return NULL;
  g->bytes = malloc(n);
  g->rgb = malloc((size_t)w * h * 3);
  if (!g->bytes || !g->rgb) goto fail;
  memcpy(g->bytes, bytes, n);
  g->mem.b = g->bytes; g->mem.n = n;
  g->gif = gd_open_gif((const char *)&g->mem);
  if (!g->gif) goto fail;
  g->w = g->gif->width; g->h = g->gif->height;
  for (int k = 0; k < 3; k++) { g->tex[k] = tex_new(g->w, g->h); if (!g->tex[k]) goto fail; }
  g->front = -1; g->run = 1;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 64 * 1024);
  g->th_on = pthread_create(&g->th, &at, gif_main, g) == 0;
  if (!g->th_on) goto fail;
  return g;
fail:
  if (g->gif) gd_close_gif(g->gif);
  for (int k = 0; k < 3; k++) if (g->tex[k]) free_later(g->tex[k]);
  free(g->bytes); free(g->rgb); free(g);
  return NULL;
}
// (the picture to show now: NULL until the first frame is decoded)
static vita2d_texture *gif_play_frame(GifPlay *g) { return g && g->front >= 0 ? g->tex[g->front] : NULL; }
static void gif_play_close(GifPlay *g) {
  if (!g) return;
  g->run = 0;
  if (g->th_on) pthread_join(g->th, NULL);
  gd_close_gif(g->gif);
  for (int k = 0; k < 3; k++) if (g->tex[k]) free_later(g->tex[k]);
  free(g->bytes); free(g->rgb); free(g);
}
