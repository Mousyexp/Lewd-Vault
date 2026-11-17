// Making a video 480p somewhere else (conv.h).
#include "conv.h"
#include "json.h"
#include <stdio.h>
#include <string.h>

static const char *const NAMES[CV_N] = {"CloudConvert", "FreeConvert", "Rendi"};
static const char *const SITES[CV_N] = {"cloudconvert.com", "freeconvert.com", "rendi.dev"};
const char *lv_conv_name(int svc) { return svc >= 0 && svc < CV_N ? NAMES[svc] : ""; }
const char *lv_conv_site(int svc) { return svc >= 0 && svc < CV_N ? SITES[svc] : ""; }

// (a string for inside JSON quotes)
static void jesc(const char *s, char *out, size_t cap) {
  size_t o = 0;
  for (; *s && o + 7 < cap; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
    else if (c < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
    else out[o++] = (char)c;
  }
  out[o] = 0;
}

void lv_conv_start(int svc, const char *key, const char *src, char *url, size_t ucap, char *body, size_t bcap, char hdr[3][300]) {
  char s[2400];
  jesc(src, s, sizeof s);
  for (int k = 0; k < 3; k++) hdr[k][0] = 0;
  snprintf(hdr[1], sizeof hdr[1], "Content-Type: application/json");
  switch (svc) {
    case CV_CLOUDCONVERT:
      snprintf(url, ucap, "https://api.cloudconvert.com/v2/jobs");
      snprintf(hdr[0], sizeof hdr[0], "Authorization: Bearer %s", key);
      snprintf(body, bcap,
               "{\"tasks\":{\"in\":{\"operation\":\"import/url\",\"url\":\"%s\"},"
               "\"conv\":{\"operation\":\"convert\",\"input\":\"in\",\"output_format\":\"mp4\",\"video_codec\":\"x264\","
               "\"width\":854,\"height\":480,\"fit\":\"max\",\"profile\":\"main\",\"level\":\"3.1\",\"crf\":26,\"preset\":\"veryfast\","
               "\"audio_codec\":\"aac\",\"audio_bitrate\":128},"
               "\"out\":{\"operation\":\"export/url\",\"input\":\"conv\"}},\"tag\":\"lewdvault\"}", s);
      break;
    case CV_FREECONVERT:
      snprintf(url, ucap, "https://api.freeconvert.com/v1/process/jobs");
      snprintf(hdr[0], sizeof hdr[0], "Authorization: Bearer %s", key);
      snprintf(body, bcap,
               "{\"tasks\":{\"in\":{\"operation\":\"import/url\",\"url\":\"%s\"},"
               "\"conv\":{\"operation\":\"convert\",\"input\":\"in\",\"input_format\":\"mp4\",\"output_format\":\"mp4\",\"options\":{"
               "\"video_codec\":\"libx264\",\"video_screen_size\":\"854:480\",\"video_fit_screen_size\":\"max\","
               "\"video_encoding_profile_h264\":\"main\",\"video_rate_control_h264\":\"crf\",\"video_crf_h264\":26,\"audio_codec\":\"aac\"}},"
               "\"out\":{\"operation\":\"export/url\",\"input\":[\"conv\"]}}}", s);
      break;
    default: // Rendi: an FFmpeg command run on its machines
      snprintf(url, ucap, "https://api.rendi.dev/v1/run-ffmpeg-command");
      snprintf(hdr[0], sizeof hdr[0], "X-API-KEY: %s", key);
      snprintf(body, bcap,
               "{\"input_files\":{\"in_1\":\"%s\"},\"output_files\":{\"out_1\":\"vita480.mp4\"},"
               "\"ffmpeg_command\":\"-i {{in_1}} -vf scale=w='min(854,iw)':h='min(480,ih)':force_original_aspect_ratio=decrease,"
               "scale=trunc(iw/2)*2:trunc(ih/2)*2 -c:v libx264 -profile:v main -level 3.1 -preset veryfast -crf 26 "
               "-c:a aac -b:a 128k -movflags +faststart {{out_1}}\"}", s);
  }
}

int lv_conv_parse_start(int svc, const char *json, size_t len, char *id, size_t cap) {
  Json j;
  id[0] = 0;
  if (json_parse(&j, json, len) != 0) return -1;
  if (svc == CV_CLOUDCONVERT) json_gs(&j, json_get(&j, 0, "data"), "id", id, cap);
  else if (svc == CV_FREECONVERT) { json_gs(&j, 0, "id", id, cap); if (!id[0]) json_gs(&j, 0, "_id", id, cap); }
  else json_gs(&j, 0, "command_id", id, cap);
  json_free(&j);
  return id[0] ? 0 : -1;
}

void lv_conv_poll_url(int svc, const char *id, char *url, size_t cap) {
  if (svc == CV_CLOUDCONVERT) snprintf(url, cap, "https://api.cloudconvert.com/v2/jobs/%s", id);
  else if (svc == CV_FREECONVERT) snprintf(url, cap, "https://api.freeconvert.com/v1/process/jobs/%s", id);
  else snprintf(url, cap, "https://api.rendi.dev/v1/commands/%s", id);
}

// (the export task's file address, in a list of tasks)
static int export_url(const Json *j, int tasks, char *out, size_t cap) {
  for (int k = 0; tasks >= 0 && k < j->t[tasks].size; k++) {
    int t = json_at(j, tasks, k);
    char op[40];
    json_gs(j, t, "operation", op, sizeof op);
    if (strcmp(op, "export/url")) continue;
    int res = json_get(j, t, "result");
    json_gs(j, res, "url", out, cap);                                        // (FreeConvert)
    if (!out[0]) json_gs(j, json_at(j, json_get(j, res, "files"), 0), "url", out, cap); // (CloudConvert)
    return out[0] ? 0 : -1;
  }
  return -1;
}
int lv_conv_parse_poll(int svc, const char *json, size_t len, char *out, size_t cap) {
  Json j;
  out[0] = 0;
  if (json_parse(&j, json, len) != 0) return -1;
  char st[40] = "";
  int r = 0;
  if (svc == CV_CLOUDCONVERT) {
    int d = json_get(&j, 0, "data");
    json_gs(&j, d, "status", st, sizeof st);
    if (!strcmp(st, "finished")) r = export_url(&j, json_get(&j, d, "tasks"), out, cap) == 0 ? 1 : -1;
    else if (!strcmp(st, "error")) r = -1;
  } else if (svc == CV_FREECONVERT) {
    json_gs(&j, 0, "status", st, sizeof st);
    if (!strcmp(st, "completed")) r = export_url(&j, json_get(&j, 0, "tasks"), out, cap) == 0 ? 1 : -1;
    else if (!strcmp(st, "failed") || !strcmp(st, "cancelled") || !strcmp(st, "deleted")) r = -1;
  } else {
    json_gs(&j, 0, "status", st, sizeof st);
    if (!strcmp(st, "SUCCESS")) { json_gs(&j, json_get(&j, json_get(&j, 0, "output_files"), "out_1"), "storage_url", out, cap); r = out[0] ? 1 : -1; }
    else if (!strcmp(st, "FAILED")) r = -1;
  }
  json_free(&j);
  return r;
}
