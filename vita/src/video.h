#pragma once
#include <stdint.h>
#include <vita2d.h>

typedef struct LvVideo LvVideo;
// MP4/M4V/MOV play in hardware, WebM/MKV (VP8/VP9) in software (video.c).
// key: the vault's (NULL for a normal vault). loop: a preview clip, played round and round.
LvVideo *lv_video_open(const char *path, const uint8_t *key, int loop);
// The newest frame (call once a frame; the texture is the player's own buffer), or NULL so far.
vita2d_texture *lv_video_frame(LvVideo *v);
int lv_video_width(const LvVideo *v);
int lv_video_height(const LvVideo *v);
uint64_t lv_video_pos(const LvVideo *v);      // ms
uint64_t lv_video_duration(const LvVideo *v); // ms, 0 until known
int lv_video_ended(const LvVideo *v);
int lv_video_failed(const LvVideo *v); // (the system player couldn't read the file)
int lv_video_paused(const LvVideo *v);
void lv_video_pause(LvVideo *v, int on);
void lv_video_seek(LvVideo *v, int64_t ms);
void lv_video_set_volume(LvVideo *v, int vol);
void lv_video_close(LvVideo *v);
