#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
// Videos on the Vita, two ways, behind one small API (video.h):
//
//  * MP4 / M4V / MOV: sceAvPlayer, the system's own player, which decodes H.264 in hardware. Set up
//    the way Vita players proven on hardware do it (SonicMastr's Vita Media Player was the
//    reference): the source is added on a thread of its own (it blocks while the file is probed),
//    the streams are enabled and playback started when the player says it is READY, frames live in
//    GPU memory it maps itself, and the sound port is kept fed (silence between frames).
//
//  * H.264 in MKV / WebM / AVI: FFmpeg reads the container and the Vita's own H.264 hardware
//    (sceAvcdec, as Moonlight's Vita port uses it) decodes the picture straight into a texture: the
//    container's length-prefixed packets are turned into start codes here, the stream's SPS and PPS
//    put in front of each key frame. Up to 1920 x 1088.
//
//  * MPEG-4 Part 2 / H.263 (XviD, DivX, old AVIs and 3GPs): FFmpeg's own decoders, in software.
//
//  * WebM / MKV: the Vita has no hardware for VP8/VP9, so they are decoded in software: FFmpeg
//    reads the container (libavformat) and decodes the sound (Opus, Vorbis, AAC), libvpx decodes the
//    picture on its own thread (libvpx's multi-threaded mode hung under test, so one decoder thread),
//    swscale turns it into RGBA at screen size, and the sound's clock paces the picture. Up to about
//    480p plays smoothly; bigger VP9 is more than one Cortex-A9 core decodes in time, so such frames
//    come late (the sound keeps going).
//
// Either way the file is read through our own reader (lvcrypt.h), so an Untraceable vault's videos
// are decrypted as they play and never written out plain.
#include "video.h"
vita2d_texture *tex_new(unsigned w, unsigned h);
#include "lvcrypt.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <malloc.h>
#include <psp2/audioout.h>
#include <psp2/avplayer.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

// (in the stub library, not in the SDK's header)
int sceAvPlayerStreamCount(SceAvPlayerHandle h);
int sceAvPlayerEnableStream(SceAvPlayerHandle h, uint32_t stream);
#define SCE_AVPLAYER_STATE_READY 2

#ifdef LV_DEBUG
#define LOG(...) do { char lb_[256]; int ln_ = snprintf(lb_, sizeof lb_, __VA_ARGS__); SceUID lf_ = sceIoOpen("ux0:data/LWDV00001/log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666); if (lf_ >= 0) { sceIoWrite(lf_, lb_, ln_ > 255 ? 255 : ln_); sceIoClose(lf_); } } while (0)
#else
#define LOG(...) do { } while (0)
#endif

#define GRAIN 1024 // samples per sound block (21 ms at 48 kHz)

struct LvVideo {
  int sw; // 0: sceAvPlayer, 1: the software player
  LvFile lf;
  uint8_t key[32];
  int has_key;
  int loop;
  int volume;
  volatile int run;
  volatile int paused;
  volatile int speed; // percent: 100, 200, 400 (faster plays without sound)
  int w, h;
  uint64_t duration;
  vita2d_texture tex; // (sceAvPlayer: points at its frame)
  vita2d_texture *rgb; // (software: our own texture)
  int has_frame;

  // ---- sceAvPlayer ----
  SceAvPlayerHandle h_av;
  volatile int ready, failed;
  SceUID add_thread, audio_thread;
  char path[1024];

  // ---- software ----
  AVFormatContext *fmt;
  AVIOContext *io;
  int vs, as;
  vpx_codec_ctx_t vpx;
  int vpx_ok;
  AVCodecContext *actx;
  SwrContext *swr;
  struct SwsContext *sws;
  int sws_w, sws_h, sws_fmt;
  pthread_t dec_thread, snd_thread;
  int dec_started, snd_started;
  pthread_mutex_t mx;
  pthread_cond_t cv;
  int16_t *ring; // stereo, 48 kHz
  int ring_cap, ring_r, ring_n;
  volatile int64_t clock_base_ms; // where the sound clock started (a seek moves it)
  volatile uint64_t played;        // samples out since then
  int64_t wall_start;              // (no sound: the wall clock, in microseconds)
  volatile int64_t seek_to;        // -1: none (the system player: a jump waiting for its first picture)
  int av_shown;                    // (the system player: its first picture has come)
  volatile int ended;
  uint8_t *pix[2];
  int pix_w, pix_h;
  volatile int pix_ready; // -1 none, else the buffer to upload
  volatile int64_t pos_ms;
  int64_t io_pos;
  vita2d_texture *rgb2[2];
  int rgb_cur;
  volatile int pix_fresh;
  volatile int snd_done, dec_done;
  // ---- H.264 in hardware (sceAvcdec) ----
  int avc;                   // (the picture's decoded by the Vita's H.264 hardware)
  SceAvcdecCtrl avc_ctl;
  SceUID avc_mem;
  int avc_lib, avc_dec;      // (the library started; the decoder made)
  vita2d_texture *avc_tex[3]; // (three frames: one shown, one the GPU may still read, one decoded into)
  vita2d_texture avc_view[3]; // (each texture's view at the picture's own size, not its padding)
  int avc_w16, avc_h16, avc_back;
  volatile int avc_front;
  uint8_t *avc_hdr; int avc_hdr_n, avc_nal; // (SPS and PPS with start codes; the packets' length size: 0 start codes already)
  uint8_t *avc_buf; size_t avc_cap;
  int64_t avc_pts[32]; int avc_npts; // (the times of the frames sent and not yet out: the earliest is the next shown)
  // ---- other pictures in software (MPEG-4 Part 2, H.263) ----
  AVCodecContext *vctx;
  AVFrame *vframe;
  // ---- a file still being downloaded (lv_video_open_growing): reads wait for its bytes ----
  const LvGrow *grow;
  SceUID gfd;
};

static void release_now(vita2d_texture *t) { vita2d_wait_rendering_done(); vita2d_free_texture(t); }
void (*lv_video_release_texture)(vita2d_texture *t) = release_now;

static int ext(const char *p, const char *e) {
  const char *d = strrchr(p, '.');
  return d && !strcasecmp(d + 1, e);
}

// ======================= sceAvPlayer =======================

static void *av_alloc(void *p, uint32_t align, uint32_t size) { return memalign(align < 16 ? 16 : align, size); }
static void avp_free(void *p, void *ptr) { free(ptr); }
// frames in CDRAM (256 KiB aligned blocks), mapped for the GPU to read
static void *av_alloc_tex(void *p, uint32_t align, uint32_t size) {
  if (align < 0x40000) align = 0x40000;
  size = (size + align - 1) & ~(align - 1);
  SceKernelAllocMemBlockOpt opt;
  memset(&opt, 0, sizeof opt);
  opt.size = sizeof opt;
  opt.attr = 0x00000004U; // (the alignment below applies)
  opt.alignment = align;
  SceUID uid = sceKernelAllocMemBlock("lv_frames", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, &opt);
  if (uid < 0) return NULL;
  void *base = NULL;
  sceKernelGetMemBlockBase(uid, &base);
  if (sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE) < 0) {
    sceKernelFreeMemBlock(uid);
    return NULL;
  }
  return base;
}
static void av_free_tex(void *p, void *ptr) {
  SceUID uid = sceKernelFindMemBlockByAddr(ptr, 0);
  if (uid < 0) return;
  vita2d_wait_rendering_done(); // (the GPU may still be drawing it)
  sceGxmUnmapMemory(ptr);
  sceKernelFreeMemBlock(uid);
}

static int f_open(void *p, const char *name) {
  LvVideo *v = p;
  if (v->grow) { v->gfd = sceIoOpen(name, SCE_O_RDONLY, 0); return v->gfd >= 0 ? 0 : -1; }
  return lv_open(&v->lf, name, v->has_key ? v->key : NULL) == 0 ? 0 : -1;
}
static int f_close(void *p) {
  LvVideo *v = p;
  if (v->grow) { if (v->gfd >= 0) sceIoClose(v->gfd); v->gfd = -1; return 0; }
  lv_close(&v->lf);
  return 0;
}
// (a growing file: wait until the download has the bytes asked for, or has ended, or the player is
//  closing; then read what's there)
static int f_read(void *p, uint8_t *buf, uint64_t pos, uint32_t len) {
  LvVideo *v = p;
  if (!v->grow) return (int)lv_read_at(&v->lf, pos, buf, len);
  const LvGrow *g = v->grow;
  while (v->run && !g->done && !g->failed && (uint64_t)g->have < pos + len) sceKernelDelayThread(20000);
  if (!v->run) return -1;
  int64_t have = g->have;
  if ((int64_t)pos >= have) return 0;
  if ((int64_t)(pos + len) > have) len = (uint32_t)(have - (int64_t)pos);
  int r = sceIoPread(v->gfd, buf, len, (SceOff)pos);
  return r < 0 ? -1 : r;
}
static uint64_t f_size(void *p) {
  LvVideo *v = p;
  if (!v->grow) return v->lf.len;
  // (the whole size, from the download's answer; until it's known, what's here)
  return v->grow->total > 0 ? (uint64_t)v->grow->total : (uint64_t)v->grow->have;
}

static void av_event(void *p, int32_t id, int32_t src, void *data) {
  LvVideo *v = p;
  if (id == SCE_AVPLAYER_STATE_READY) {
    int n = sceAvPlayerStreamCount(v->h_av);
    for (int k = 0; k < n; k++) {
      SceAvPlayerStreamInfo si;
      if (sceAvPlayerGetStreamInfo(v->h_av, k, &si) != 0) continue;
      if (si.type == SCE_AVPLAYER_VIDEO) { sceAvPlayerEnableStream(v->h_av, k); v->duration = si.duration; }
      else if (si.type == SCE_AVPLAYER_AUDIO) sceAvPlayerEnableStream(v->h_av, k);
    }
    sceAvPlayerStart(v->h_av);
    v->ready = 1;
  }
}

static int av_add_main(SceSize args, void *argp) {
  sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << 1); // (off the drawing thread's core)
  LvVideo *v = *(LvVideo **)argp;
  if (sceAvPlayerAddSource(v->h_av, v->path) < 0) v->failed = 1;
  v->dec_done = 1;
  return sceKernelExitDeleteThread(0);
}

// (the volume: the port at full, the samples scaled here, so it can go past it (up to 150%), clipped)
static void gain_into(int16_t *dst, const int16_t *src, int n, int vol) {
  for (int k = 0; k < n; k++) {
    int x = (int)src[k] * vol / SCE_AUDIO_VOLUME_0DB;
    dst[k] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
  }
}
static int av_sound_main(SceSize args, void *argp) {
  sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << 2); // (off the drawing thread's core)
  LvVideo *v = *(LvVideo **)argp;
  int rate = 48000, ch = 2;
  int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, rate, SCE_AUDIO_OUT_MODE_STEREO);
  int16_t *silence = memalign(64, GRAIN * 2 * sizeof(int16_t));
  memset(silence, 0, GRAIN * 2 * sizeof(int16_t));
  int16_t *gbuf = memalign(64, GRAIN * 2 * sizeof(int16_t));
  if (port >= 0) { int lr[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB}; sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, lr); }
  while (v->run) {
    SceAvPlayerFrameInfo f;
    if (v->ready && !v->paused && sceAvPlayerIsActive(v->h_av) && sceAvPlayerGetAudioData(v->h_av, &f)) {
      int r = (int)f.details.audio.sampleRate, c = f.details.audio.channelCount;
      if (port >= 0 && c && r && (r != rate || c != ch)) {
        rate = r; ch = c;
        sceAudioOutSetConfig(port, GRAIN, rate, ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
      }
      // (a frame can hold more than one block, 2048 samples for HE-AAC: all of it played, a block at a
      //  time; a short tail made whole with silence. Only its first block played before: the sound
      //  stuttered and "echoed" on long videos)
      int total = f.details.audio.size && ch ? (int)(f.details.audio.size / (2 * ch)) : GRAIN;
      const int16_t *src = (const int16_t *)f.pData;
      for (int off = 0; port >= 0 && gbuf && off < total; off += GRAIN) {
        int nsm = total - off < GRAIN ? total - off : GRAIN;
        gain_into(gbuf, src + off * ch, nsm * ch, v->volume);
        if (nsm < GRAIN) memset(gbuf + nsm * ch, 0, (GRAIN - nsm) * ch * sizeof(int16_t));
        sceAudioOutOutput(port, gbuf);
      }
      if (port >= 0 && !gbuf) sceAudioOutOutput(port, f.pData);
    } else if (port >= 0) {
      sceAudioOutOutput(port, silence); // (keeps the port's timing; also waits a block)
    } else {
      sceKernelDelayThread(10000);
    }
  }
  if (port >= 0) { sceAudioOutOutput(port, NULL); sceAudioOutReleasePort(port); }
  free(silence);
  free(gbuf);
  v->snd_done = 1;
  return sceKernelExitDeleteThread(0);
}

static int av_open(LvVideo *v) {
  static int loaded;
  if (!loaded) { sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER); loaded = 1; }
  SceAvPlayerInitData init;
  memset(&init, 0, sizeof init);
  init.memoryReplacement.objectPointer = v;
  init.memoryReplacement.allocate = av_alloc;
  init.memoryReplacement.deallocate = avp_free;
  init.memoryReplacement.allocateTexture = av_alloc_tex;
  init.memoryReplacement.deallocateTexture = av_free_tex;
  init.fileReplacement.objectPointer = v;
  init.fileReplacement.open = f_open;
  init.fileReplacement.close = f_close;
  init.fileReplacement.readOffset = f_read;
  init.fileReplacement.size = f_size;
  init.eventReplacement.objectPointer = v;
  init.eventReplacement.eventCallback = av_event;
  init.basePriority = 125;
  init.numOutputVideoFrameBuffers = 6;
  init.autoStart = SCE_FALSE;
  v->h_av = sceAvPlayerInit(&init);
  // (the handle is an address in the player's memory, 0x81000000 up: negative as an int; an error is
  //  one of Sony's codes, 0x80xxxxxx, or none at all)
  if (v->h_av == 0 || ((unsigned)v->h_av >> 24) == 0x80) { LOG("av: init failed %08x\n", (unsigned)v->h_av); v->h_av = 0; return -1; }
  sceAvPlayerSetLooping(v->h_av, v->loop ? SCE_TRUE : SCE_FALSE);
  v->add_thread = sceKernelCreateThread("lv_av_add", av_add_main, 0x10000100, 0x10000, 0, 0, NULL); // (64 KB: the system player reads the file's header on it)
  if (v->add_thread >= 0) sceKernelStartThread(v->add_thread, sizeof v, &v);
  else if (sceAvPlayerAddSource(v->h_av, v->path) < 0) return -1;
  v->audio_thread = sceKernelCreateThread("lv_av_sound", av_sound_main, 0x10000100, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
  if (v->audio_thread >= 0) sceKernelStartThread(v->audio_thread, sizeof v, &v);
  return 0;
}

static vita2d_texture *av_frame(LvVideo *v) {
  SceAvPlayerFrameInfo f;
  // (a frame with no size, or an absurd one, is skipped: drawn, its scale would be infinite and the
  //  GPU would get NaN corners: garbage over the whole screen)
  if (v->ready && sceAvPlayerIsActive(v->h_av) && sceAvPlayerGetVideoData(v->h_av, &f) && f.pData &&
      f.details.video.width >= 16 && f.details.video.height >= 16 && f.details.video.width <= 4096 && f.details.video.height <= 4096) {
    v->w = (int)f.details.video.width;
    v->h = (int)f.details.video.height;
    if (!v->av_shown) { v->av_shown = 1; if (v->seek_to > 0) { sceAvPlayerJumpToTime(v->h_av, (uint64_t)v->seek_to); } v->seek_to = -1; }
    sceGxmTextureInitLinear(&v->tex.gxm_tex, f.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, v->w, v->h, 0);
    sceGxmTextureSetMinFilter(&v->tex.gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(&v->tex.gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
    v->has_frame = 1;
  }
  return v->has_frame ? &v->tex : NULL;
}

// ======================= the software player =======================

static int io_read(void *p, uint8_t *buf, int n) {
  LvVideo *v = p;
  size_t got = lv_read_at(&v->lf, (uint64_t)v->io_pos, buf, (size_t)n);
  v->io_pos += (int64_t)got;
  return got ? (int)got : AVERROR_EOF;
}
static int64_t io_seek(void *p, int64_t off, int whence) {
  LvVideo *v = p;
  if (whence & AVSEEK_SIZE) return (int64_t)v->lf.len;
  whence &= ~AVSEEK_FORCE;
  int64_t to = whence == SEEK_SET ? off : whence == SEEK_CUR ? v->io_pos + off : (int64_t)v->lf.len + off;
  if (to < 0 || to > (int64_t)v->lf.len) return -1;
  v->io_pos = to;
  return to;
}

static int64_t now_us(void) { return (int64_t)sceKernelGetProcessTimeWide(); }
static int64_t sw_clock(LvVideo *v) {
  if (v->as >= 0 && v->speed == 100) return v->clock_base_ms + (int64_t)(v->played * 1000 / 48000);
  return v->clock_base_ms + (now_us() - v->wall_start) * v->speed / 100000;
}

static void ring_put(LvVideo *v, const int16_t *s, int frames) {
  pthread_mutex_lock(&v->mx);
  while (frames > 0 && v->run && v->seek_to < 0) {
    while (v->ring_n == v->ring_cap && v->run && v->seek_to < 0) pthread_cond_wait(&v->cv, &v->mx);
    int room = v->ring_cap - v->ring_n;
    int t = frames < room ? frames : room;
    for (int k = 0; k < t; k++) {
      int at = (v->ring_r + v->ring_n + k) % v->ring_cap;
      v->ring[at * 2] = s[k * 2];
      v->ring[at * 2 + 1] = s[k * 2 + 1];
    }
    v->ring_n += t;
    s += t * 2;
    frames -= t;
  }
  pthread_mutex_unlock(&v->mx);
}

static void *sw_sound_main(void *arg) {
  sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << 2); // (off the drawing thread's core)
  LvVideo *v = arg;
  int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, 48000, SCE_AUDIO_OUT_MODE_STEREO);
  int16_t *blk = memalign(64, GRAIN * 2 * sizeof(int16_t));
  int16_t *gbuf = memalign(64, GRAIN * 2 * sizeof(int16_t));
  if (port >= 0) { int lr[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB}; sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, lr); }
  while (v->run) {
    int real = 0;
    pthread_mutex_lock(&v->mx);
    if (!v->paused && v->ring_n >= GRAIN) {
      for (int k = 0; k < GRAIN; k++) {
        int at = (v->ring_r + k) % v->ring_cap;
        blk[k * 2] = v->ring[at * 2];
        blk[k * 2 + 1] = v->ring[at * 2 + 1];
      }
      v->ring_r = (v->ring_r + GRAIN) % v->ring_cap;
      v->ring_n -= GRAIN;
      real = 1;
      pthread_cond_broadcast(&v->cv);
    }
    pthread_mutex_unlock(&v->mx);
    if (!real) memset(blk, 0, GRAIN * 2 * sizeof(int16_t));
    if (real && gbuf) { gain_into(gbuf, blk, GRAIN * 2, v->volume); memcpy(blk, gbuf, GRAIN * 2 * sizeof(int16_t)); }
    if (port >= 0) sceAudioOutOutput(port, blk);
    else sceKernelDelayThread(21000);
    if (real) v->played += GRAIN;
  }
  if (port >= 0) { sceAudioOutOutput(port, NULL); sceAudioOutReleasePort(port); }
  free(blk);
  free(gbuf);
  v->snd_done = 1;
  return NULL;
}

static void sw_show_planes(LvVideo *v, int iw, int ih, int f, const uint8_t *const planes[3], const int strides[3]);
static void sw_show(LvVideo *v, vpx_image_t *img) {
  int f = img->fmt == VPX_IMG_FMT_I444 ? AV_PIX_FMT_YUV444P : img->fmt == VPX_IMG_FMT_I422 ? AV_PIX_FMT_YUV422P : AV_PIX_FMT_YUV420P;
  const uint8_t *const planes[3] = {img->planes[0], img->planes[1], img->planes[2]};
  const int strides[3] = {img->stride[0], img->stride[1], img->stride[2]};
  sw_show_planes(v, (int)img->d_w, (int)img->d_h, f, planes, strides);
}
static void sw_show_planes(LvVideo *v, int iw, int ih, int f, const uint8_t *const planes[3], const int strides[3]) {
  // made 480p (854 x 480 at most) before it's drawn: a bigger frame is scaled down here, once, and the
  // GPU takes it from there to the screen
  int ow = iw, oh = ih;
  if (ow > 854 || oh > 480) {
    double s = ow * 480.0 > oh * 854.0 ? 854.0 / ow : 480.0 / oh;
    ow = ((int)(ow * s)) & ~1;
    oh = ((int)(oh * s)) & ~1;
  }
  if (!v->sws || v->sws_w != iw || v->sws_h != ih || v->sws_fmt != f || v->pix_w != ow || v->pix_h != oh) {
    if (v->sws) sws_freeContext(v->sws);
    v->sws = sws_getContext(iw, ih, f, ow, oh, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    v->sws_w = iw; v->sws_h = ih; v->sws_fmt = f;
    if (v->pix_w != ow || v->pix_h != oh) {
      pthread_mutex_lock(&v->mx);
      free(v->pix[0]); free(v->pix[1]);
      v->pix[0] = memalign(64, (size_t)ow * oh * 4);
      v->pix[1] = memalign(64, (size_t)ow * oh * 4);
      v->pix_w = ow; v->pix_h = oh;
      v->pix_ready = -1;
      pthread_mutex_unlock(&v->mx);
    }
  }
  if (!v->sws || !v->pix[0]) return;
  int back = v->pix_ready == 0 ? 1 : 0;
  uint8_t *dst[1] = {v->pix[back]};
  int dst_stride[1] = {ow * 4};
  sws_scale(v->sws, planes, strides, 0, ih, dst, dst_stride);
  pthread_mutex_lock(&v->mx);
  v->pix_ready = back;
  v->pix_fresh = 1;
  pthread_mutex_unlock(&v->mx);
}

static void sw_seek_now(LvVideo *v) {
  int64_t to = v->seek_to;
  av_seek_frame(v->fmt, -1, to * 1000, AVSEEK_FLAG_BACKWARD);
  if (v->actx) avcodec_flush_buffers(v->actx);
  pthread_mutex_lock(&v->mx);
  v->ring_n = 0;
  v->ring_r = 0;
  v->played = 0;
  v->clock_base_ms = to;
  v->wall_start = now_us();
  v->avc_npts = 0;
  v->seek_to = -1;
  pthread_cond_broadcast(&v->cv);
  pthread_mutex_unlock(&v->mx);
}

// ---------- H.264 in hardware ----------
// (the stream's SPS and PPS from the container's avcC box, as start-code NAL units)
static int avc_parse_extra(LvVideo *v, const uint8_t *ex, int n) {
  if (n < 7 || ex[0] != 1) { v->avc_nal = 0; return 0; } // (no avcC: start codes already, or nothing)
  v->avc_nal = (ex[4] & 3) + 1;
  v->avc_hdr = malloc((size_t)n * 2 + 64);
  if (!v->avc_hdr) return -1;
  int at = 5, o = 0;
  for (int set = 0; set < 2; set++) {
    if (at >= n) break;
    int cnt = set == 0 ? ex[at++] & 0x1f : ex[at++];
    for (int k = 0; k < cnt && at + 2 <= n; k++) {
      int l = ex[at] << 8 | ex[at + 1];
      at += 2;
      if (at + l > n) break;
      memcpy(v->avc_hdr + o, "\0\0\0\1", 4); o += 4;
      memcpy(v->avc_hdr + o, ex + at, (size_t)l); o += l;
      at += l;
    }
  }
  v->avc_hdr_n = o;
  return 0;
}
// a packet as start-code NAL units (the SPS and PPS first, on a key frame); its length
static size_t avc_annexb(LvVideo *v, const AVPacket *pkt) {
  size_t need = (size_t)pkt->size * 2 + (size_t)v->avc_hdr_n + 64;
  if (need > v->avc_cap) { uint8_t *nb = realloc(v->avc_buf, need); if (!nb) return 0; v->avc_buf = nb; v->avc_cap = need; }
  size_t o = 0;
  if ((pkt->flags & AV_PKT_FLAG_KEY) && v->avc_hdr_n) { memcpy(v->avc_buf, v->avc_hdr, (size_t)v->avc_hdr_n); o = (size_t)v->avc_hdr_n; }
  if (!v->avc_nal) { memcpy(v->avc_buf + o, pkt->data, (size_t)pkt->size); return o + (size_t)pkt->size; }
  for (int at = 0; at + v->avc_nal <= pkt->size;) {
    uint32_t l = 0;
    for (int k = 0; k < v->avc_nal; k++) l = l << 8 | pkt->data[at + k];
    at += v->avc_nal;
    if (l > (uint32_t)(pkt->size - at)) break;
    memcpy(v->avc_buf + o, "\0\0\0\1", 4); o += 4;
    memcpy(v->avc_buf + o, pkt->data + at, l); o += l;
    at += (int)l;
  }
  return o;
}
static int avc_open(LvVideo *v, AVCodecParameters *cp) {
  if (cp->width <= 0 || cp->height <= 0 || cp->width > 1920 || cp->height > 1088) return -1;
  if (avc_parse_extra(v, cp->extradata, cp->extradata_size) != 0) return -1;
  sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
  v->avc_w16 = (cp->width + 15) & ~15;
  v->avc_h16 = (cp->height + 15) & ~15;
  SceVideodecQueryInitInfoHwAvcdec init = {sizeof init, (uint32_t)v->avc_w16, (uint32_t)v->avc_h16, 5, 1};
  if (sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init) < 0) { LOG("avc: init failed\n"); return -1; }
  v->avc_lib = 1;
  SceAvcdecQueryDecoderInfo q = {(uint32_t)v->avc_w16, (uint32_t)v->avc_h16, 5};
  SceAvcdecDecoderInfo di;
  memset(&di, 0, sizeof di);
  if (sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &di) < 0) return -1;
  uint32_t sz = (di.frameMemSize + 0x3FFFF) & ~0x3FFFFu;
  v->avc_mem = sceKernelAllocMemBlock("lv_avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, sz, NULL);
  if (v->avc_mem < 0) { LOG("avc: no memory for the decoder (%u)\n", (unsigned)sz); return -1; }
  memset(&v->avc_ctl, 0, sizeof v->avc_ctl);
  sceKernelGetMemBlockBase(v->avc_mem, &v->avc_ctl.frameBuf.pBuf);
  v->avc_ctl.frameBuf.size = sz;
  if (sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &v->avc_ctl, &q) < 0) { LOG("avc: decoder not made\n"); return -1; }
  v->avc_dec = 1;
  // (the frames: the hardware writes into video memory the GPU then draws)
  for (int k = 0; k < 3; k++) {
    v->avc_tex[k] = vita2d_create_empty_texture(v->avc_w16, v->avc_h16);
    if (!v->avc_tex[k]) return -1;
    vita2d_texture *view = &v->avc_view[k];
    memset(view, 0, sizeof *view);
    sceGxmTextureInitLinearStrided(&view->gxm_tex, vita2d_texture_get_datap(v->avc_tex[k]), SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, cp->width, cp->height, v->avc_w16 * 4);
    sceGxmTextureSetMinFilter(&view->gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(&view->gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
  }
  v->avc_front = -1; v->avc_back = 0;
  v->avc = 1;
  LOG("avc: %dx%d in hardware, nal %d, header %d bytes\n", cp->width, cp->height, v->avc_nal, v->avc_hdr_n);
  return 0;
}
static void avc_close(LvVideo *v) {
  if (v->avc_dec) sceAvcdecDeleteDecoder(&v->avc_ctl);
  if (v->avc_mem > 0) sceKernelFreeMemBlock(v->avc_mem);
  if (v->avc_lib) sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
  for (int k = 0; k < 3; k++) if (v->avc_tex[k]) lv_video_release_texture(v->avc_tex[k]);
  free(v->avc_hdr); free(v->avc_buf);
  v->avc_dec = v->avc_lib = 0; v->avc_mem = 0;
}
// waits for a picture's moment on the sound's clock (gives up on a seek or a pause)
static void sw_wait_for(LvVideo *v, int64_t pts) {
  while (v->run && v->seek_to < 0 && !v->paused) {
    int64_t ahead = pts - sw_clock(v);
    if (ahead <= 4) break;
    sceKernelDelayThread((SceUInt)((ahead > 20 ? 20 : ahead) * 1000));
  }
}
static void *sw_decode_main(void *arg) {
  sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << 1); // (off the drawing thread's core)
  LvVideo *v = arg;
  AVPacket *pkt = av_packet_alloc();
  AVFrame *af = av_frame_alloc();
  int16_t *pcm = malloc(48000 * 2 * sizeof(int16_t));
  int64_t skip_until = -1;
  while (v->run) {
    if (v->seek_to >= 0) { skip_until = v->seek_to; sw_seek_now(v); }
    if (v->paused) {
      // (the wall clock stops with the picture)
      int64_t t = sw_clock(v);
      while (v->paused && v->run && v->seek_to < 0) sceKernelDelayThread(40000); // (paused: a look 25 times a second is plenty)
      v->clock_base_ms = t;
      v->wall_start = now_us();
      v->played = 0;
      continue;
    }
    int r = av_read_frame(v->fmt, pkt);
    if (r < 0) {
      if (v->loop) { v->seek_to = 0; continue; }
      v->ended = 1;
      while (v->run && v->seek_to < 0) sceKernelDelayThread(50000); // (at the end: waiting for a seek, unhurried)
      v->ended = 0;
      continue;
    }
    if (pkt->stream_index == v->as && v->actx && v->speed == 100) {
      if (avcodec_send_packet(v->actx, pkt) == 0) {
        while (avcodec_receive_frame(v->actx, af) == 0) {
          int n = swr_convert(v->swr, (uint8_t **)&pcm, 48000, (const uint8_t **)af->extended_data, af->nb_samples);
          if (n > 0) ring_put(v, pcm, n);
        }
      }
    } else if (pkt->stream_index == v->vs && v->avc) {
      AVRational tb = v->fmt->streams[v->vs]->time_base;
      // (its time: its own, else its decoding time (AVI has only that), else a frame on from the last)
      static int64_t last_in;
      int64_t pts = pkt->pts != AV_NOPTS_VALUE ? av_rescale_q(pkt->pts, tb, (AVRational){1, 1000})
                  : pkt->dts != AV_NOPTS_VALUE ? av_rescale_q(pkt->dts, tb, (AVRational){1, 1000}) : last_in + 33;
      last_in = pts;
      size_t n = avc_annexb(v, pkt);
      if (n) {
        SceAvcdecAu au;
        memset(&au, 0, sizeof au);
        au.pts.upper = 0; au.pts.lower = (uint32_t)(pts < 0 ? 0 : pts);
        au.dts.upper = au.dts.lower = 0xFFFFFFFF;
        au.es.pBuf = v->avc_buf; au.es.size = (uint32_t)n;
        int back = v->avc_back;
        SceAvcdecPicture pic;
        memset(&pic, 0, sizeof pic);
        pic.size = sizeof pic;
        pic.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
        pic.frame.framePitch = (uint32_t)v->avc_w16;
        pic.frame.frameWidth = (uint32_t)v->avc_w16;
        pic.frame.frameHeight = (uint32_t)v->avc_h16;
        pic.frame.pPicture[0] = vita2d_texture_get_datap(v->avc_tex[back]);
        SceAvcdecPicture *pp = &pic;
        SceAvcdecArrayPicture arr = {0, 1, &pp};
        if (v->avc_npts < 32) v->avc_pts[v->avc_npts++] = pts;
        int r = sceAvcdecDecode(&v->avc_ctl, &au, &arr);
        if (r < 0) LOG("avc: decode error %x at %lld\n", r, (long long)pts);
        else if (arr.numOfOutput > 0) {
          // (its time: the earliest of those sent and not yet out, as frames come out in the order
          //  they're shown; the decoder's own isn't always filled in)
          int64_t opts = pts;
          if (v->avc_npts) {
            int m = 0;
            for (int q = 1; q < v->avc_npts; q++) if (v->avc_pts[q] < v->avc_pts[m]) m = q;
            opts = v->avc_pts[m];
            v->avc_pts[m] = v->avc_pts[--v->avc_npts];
          }
          if (!(skip_until >= 0 && opts < skip_until - 40)) {
            skip_until = -1;
            sw_wait_for(v, opts);
            if (v->seek_to >= 0 || !v->run) { av_packet_unref(pkt); continue; }
            v->pos_ms = opts;
            pthread_mutex_lock(&v->mx);
            v->avc_front = back;
            v->avc_back = (back + 1) % 3; // (never the one shown, nor the one shown before it)
            pthread_mutex_unlock(&v->mx);
          }
        }
      }
    } else if (pkt->stream_index == v->vs && v->vctx) {
      if (avcodec_send_packet(v->vctx, pkt) == 0) {
        while (avcodec_receive_frame(v->vctx, v->vframe) == 0) {
          AVRational tb = v->fmt->streams[v->vs]->time_base;
          int64_t bt = v->vframe->best_effort_timestamp;
          int64_t pts = bt != AV_NOPTS_VALUE ? av_rescale_q(bt, tb, (AVRational){1, 1000}) : v->pos_ms;
          if (skip_until >= 0 && pts < skip_until - 40) continue;
          skip_until = -1;
          sw_wait_for(v, pts);
          if (v->seek_to >= 0 || !v->run) break;
          v->pos_ms = pts;
          const uint8_t *const planes[3] = {v->vframe->data[0], v->vframe->data[1], v->vframe->data[2]};
          const int strides[3] = {v->vframe->linesize[0], v->vframe->linesize[1], v->vframe->linesize[2]};
          sw_show_planes(v, v->vframe->width, v->vframe->height, v->vframe->format, planes, strides);
        }
      }
    } else if (pkt->stream_index == v->vs && v->vpx_ok) {
      AVRational tb = v->fmt->streams[v->vs]->time_base;
      int64_t pts = pkt->pts != AV_NOPTS_VALUE ? av_rescale_q(pkt->pts, tb, (AVRational){1, 1000}) : v->pos_ms;
      vpx_codec_err_t de = vpx_codec_decode(&v->vpx, pkt->data, (unsigned)pkt->size, NULL, 0);
      if (de != VPX_CODEC_OK) LOG("sw: vpx error %d at %lld\n", (int)de, (long long)pts);
      if (de == VPX_CODEC_OK) {
        vpx_codec_iter_t it = NULL;
        vpx_image_t *img;
        while ((img = vpx_codec_get_frame(&v->vpx, &it))) {
          if (skip_until >= 0 && pts < skip_until - 40) continue; // (decoding up to where a seek landed)
          skip_until = -1;
          // wait for its moment on the sound's clock (and give up waiting on a seek or a pause)
          while (v->run && v->seek_to < 0 && !v->paused) {
            int64_t ahead = pts - sw_clock(v);
            if (ahead <= 4) break;
            sceKernelDelayThread((SceUInt)((ahead > 20 ? 20 : ahead) * 1000));
          }
          if (v->seek_to >= 0 || !v->run) break;
          v->pos_ms = pts;
          sw_show(v, img);
        }
      }
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  av_frame_free(&af);
  free(pcm);
  v->dec_done = 1;
  return NULL;
}

static int sw_open(LvVideo *v) {
  if (lv_open(&v->lf, v->path, v->has_key ? v->key : NULL) != 0) return -1;
  pthread_mutex_init(&v->mx, NULL);
  pthread_cond_init(&v->cv, NULL);
  v->vs = v->as = -1;
  v->seek_to = -1;
  v->pix_ready = -1;
  uint8_t *iobuf = av_malloc(64 * 1024);
  v->io = avio_alloc_context(iobuf, 64 * 1024, 0, v, io_read, NULL, io_seek);
  v->fmt = avformat_alloc_context();
  v->fmt->pb = v->io;
  LOG("sw: opening %s (%llu bytes)\n", v->path, (unsigned long long)v->lf.len);
  if (avformat_open_input(&v->fmt, NULL, NULL, NULL) < 0) { LOG("sw: open_input failed\n"); v->fmt = NULL; return -1; }
  LOG("sw: open_input ok\n");
  // (no avformat_find_stream_info: WebM/MKV headers already give the codecs, sizes and duration, and
  // probing would decode frames with decoders this FFmpeg doesn't have)
  LOG("sw: %u streams\n", v->fmt->nb_streams);
  if (v->fmt->duration > 0) v->duration = (uint64_t)(v->fmt->duration / 1000);
  for (unsigned k = 0; k < v->fmt->nb_streams; k++) {
    AVCodecParameters *cp = v->fmt->streams[k]->codecpar;
    if (cp->codec_type == AVMEDIA_TYPE_VIDEO && v->vs < 0 && (cp->codec_id == AV_CODEC_ID_VP9 || cp->codec_id == AV_CODEC_ID_VP8)) {
      vpx_codec_dec_cfg_t cfg = {.threads = 1, .w = (unsigned)cp->width, .h = (unsigned)cp->height};
      vpx_codec_iface_t *ifc = cp->codec_id == AV_CODEC_ID_VP9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx();
      if (vpx_codec_dec_init(&v->vpx, ifc, &cfg, 0) == VPX_CODEC_OK) {
        v->vpx_ok = 1;
        v->vs = (int)k;
        v->w = cp->width;
        v->h = cp->height;
      }
    } else if (cp->codec_type == AVMEDIA_TYPE_VIDEO && v->vs < 0 && cp->codec_id == AV_CODEC_ID_H264) {
      // (H.264: the Vita's own hardware)
      if (avc_open(v, cp) == 0) { v->vs = (int)k; v->w = cp->width; v->h = cp->height; v->pix_w = v->w; v->pix_h = v->h; }
      else LOG("sw: H.264 %dx%d: not in hardware\n", cp->width, cp->height);
    } else if (cp->codec_type == AVMEDIA_TYPE_VIDEO && v->vs < 0 && avcodec_find_decoder(cp->codec_id)) {
      // (MPEG-4 Part 2, H.263 and the like: FFmpeg's own decoder)
      const AVCodec *c = avcodec_find_decoder(cp->codec_id);
      v->vctx = avcodec_alloc_context3(c);
      if (v->vctx && avcodec_parameters_to_context(v->vctx, cp) >= 0 && avcodec_open2(v->vctx, c, NULL) >= 0 && (v->vframe = av_frame_alloc())) { v->vs = (int)k; v->w = cp->width; v->h = cp->height; }
      else if (v->vctx) avcodec_free_context(&v->vctx);
    } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO && v->as < 0) {
      const AVCodec *c = avcodec_find_decoder(cp->codec_id);
      if (!c) continue;
      v->actx = avcodec_alloc_context3(c);
      if (avcodec_parameters_to_context(v->actx, cp) < 0 || avcodec_open2(v->actx, c, NULL) < 0) { avcodec_free_context(&v->actx); continue; }
      AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
      if (swr_alloc_set_opts2(&v->swr, &stereo, AV_SAMPLE_FMT_S16, 48000, &v->actx->ch_layout, v->actx->sample_fmt, v->actx->sample_rate, 0, NULL) < 0 ||
          swr_init(v->swr) < 0) {
        avcodec_free_context(&v->actx);
        continue;
      }
      v->as = (int)k;
    }
  }
  LOG("sw: video %d audio %d %dx%d\n", v->vs, v->as, v->w, v->h);
  if (v->vs < 0) return -1; // (no picture we can decode: H.264 in MKV, AV1, ...)
  v->ring_cap = 48000; // one second of sound ahead
  v->ring = malloc((size_t)v->ring_cap * 2 * sizeof(int16_t));
  v->wall_start = now_us();
  v->run = 1;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024);
  v->dec_started = pthread_create(&v->dec_thread, &at, sw_decode_main, v) == 0;
  v->snd_started = pthread_create(&v->snd_thread, &at, sw_sound_main, v) == 0;
  return v->dec_started ? 0 : -1;
}

// Waits up to `ms` for a thread to say it's done.
static int wait_flag(volatile int *flag, int ms) {
  for (int t = 0; t < ms && !*flag; t += 5) sceKernelDelayThread(5000);
  return *flag;
}

// 1 when everything was let go; 0 when a thread didn't stop in time (the sound output can block):
// then nothing it might still touch is freed, and the caller keeps the rest too.
static int sw_close(LvVideo *v) {
  v->run = 0;
  if (v->ring) { pthread_mutex_lock(&v->mx); pthread_cond_broadcast(&v->cv); pthread_mutex_unlock(&v->mx); }
  int dec_ok = !v->dec_started || wait_flag(&v->dec_done, 1500);
  int snd_ok = !v->snd_started || wait_flag(&v->snd_done, 500);
  if (v->dec_started && dec_ok) pthread_join(v->dec_thread, NULL);
  if (v->snd_started && snd_ok) pthread_join(v->snd_thread, NULL);
  LOG("sw_close dec %d snd %d\n", dec_ok, snd_ok);
  if (!dec_ok || !snd_ok) {
    if (v->dec_started && !dec_ok) pthread_detach(v->dec_thread);
    if (v->snd_started && !snd_ok) pthread_detach(v->snd_thread);
    for (int k = 0; k < 2; k++) if (v->rgb2[k]) lv_video_release_texture(v->rgb2[k]);
    return 0;
  }
  if (v->vpx_ok) vpx_codec_destroy(&v->vpx);
  avc_close(v);
  if (v->vctx) avcodec_free_context(&v->vctx);
  if (v->vframe) av_frame_free(&v->vframe);
  if (v->actx) avcodec_free_context(&v->actx);
  if (v->swr) swr_free(&v->swr);
  if (v->sws) sws_freeContext(v->sws);
  if (v->fmt) avformat_close_input(&v->fmt);
  if (v->io) { av_freep(&v->io->buffer); avio_context_free(&v->io); }
  lv_close(&v->lf);
  free(v->ring);
  free(v->pix[0]);
  free(v->pix[1]);
  for (int k = 0; k < 2; k++) if (v->rgb2[k]) lv_video_release_texture(v->rgb2[k]);
  return 1;
}

// A new picture goes into whichever of two textures wasn't drawn last (the GPU may still be reading
// that one), and only when the decoder has a new one.
static vita2d_texture *sw_frame(LvVideo *v) {
  if (v->avc) { // (the hardware's frame, as it is: nothing copied)
    pthread_mutex_lock(&v->mx);
    int f = v->avc_front;
    pthread_mutex_unlock(&v->mx);
    if (f < 0) return NULL;
    v->has_frame = 1;
    v->rgb = &v->avc_view[f];
    return v->rgb;
  }
  pthread_mutex_lock(&v->mx);
  int k = v->pix_ready;
  if (k >= 0 && v->pix[k] && v->pix_fresh) {
    int t = v->rgb_cur ^ 1;
    vita2d_texture **tex = &v->rgb2[t];
    if (!*tex || (int)vita2d_texture_get_width(*tex) != v->pix_w || (int)vita2d_texture_get_height(*tex) != v->pix_h) {
      if (*tex) lv_video_release_texture(*tex);
      *tex = tex_new(v->pix_w, v->pix_h); // (main.c: video memory, else main memory)
      if (*tex) vita2d_texture_set_filters(*tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    }
    if (*tex) {
      uint8_t *d = vita2d_texture_get_datap(*tex);
      unsigned stride = vita2d_texture_get_stride(*tex);
      for (int y = 0; y < v->pix_h; y++) memcpy(d + (size_t)y * stride, v->pix[k] + (size_t)y * v->pix_w * 4, (size_t)v->pix_w * 4);
      v->rgb_cur = t;
      v->rgb = *tex;
      v->has_frame = 1;
    }
    v->pix_fresh = 0;
  }
  pthread_mutex_unlock(&v->mx);
  return v->has_frame ? v->rgb : NULL;
}

// ======================= the API =======================

LvVideo *lv_video_open_growing(const char *path, const LvGrow *g) {
  if (ext(path, "webm") || ext(path, "mkv")) return NULL; // (the system player's MP4 only)
  LvVideo *v = calloc(1, sizeof *v);
  if (!v) return NULL;
  snprintf(v->path, sizeof v->path, "%s", path);
  v->grow = g;
  v->gfd = -1;
  v->volume = SCE_AUDIO_VOLUME_0DB;
  v->speed = 100;
  v->add_thread = v->audio_thread = -1;
  v->run = 1;
  v->seek_to = -1;
  if (av_open(v) != 0) { lv_video_close(v); return NULL; }
  return v;
}
LvVideo *lv_video_open(const char *path, const uint8_t *key, int loop) {
  LvVideo *v = calloc(1, sizeof *v);
  if (!v) return NULL;
  snprintf(v->path, sizeof v->path, "%s", path);
  if (key) { memcpy(v->key, key, 32); v->has_key = 1; }
  v->loop = loop;
  v->volume = SCE_AUDIO_VOLUME_0DB;
  v->speed = 100;
  v->add_thread = v->audio_thread = -1;
  v->run = 1;
  v->seek_to = -1;
  v->sw = ext(path, "webm") || ext(path, "mkv") || ext(path, "avi");
  if ((v->sw ? sw_open(v) : av_open(v)) != 0) { lv_video_close(v); return NULL; }
  return v;
}

vita2d_texture *lv_video_frame(LvVideo *v) {
  if (!v) return NULL;
  return v->sw ? sw_frame(v) : av_frame(v);
}
int lv_video_width(const LvVideo *v) { return v ? (v->sw && v->rgb ? v->pix_w : v->w) : 0; }
int lv_video_height(const LvVideo *v) { return v ? (v->sw && v->rgb ? v->pix_h : v->h) : 0; }
int lv_video_failed(const LvVideo *v) { return v && !v->sw && v->failed; }
uint64_t lv_video_pos(const LvVideo *v) {
  if (!v) return 0;
  if (v->sw) return (uint64_t)(v->pos_ms > 0 ? v->pos_ms : 0);
  return v->ready ? sceAvPlayerCurrentTime(v->h_av) : 0;
}
uint64_t lv_video_duration(const LvVideo *v) { return v ? v->duration : 0; }
int lv_video_ended(const LvVideo *v) {
  if (!v) return 0;
  if (v->sw) return v->ended;
  return v->ready && v->has_frame && !sceAvPlayerIsActive(v->h_av);
}
int lv_video_paused(const LvVideo *v) { return v && v->paused; }
void lv_video_pause(LvVideo *v, int on) {
  if (!v || on == v->paused) return;
  v->paused = on;
  if (!v->sw && v->ready) { if (on) sceAvPlayerPause(v->h_av); else sceAvPlayerResume(v->h_av); }
}
void lv_video_seek(LvVideo *v, int64_t ms) {
  if (!v) return;
  if (v->duration && ms > (int64_t)v->duration - 500) ms = (int64_t)v->duration - 500;
  if (ms < 0) ms = 0;
  if (v->sw) { v->seek_to = ms; pthread_mutex_lock(&v->mx); pthread_cond_broadcast(&v->cv); pthread_mutex_unlock(&v->mx); }
  else if (v->ready && v->av_shown) sceAvPlayerJumpToTime(v->h_av, (uint64_t)ms);
  else v->seek_to = ms; // (asked before the system player shows anything: done once its first picture is here)
}
void lv_video_set_volume(LvVideo *v, int vol) { if (v) v->volume = vol; }
int lv_video_speed(const LvVideo *v) { return v ? v->speed : 100; }
void lv_video_set_speed(LvVideo *v, int pct) {
  if (!v || pct == v->speed || (pct != 100 && pct != 200 && pct != 400)) return;
  if (!v->sw) {
    if (v->ready) sceAvPlayerSetTrickSpeed(v->h_av, pct);
    v->speed = pct;
    return;
  }
  // the clock goes on from where it is, now at the new pace (on the wall clock while it's fast: the
  // sound stops, as the system player's does)
  pthread_mutex_lock(&v->mx);
  int64_t t = sw_clock(v);
  v->ring_n = 0;
  v->ring_r = 0;
  v->played = 0;
  v->clock_base_ms = t;
  v->wall_start = now_us();
  v->speed = pct;
  pthread_cond_broadcast(&v->cv);
  pthread_mutex_unlock(&v->mx);
}

void lv_video_close(LvVideo *v) {
  if (!v) return;
  if (v->sw) {
    if (!sw_close(v)) return; // (a thread still has it: kept, a small leak rather than a crash)
  } else {
    v->run = 0;
    int ok = 1;
    if (v->audio_thread >= 0) ok &= wait_flag(&v->snd_done, 500);
    if (v->add_thread >= 0) ok &= wait_flag(&v->dec_done, 2000);
    if (v->h_av != 0) { sceAvPlayerStop(v->h_av); sceAvPlayerClose(v->h_av); } // (an address: negative as an int)
    if (!ok) return;
    lv_close(&v->lf);
  }
  free(v);
}
