#include "sfx.h"
#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRAIN 256 // (5 ms at 48 kHz: a tap is heard at once)
#define VOICES 6

static const char *NAMES[SFX_COUNT] = {"tick", "select", "back", "toggle", "open", "close", "tab", "error", "saved",
                                       "done", "delete", "slideshow", "more", "start"};
// how loud each is next to the others (the ticks are many, so soft)
static const int GAIN[SFX_COUNT] = {55, 80, 75, 70, 70, 70, 75, 85, 90, 85, 85, 80, 60, 85};
// the least time between two of the same, in ms (a fast scroll is a patter, not a buzz)
static const int GAP[SFX_COUNT] = {45, 60, 60, 60, 120, 120, 80, 200, 300, 300, 200, 300, 200, 1000};

typedef struct { int16_t *pcm; int frames, channels; } Clip;
static Clip clips[SFX_COUNT];
typedef struct { int clip, pos; } Voice;
static Voice voices[VOICES];
static SceKernelLwMutexWork mutex_work;
static SceUID wake = -1; // (the mixer sleeps on it while nothing plays; a sound wakes it)
static volatile int volume = 70;
static uint64_t last[SFX_COUNT];
static int ready;

// A WAV's samples (16-bit PCM, its data chunk found by name).
static char pack_dir[48] = "app0:sounds"; // (the pack: the Vita's own, or app0:sounds/<pack>)
static void load_into(Clip *dst, int id);
static void load(int id) { load_into(clips, id); }
static void load_into(Clip *dst, int id) {
  char path[96];
  snprintf(path, sizeof path, "%s/%s.wav", pack_dir, NAMES[id]);
  FILE *f = fopen(path, "rb");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(n);
  if (!b || fread(b, 1, n, f) != (size_t)n || n < 44 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) { free(b); fclose(f); return; }
  fclose(f);
  int channels = 1;
  long at = 12;
  while (at + 8 <= n) {
    uint32_t sz = b[at + 4] | b[at + 5] << 8 | b[at + 6] << 16 | (uint32_t)b[at + 7] << 24;
    if (!memcmp(b + at, "fmt ", 4) && at + 12 <= n) channels = b[at + 10];
    if (!memcmp(b + at, "data", 4)) {
      long len = (long)sz > n - at - 8 ? n - at - 8 : (long)sz;
      dst[id].pcm = malloc(len);
      if (dst[id].pcm) {
        memcpy(dst[id].pcm, b + at + 8, len);
        dst[id].channels = channels == 2 ? 2 : 1;
        dst[id].frames = (int)(len / 2 / dst[id].channels);
      }
      break;
    }
    at += 8 + sz + (sz & 1);
  }
  free(b);
}

static int mixer(SceSize args, void *argp) {
  int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE, GRAIN, 48000, SCE_AUDIO_OUT_MODE_STEREO);
  if (port < 0) return sceKernelExitDeleteThread(0);
  static int16_t out[GRAIN * 2];
  int32_t acc[GRAIN * 2];
  for (;;) {
    memset(acc, 0, sizeof acc);
    int any = 0;
    sceKernelLockLwMutex(&mutex_work, 1, NULL);
    for (int v = 0; v < VOICES; v++) {
      Voice *vo = &voices[v];
      if (vo->clip < 0) continue;
      Clip *c = &clips[vo->clip];
      int g = GAIN[vo->clip] * volume; // (out of 100 * 100)
      for (int i = 0; i < GRAIN && vo->pos < c->frames; i++, vo->pos++) {
        int l = c->pcm[vo->pos * c->channels], r = c->channels == 2 ? c->pcm[vo->pos * 2 + 1] : l;
        acc[i * 2] += l * g / 10000;
        acc[i * 2 + 1] += r * g / 10000;
      }
      if (vo->pos >= c->frames) vo->clip = -1;
      any = 1;
    }
    sceKernelUnlockLwMutex(&mutex_work, 1);
    // (nothing playing: asleep until a sound starts, not waking every few milliseconds to look)
    if (!any) { if (wake >= 0) sceKernelWaitSema(wake, 1, NULL); else sceKernelDelayThread(4000); continue; }
    for (int i = 0; i < GRAIN * 2; i++) out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
    sceAudioOutOutput(port, out);
  }
  return 0;
}

void sfx_init(void) {
  for (int k = 0; k < SFX_COUNT; k++) load(k);
  for (int v = 0; v < VOICES; v++) voices[v].clip = -1;
  sceKernelCreateLwMutex(&mutex_work, "lv_sfx", 0, 0, NULL);
  wake = sceKernelCreateSema("lv_sfx_wake", 0, 0, 1, NULL);
  SceUID t = sceKernelCreateThread("lv_sfx", mixer, 0x10000100 - 10, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
  if (t >= 0) sceKernelStartThread(t, 0, NULL);
  ready = 1;
}
// Another pack: its sounds loaded, the old ones let go (the mixer held off meanwhile). A pack missing a
// sound keeps silence for it.
void sfx_pack(const char *pack) {
  Clip fresh[SFX_COUNT], old[SFX_COUNT];
  memset(fresh, 0, sizeof fresh);
  snprintf(pack_dir, sizeof pack_dir, "%s%s%s", "app0:sounds", pack && pack[0] ? "/" : "", pack ? pack : "");
  for (int k = 0; k < SFX_COUNT; k++) load_into(fresh, k); // (apart: the mixer still plays the old ones)
  if (ready) sceKernelLockLwMutex(&mutex_work, 1, NULL);
  memcpy(old, clips, sizeof clips);
  for (int v = 0; v < VOICES; v++) voices[v].clip = -1;
  memcpy(clips, fresh, sizeof clips);
  if (ready) sceKernelUnlockLwMutex(&mutex_work, 1);
  for (int k = 0; k < SFX_COUNT; k++) free(old[k].pcm);
}
void sfx_volume(int percent) { volume = percent < 0 ? 0 : percent > 100 ? 100 : percent; }

void sfx(int id) {
  if (!ready || id < 0 || id >= SFX_COUNT || !clips[id].pcm || volume == 0) return;
  uint64_t now = sceKernelGetProcessTimeWide() / 1000;
  if (now - last[id] < (uint64_t)GAP[id]) return;
  last[id] = now;
  sceKernelLockLwMutex(&mutex_work, 1, NULL);
  int v = 0, oldest = 0;
  for (; v < VOICES && voices[v].clip >= 0; v++)
    if (voices[v].pos > voices[oldest].pos) oldest = v;
  if (v == VOICES) v = oldest; // (all busy: the one furthest along gives way)
  voices[v].clip = id;
  voices[v].pos = 0;
  sceKernelUnlockLwMutex(&mutex_work, 1);
  if (wake >= 0) sceKernelSignalSema(wake, 1); // (at most 1: an extra signal while it's already awake is refused, harmlessly)
}
