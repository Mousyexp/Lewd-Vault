#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
// Google Drive on the Vita (drive.h): src-tauri/src/gdrive.rs in C, with the sign-in Google offers to
// devices without a keyboard: the Vita shows a short code and a QR code, you open google.com/device on
// a phone (the QR code), type the code and allow it; the Vita then gets its own token.
//
// What's the same as the PC: the scope (drive.file: only the files Lewd Vault made), the "Lewd Vault"
// folder in My Drive ("Lewd Vault (encrypted)" for an Untraceable vault), the paths in it, what
// syncs (not _app/cache, _app/.incoming, _app/backups, *.part or conflict copies), and the rules: a
// change on one side goes to the other; a file is only deleted on one side when the other deleted it
// and it hadn't changed since the last sync, never on the first sync; a sync that would delete more
// than half of one side (and over 20 files) stops; when both changed, the Vita's copy wins and
// Drive's goes beside it as "<name> (from Drive)<ext>" (index.csv and views.csv are then folded in,
// vault.c lv_vault_merge_conflicts). An encrypted vault in Drive must have the same password.
//
// The key: drive.file lets a key see the files made by its own Google Cloud project, so the Vita needs
// a client of the PC's project; Google's device sign-in takes only "TVs and Limited Input devices"
// clients, so it's that kind (docs/GUIDE.md). A build can carry one (LEWD_VAULT_GOOGLE_TV_CLIENT_ID /
// _SECRET), or it's typed in once. Kept in ux0:data/LWDV00001/drive/: account.txt (the key and the
// sign-in; never in the vault) and one manifest per vault (each file as it was at the last sync).
// Files deleted here by a sync go to ux0:data/LWDV00001/drive/trash, out of the vault.
#include "drive.h"
#include "json.h"
#include "vault.h"
#include "net.h"
#include <dirent.h>
#include <errno.h>
#include <psp2/io/devctl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define DIRP "ux0:data/LWDV00001/drive"
#define SCOPE "https://www.googleapis.com/auth/drive.file"
// Google's addresses (a debug build can be pointed at a stand-in: debug_drive.txt, tests/drive_mock.py)
static char DEVICE_URL[200] = "https://oauth2.googleapis.com/device/code", TOKEN_URL[200] = "https://oauth2.googleapis.com/token",
            REVOKE_URL[200] = "https://oauth2.googleapis.com/revoke", FILES[200] = "https://www.googleapis.com/drive/v3/files",
            UPLOAD[200] = "https://www.googleapis.com/upload/drive/v3/files";
#define FOLDER_MIME "application/vnd.google-apps.folder"
#define CONFLICT " (from Drive)"
#define CHUNK (8u * 1024 * 1024) // (a multiple of 256 KB, as resumable uploads want)

#ifdef LV_DEBUG
#include <psp2/io/fcntl.h>
#define LOG(...) do { char lb_[256]; int ln_ = snprintf(lb_, sizeof lb_, __VA_ARGS__); SceUID lf_ = sceIoOpen("ux0:data/LWDV00001/log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666); if (lf_ >= 0) { sceIoWrite(lf_, lb_, ln_ > 255 ? 255 : ln_); sceIoClose(lf_); } } while (0)
#else
#define LOG(...) do { } while (0)
#endif

DriveStatus drive;
static struct { char id[160], secret[80], refresh[300], last[40]; } acct;
static char token[2100];
static uint64_t token_until;
static volatile int cancel;
static pthread_mutex_t amx = PTHREAD_MUTEX_INITIALIZER; // (acct, token)
static LvHttp *http;

static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }
static void say(const char *m) { snprintf(drive.message, sizeof drive.message, "%s", m); }

// ---------- the account ----------
static void save_account(void) {
  sceIoMkdir(DIRP, 0777);
  FILE *f = fopen(DIRP "/account.txt.part", "w");
  if (!f) return;
  fprintf(f, "%s\n%s\n%s\n%s\n", acct.id, acct.secret, acct.refresh, acct.last);
  fclose(f);
  sceIoRemove(DIRP "/account.txt");
  sceIoRename(DIRP "/account.txt.part", DIRP "/account.txt");
}
static void line(FILE *f, char *out, size_t cap) {
  out[0] = 0;
  if (!fgets(out, (int)cap, f)) return;
  out[strcspn(out, "\r\n")] = 0;
}
void drive_load(void) {
#ifdef LV_DEBUG
  FILE *dbg = fopen("ux0:data/LWDV00001/debug_drive.txt", "r");
  if (dbg) {
    char base[150] = "";
    line(dbg, base, sizeof base);
    fclose(dbg);
    if (base[0]) {
      snprintf(DEVICE_URL, sizeof DEVICE_URL, "%s/device/code", base);
      snprintf(TOKEN_URL, sizeof TOKEN_URL, "%s/token", base);
      snprintf(REVOKE_URL, sizeof REVOKE_URL, "%s/revoke", base);
      snprintf(FILES, sizeof FILES, "%s/drive/v3/files", base);
      snprintf(UPLOAD, sizeof UPLOAD, "%s/upload/drive/v3/files", base);
    }
  }
#endif
  FILE *f = fopen(DIRP "/account.txt", "r");
  if (f) {
    line(f, acct.id, sizeof acct.id);
    line(f, acct.secret, sizeof acct.secret);
    line(f, acct.refresh, sizeof acct.refresh);
    line(f, acct.last, sizeof acct.last);
    fclose(f);
  }
#if defined(LV_GOOGLE_TV_ID) && defined(LV_GOOGLE_TV_SECRET)
  if (!acct.id[0]) { snprintf(acct.id, sizeof acct.id, "%s", LV_GOOGLE_TV_ID); snprintf(acct.secret, sizeof acct.secret, "%s", LV_GOOGLE_TV_SECRET); }
#endif
  // (or a key put on the card by hand: ux0:data/LWDV00001/google.txt, the Client ID then the secret)
  if (!acct.id[0] && (f = fopen("ux0:data/LWDV00001/google.txt", "r"))) {
    line(f, acct.id, sizeof acct.id);
    line(f, acct.secret, sizeof acct.secret);
    fclose(f);
    if (acct.id[0]) save_account();
  }
}
int drive_has_key(void) { return acct.id[0] != 0; }
int drive_signed_in(void) { return acct.refresh[0] != 0; }
const char *drive_key_id(void) { return acct.id; }
const char *drive_last_sync(void) { return acct.last; }
void drive_set_key(const char *id, const char *secret) {
  pthread_mutex_lock(&amx);
  if (strcmp(acct.id, id)) acct.refresh[0] = 0; // (another key: signed out)
  snprintf(acct.id, sizeof acct.id, "%s", id);
  snprintf(acct.secret, sizeof acct.secret, "%s", secret);
  token[0] = 0;
  save_account();
  pthread_mutex_unlock(&amx);
}
void drive_cancel(void) { cancel = 1; }

// ---------- small pieces ----------
static void form_add(char *o, size_t cap, const char *k, const char *v) {
  size_t n = strlen(o);
  n += snprintf(o + n, n < cap ? cap - n : 0, "%s%s=", n ? "&" : "", k);
  for (const unsigned char *p = (const unsigned char *)v; *p && n + 4 < cap; p++) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p)) o[n++] = *p;
    else n += snprintf(o + n, cap - n, "%%%02X", *p);
  }
  o[n < cap ? n : cap - 1] = 0;
}
static void url_enc(const char *v, char *o, size_t cap) { o[0] = 0; char t[1200] = ""; form_add(t, sizeof t, "x", v); snprintf(o, cap, "%s", t + 2); }
static void json_esc(const char *s, char *o, size_t cap) {
  size_t n = 0;
  for (; *s && n + 7 < cap; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = c; }
    else if (c < 32) n += snprintf(o + n, cap - n, "\\u%04x", c);
    else o[n++] = c;
  }
  o[n] = 0;
}
static int64_t free_room(const char *root) {
  char dev[16];
  const char *c = strchr(root, ':');
  if (!c || c - root > 10) return -1;
  snprintf(dev, sizeof dev, "%.*s", (int)(c - root + 1), root);
  SceIoDevInfo info;
  memset(&info, 0, sizeof info);
  if (sceIoDevctl(dev, 0x3001, NULL, 0, &info, sizeof info) < 0) return -1;
  return (int64_t)info.free_size;
}

// MD5 (RFC 1321), for "is it the same file?" on a first sync
typedef struct { uint32_t a, b, c, d; uint64_t n; uint8_t buf[64]; } Md5;
static const uint32_t MK[64] = {
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
  0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
  0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
  0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
  0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
  0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const uint8_t MR[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                               4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
static void md5_block(Md5 *m, const uint8_t *p) {
  uint32_t w[16], a = m->a, b = m->b, c = m->c, d = m->d;
  for (int i = 0; i < 16; i++) w[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
  for (int i = 0; i < 64; i++) {
    uint32_t f;
    int g;
    if (i < 16) { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
    else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
    else { f = c ^ (b | ~d); g = (7 * i) & 15; }
    uint32_t t = d;
    d = c;
    c = b;
    uint32_t x = a + f + MK[i] + w[g];
    b = b + (x << MR[i] | x >> (32 - MR[i]));
    a = t;
  }
  m->a += a; m->b += b; m->c += c; m->d += d;
}
static void md5_init(Md5 *m) { m->a = 0x67452301; m->b = 0xefcdab89; m->c = 0x98badcfe; m->d = 0x10325476; m->n = 0; }
static void md5_add(Md5 *m, const uint8_t *p, size_t n) {
  size_t have = m->n & 63;
  m->n += n;
  if (have) {
    size_t t = 64 - have < n ? 64 - have : n;
    memcpy(m->buf + have, p, t);
    p += t; n -= t;
    if (have + t < 64) return;
    md5_block(m, m->buf);
  }
  for (; n >= 64; p += 64, n -= 64) md5_block(m, p);
  memcpy(m->buf, p, n);
}
static void md5_hex(Md5 *m, char out[33]) {
  uint64_t bits = m->n * 8;
  uint8_t pad = 0x80, z = 0;
  md5_add(m, &pad, 1);
  while ((m->n & 63) != 56) md5_add(m, &z, 1);
  uint8_t l[8];
  for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (8 * i));
  md5_add(m, l, 8);
  uint32_t v[4] = {m->a, m->b, m->c, m->d};
  for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", (v[i / 4] >> (8 * (i % 4))) & 255);
}
static int file_md5(const char *path, char out[33]) {
  FILE *f = fopen(path, "rb");
  out[0] = 0;
  if (!f) return -1;
  Md5 m;
  md5_init(&m);
  static uint8_t buf[256 * 1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0 && !cancel) md5_add(&m, buf, n);
  fclose(f);
  if (cancel) return -1;
  md5_hex(&m, out);
  return 0;
}

// ---------- Google's answers ----------
static int post_form(const char *url, const char *form, Json *j, uint8_t **raw, const char **why) {
  static const char *const hs[] = {"Content-Type: application/x-www-form-urlencoded", NULL};
  LvReq rq = {.method = "POST", .headers = hs, .body = form, .body_len = strlen(form)};
  size_t n;
  int st = lv_http_req(http, url, &rq, raw, &n, 1 << 20, NULL, why);
  if (st < 0) return st;
  if (json_parse(j, (const char *)*raw, n) != 0) { free(*raw); *raw = NULL; return -1; }
  return st;
}

// A fresh token from the sign-in (they last an hour). 0, or -1 (*why says; signed out: "signed out").
static int refresh_token(const char **why) {
  if (!acct.refresh[0]) { if (why) *why = "Sign in to Google Drive first"; return -1; }
  pthread_mutex_lock(&amx);
  if (token[0] && now_us() < token_until) { pthread_mutex_unlock(&amx); return 0; }
  char form[1200] = "";
  form_add(form, sizeof form, "client_id", acct.id);
  form_add(form, sizeof form, "client_secret", acct.secret);
  form_add(form, sizeof form, "refresh_token", acct.refresh);
  form_add(form, sizeof form, "grant_type", "refresh_token");
  pthread_mutex_unlock(&amx);
  Json j;
  uint8_t *raw = NULL;
  int st = post_form(TOKEN_URL, form, &j, &raw, why);
  if (st < 0) { if (!raw && why && !*why) *why = "No answer from Google"; return -1; }
  char t[2100], err[64];
  json_gs(&j, 0, "access_token", t, sizeof t);
  json_gs(&j, 0, "error", err, sizeof err);
  double life = json_gn(&j, 0, "expires_in");
  json_free(&j);
  free(raw);
  if (!t[0]) {
    if (!strcmp(err, "invalid_grant")) {
      pthread_mutex_lock(&amx);
      acct.refresh[0] = 0;
      save_account();
      pthread_mutex_unlock(&amx);
      if (why) *why = "Google signed you out: sign in again";
    } else if (why) *why = err[0] ? "Google refused the key: check it" : "No answer from Google";
    return -1;
  }
  pthread_mutex_lock(&amx);
  snprintf(token, sizeof token, "%s", t);
  token_until = now_us() + (uint64_t)((life > 120 ? life - 60 : 60) * 1000000.0);
  pthread_mutex_unlock(&amx);
  return 0;
}

// A call to Drive with the token (a fresh one once if it's turned down). Returns the status.
static int call(const char *url, LvReq *rq, uint8_t **out, size_t *n, const char **why) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (refresh_token(why) != 0) return -1;
    char auth[2200];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
    const char *hs[8];
    int k = 0;
    hs[k++] = auth;
    for (const char *const *x = rq ? rq->headers : NULL; x && *x && k < 7; x++) hs[k++] = *x;
    hs[k] = NULL;
    LvReq r2 = rq ? *rq : (LvReq){0};
    r2.headers = hs;
    int st = lv_http_req(http, url, &r2, out, n, 64u << 20, &cancel, why);
    if (st == 401) { free(*out); *out = NULL; pthread_mutex_lock(&amx); token[0] = 0; pthread_mutex_unlock(&amx); continue; }
    if ((st == 429 || st >= 500 || st < 0) && attempt < 2 && !cancel) { free(*out); *out = NULL; sceKernelDelayThread(1500000 << attempt); continue; }
    if (st == 403 && *out && strstr((char *)*out, "storageQuotaExceeded")) *why = "Your Google Drive is full";
    return st;
  }
  return -1;
}
static int call_json(const char *url, LvReq *rq, Json *j, uint8_t **raw, const char **why) {
  size_t n = 0;
  int st = call(url, rq, raw, &n, why);
  if (st < 200 || st >= 300) { free(*raw); *raw = NULL; return st < 0 ? -1 : -st; }
  if (json_parse(j, (const char *)*raw, n) != 0) { free(*raw); *raw = NULL; *why = "Drive's answer couldn't be read"; return -1; }
  return 0;
}

// ---------- signing in: the device code ----------
static void *sign_in_main(void *arg) {
  sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << 2); // (off the drawing thread's core)
  (void)arg;
  if (!http) http = lv_http_new();
  const char *why = NULL;
  char form[600] = "";
  form_add(form, sizeof form, "client_id", acct.id);
  form_add(form, sizeof form, "scope", SCOPE);
  Json j;
  uint8_t *raw = NULL;
  int st = post_form(DEVICE_URL, form, &j, &raw, &why);
  if (st != 200) {
    char e[80] = "";
    if (raw) { json_gs(&j, 0, "error", e, sizeof e); json_free(&j); free(raw); }
    snprintf(drive.message, sizeof drive.message, "%s", !strcmp(e, "invalid_client") ? "Google doesn't know this key: check the Client ID"
             : !strcmp(e, "unauthorized_client") || !strcmp(e, "invalid_scope") ? "This key isn't for TVs and limited input devices (docs/GUIDE.md)"
             : why && *why ? why : "No answer from Google");
    drive.state = DRV_FAILED;
    return NULL;
  }
  char device[300];
  json_gs(&j, 0, "device_code", device, sizeof device);
  json_gs(&j, 0, "user_code", drive.user_code, sizeof drive.user_code);
  json_gs(&j, 0, "verification_url", drive.url, sizeof drive.url);
  if (!drive.url[0]) json_gs(&j, 0, "verification_uri", drive.url, sizeof drive.url);
  double life = json_gn(&j, 0, "expires_in"), every = json_gn(&j, 0, "interval");
  json_free(&j);
  free(raw);
  if (every < 5) every = 5;
  drive.code_until = now_us() + (uint64_t)((life > 0 ? life : 1800) * 1000000.0);
  drive.state = DRV_CODE;
  // asking now and then whether it's been allowed
  while (!cancel && now_us() < drive.code_until) {
    for (int k = 0; k < (int)(every * 10) && !cancel; k++) sceKernelDelayThread(100000);
    if (cancel) break;
    char f2[1200] = "";
    form_add(f2, sizeof f2, "client_id", acct.id);
    form_add(f2, sizeof f2, "client_secret", acct.secret);
    form_add(f2, sizeof f2, "device_code", device);
    form_add(f2, sizeof f2, "grant_type", "urn:ietf:params:oauth:grant-type:device_code");
    raw = NULL;
    st = post_form(TOKEN_URL, f2, &j, &raw, &why);
    if (!raw) continue; // (no network for a moment)
    char e[64], rt[300], at[2100];
    json_gs(&j, 0, "error", e, sizeof e);
    json_gs(&j, 0, "refresh_token", rt, sizeof rt);
    json_gs(&j, 0, "access_token", at, sizeof at);
    double alife = json_gn(&j, 0, "expires_in");
    json_free(&j);
    free(raw);
    if (rt[0]) {
      pthread_mutex_lock(&amx);
      snprintf(acct.refresh, sizeof acct.refresh, "%s", rt);
      snprintf(token, sizeof token, "%s", at);
      token_until = now_us() + (uint64_t)((alife > 120 ? alife - 60 : 60) * 1000000.0);
      save_account();
      pthread_mutex_unlock(&amx);
      say("Signed in to Google Drive");
      drive.state = DRV_DONE;
      return NULL;
    }
    if (!strcmp(e, "slow_down")) every += 5;
    else if (!strcmp(e, "access_denied")) { say("It wasn't allowed on the phone"); drive.state = DRV_FAILED; return NULL; }
    else if (!strcmp(e, "expired_token")) break;
    else if (e[0] && strcmp(e, "authorization_pending")) { snprintf(drive.message, sizeof drive.message, "Google said: %s", e); drive.state = DRV_FAILED; return NULL; }
  }
  say(cancel ? "Sign-in stopped" : "The code ran out: try again");
  drive.state = DRV_FAILED;
  return NULL;
}
static pthread_t worker;
static int worker_started;
static volatile int worker_done;
static void *(*job_fn)(void *);
// (a job: its work, then its connection to Google let go: no socket kept open between syncs)
static void *job_main(void *arg) {
  void *r = job_fn(arg);
  if (http) lv_http_drop(http);
  worker_done = 1;
  return r;
}
static int start_worker(void *(*fn)(void *), void *arg) {
  if (worker_started) { pthread_join(worker, NULL); worker_started = 0; }
  cancel = 0;
  worker_done = 0;
  job_fn = fn;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 256 * 1024);
  worker_started = pthread_create(&worker, &at, job_main, arg) == 0;
  return worker_started ? 0 : -1;
}
void drive_reap(void) {
  if (worker_started && worker_done) { pthread_join(worker, NULL); worker_started = 0; } // (its stack back)
}
void drive_sign_in(void) {
  if (drive.state == DRV_CODE || drive.state == DRV_SYNCING) return;
  memset((void *)&drive, 0, sizeof drive);
  drive.signing = 1;
  drive.state = DRV_SYNCING; // (busy: until the code is there)
  say("Asking Google for a code…");
  if (start_worker(sign_in_main, NULL) != 0) { say("The sign-in couldn't start"); drive.state = DRV_FAILED; }
}
void drive_sign_out(void) {
  if (drive.state == DRV_CODE || drive.state == DRV_SYNCING) return;
  pthread_mutex_lock(&amx);
  // (Google forgets the sign-in too; with no network it lapses by itself)
  if (acct.refresh[0]) {
    if (!http) http = lv_http_new();
    char form[400] = "";
    form_add(form, sizeof form, "token", acct.refresh);
    static const char *const hs[] = {"Content-Type: application/x-www-form-urlencoded", NULL};
    LvReq rq = {.method = "POST", .headers = hs, .body = form, .body_len = strlen(form)};
    uint8_t *o = NULL;
    size_t n;
    const char *why;
    lv_http_req(http, REVOKE_URL, &rq, &o, &n, 65536, NULL, &why);
    free(o);
  }
  acct.refresh[0] = 0;
  acct.last[0] = 0;
  token[0] = 0;
  save_account();
  pthread_mutex_unlock(&amx);
  say("Signed out of Google Drive");
}

// ---------- string → number maps (paths, Drive ids) ----------
typedef struct { char **k; int *v; int cap, n; } Map;
static unsigned hash(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
static int map_init(Map *m, int n) {
  m->cap = 64;
  while (m->cap < n * 2 + 2) m->cap *= 2;
  m->k = calloc(m->cap, sizeof *m->k);
  m->v = calloc(m->cap, sizeof *m->v);
  m->n = 0;
  return m->k && m->v ? 0 : -1;
}
static int map_get(const Map *m, const char *k) {
  if (!m->cap) return -1;
  for (unsigned i = hash(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
    if (!m->k[i]) return -1;
    if (!strcmp(m->k[i], k)) return m->v[i];
  }
}
// (the key is the caller's, kept as it is)
static int map_put(Map *m, char *k, int v) {
  if ((m->n + 1) * 2 > m->cap) {
    Map b;
    if (map_init(&b, m->cap) != 0) return -1;
    for (int i = 0; i < m->cap; i++) if (m->k[i]) map_put(&b, m->k[i], m->v[i]);
    free(m->k); free(m->v);
    *m = b;
  }
  for (unsigned i = hash(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
    if (!m->k[i]) { m->k[i] = k; m->v[i] = v; m->n++; return 0; }
    if (!strcmp(m->k[i], k)) return 0; // (the first one wins)
  }
}
static void map_free(Map *m) { free(m->k); free(m->v); memset(m, 0, sizeof *m); }

// ---------- the vault on the card ----------
typedef struct { char *path; int64_t size, mtime; } Local;
typedef struct { char *path, *id, *md5; int64_t size; } Remote;
typedef struct { char *path, *id, *md5; int64_t size, mtime; } Synced;
typedef struct { Local *l; int nl, capl; Remote *r; int nr, capr; Synced *m; int nm, capm; char root_id[80]; } Plan;

static int ok_path(const char *rel) {
  if (!strcmp(rel, "_app/cache") || !strncmp(rel, "_app/cache/", 11) || !strncmp(rel, "_app/.incoming", 14) || !strncmp(rel, "_app/backups", 12)) return 0;
  size_t n = strlen(rel);
  if (n >= 5 && !strcmp(rel + n - 5, ".part")) return 0;
  if (strstr(rel, CONFLICT) || strstr(rel, ".drive.part")) return 0;
  for (const char *p = rel; p; p = strchr(p, '/')) {
    if (*p == '/') p++;
    if (!strncmp(p, ".lewd-vault-probe", 17) || !strncasecmp(p, "desktop.ini", 11)) return 0;
  }
  return 1;
}
static int64_t mtime_of(const struct stat *st) { return (int64_t)st->st_mtime * 1000; }
static void walk(const char *root, const char *rel, Plan *pl, int depth) {
  if (depth > 12) return;
  char dir[1100];
  snprintf(dir, sizeof dir, "%s%s%s", root, rel[0] ? "/" : "", rel);
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) && !cancel) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char r2[900], full[1300];
    snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
    if (!ok_path(r2)) continue;
    snprintf(full, sizeof full, "%s/%s", root, r2);
    struct stat st;
    if (stat(full, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) { walk(root, r2, pl, depth + 1); continue; }
    if (pl->nl == pl->capl) {
      int c = pl->capl ? pl->capl * 2 : 256;
      Local *nl = realloc(pl->l, c * sizeof *nl);
      if (!nl) continue;
      pl->l = nl;
      pl->capl = c;
    }
    char *p = strdup(r2);
    if (!p) continue;
    pl->l[pl->nl++] = (Local){p, st.st_size, mtime_of(&st)};
  }
  closedir(d);
}

static void manifest_path(const char *root, char *o, size_t cap) {
  unsigned h = hash(root);
  snprintf(o, cap, DIRP "/manifest-%08x.txt", h);
}
static void manifest_load(const char *root, Plan *pl) {
  char p[200];
  manifest_path(root, p, sizeof p);
  FILE *f = fopen(p, "r");
  if (!f) return;
  char ln[1400];
  if (fgets(ln, sizeof ln, f)) { ln[strcspn(ln, "\r\n")] = 0; snprintf(pl->root_id, sizeof pl->root_id, "%s", ln); }
  while (fgets(ln, sizeof ln, f)) {
    ln[strcspn(ln, "\r\n")] = 0;
    char *c[5];
    int n = 0;
    c[n++] = ln;
    for (char *t = ln; *t && n < 5; t++) if (*t == '\t') { *t = 0; c[n++] = t + 1; }
    if (n < 5) continue;
    if (pl->nm == pl->capm) {
      int cc = pl->capm ? pl->capm * 2 : 256;
      Synced *nm = realloc(pl->m, cc * sizeof *nm);
      if (!nm) break;
      pl->m = nm;
      pl->capm = cc;
    }
    pl->m[pl->nm++] = (Synced){strdup(c[0]), strdup(c[1]), strdup(c[4]), atoll(c[2]), atoll(c[3])};
  }
  fclose(f);
}
static void manifest_save(const char *root, const Plan *pl) {
  char p[200], part[220];
  manifest_path(root, p, sizeof p);
  snprintf(part, sizeof part, "%s.part", p);
  sceIoMkdir(DIRP, 0777);
  FILE *f = fopen(part, "w");
  if (!f) return;
  fprintf(f, "%s\n", pl->root_id);
  for (int k = 0; k < pl->nm; k++)
    if (pl->m[k].path && pl->m[k].id && pl->m[k].md5) fprintf(f, "%s\t%s\t%lld\t%lld\t%s\n", pl->m[k].path, pl->m[k].id, (long long)pl->m[k].size, (long long)pl->m[k].mtime, pl->m[k].md5);
  int ok = fclose(f) == 0;
  if (ok) { sceIoRemove(p); sceIoRename(part, p); }
}
static Synced *synced_set(Plan *pl, Map *mm, const char *path, const char *id, int64_t size, int64_t mtime, const char *md5) {
  int k = map_get(mm, path);
  if (k < 0) {
    if (pl->nm == pl->capm) {
      int c = pl->capm ? pl->capm * 2 : 256;
      Synced *nm = realloc(pl->m, c * sizeof *nm);
      if (!nm) return NULL;
      // (the map's keys are the paths' own strings: they stay where they are)
      pl->m = nm;
      pl->capm = c;
    }
    k = pl->nm++;
    pl->m[k] = (Synced){strdup(path), NULL, NULL, 0, 0};
    if (pl->m[k].path) map_put(mm, pl->m[k].path, k);
  } else {
    free(pl->m[k].id);
    free(pl->m[k].md5);
  }
  pl->m[k].id = strdup(id);
  pl->m[k].md5 = strdup(md5);
  pl->m[k].size = size;
  pl->m[k].mtime = mtime;
  return &pl->m[k];
}
static void synced_drop(Plan *pl, Map *mm, const char *path) {
  int k = map_get(mm, path);
  if (k < 0) return;
  free(pl->m[k].id); free(pl->m[k].md5);
  pl->m[k].id = pl->m[k].md5 = NULL;
  // (the path stays as the map's key; it isn't written)
  pl->m[k].size = -1;
}

// ---------- Drive: the folder, the listing, files up and down ----------
static int ensure_root(Plan *pl, const char *folder, int make, const char **why) {
  Json j;
  uint8_t *raw = NULL;
  if (pl->root_id[0]) {
    char url[300];
    snprintf(url, sizeof url, "%s/%s?fields=id,trashed", FILES, pl->root_id);
    int r = call_json(url, NULL, &j, &raw, why);
    if (r == 0) {
      int trashed = json_get(&j, 0, "trashed") >= 0 && j.t[json_get(&j, 0, "trashed")].type == J_TRUE;
      json_free(&j);
      free(raw);
      if (!trashed) return 0;
    } else if (r != -404) return -1;
  }
  char q[400], qe[1200], fe[200], url[1500];
  json_esc(folder, fe, sizeof fe); // (Drive's quoting is like JSON's for ' and \)
  char fq[200];
  size_t n = 0;
  for (const char *s = folder; *s && n + 3 < sizeof fq; s++) { if (*s == '\'' || *s == '\\') fq[n++] = '\\'; fq[n++] = *s; }
  fq[n] = 0;
  snprintf(q, sizeof q, "name = '%s' and mimeType = '" FOLDER_MIME "' and 'root' in parents and trashed = false", fq);
  url_enc(q, qe, sizeof qe);
  snprintf(url, sizeof url, "%s?q=%s&fields=files(id)&orderBy=createdTime", FILES, qe);
  if (call_json(url, NULL, &j, &raw, why) != 0) return -1;
  int files = json_get(&j, 0, "files");
  int first = json_at(&j, files, 0);
  char id[80] = "";
  if (first >= 0) json_gs(&j, first, "id", id, sizeof id);
  json_free(&j);
  free(raw);
  if (!id[0]) {
    if (!make) return 1; // (not in Drive)
    char meta[400];
    snprintf(meta, sizeof meta, "{\"name\":\"%s\",\"mimeType\":\"" FOLDER_MIME "\",\"parents\":[\"root\"]}", fe);
    static const char *const hs[] = {"Content-Type: application/json; charset=UTF-8", NULL};
    LvReq rq = {.method = "POST", .headers = hs, .body = meta, .body_len = strlen(meta)};
    char fu[240];
    snprintf(fu, sizeof fu, "%s?fields=id", FILES);
    if (call_json(fu, &rq, &j, &raw, why) != 0) return -1;
    json_gs(&j, 0, "id", id, sizeof id);
    json_free(&j);
    free(raw);
    if (!id[0]) { *why = "Drive made no folder"; return -1; }
  }
  snprintf(pl->root_id, sizeof pl->root_id, "%s", id);
  return 0;
}
int drive_has_manifest(const char *root) {
  char p[200];
  manifest_path(root, p, sizeof p);
  SceIoStat st;
  return sceIoGetstat(p, &st) >= 0;
}
int drive_folder_exists(const char *folder) {
  if (!http) http = lv_http_new();
  Plan pl;
  memset(&pl, 0, sizeof pl);
  const char *why = NULL;
  return ensure_root(&pl, folder, 0, &why) == 0;
}

typedef struct { char *id, *name, *parent, *md5; int64_t size; int folder; char *path; int done; } Node;
static const char *node_path(Node *nd, Map *ids, int k, const char *root, int depth) {
  if (nd[k].done) return nd[k].path;
  nd[k].done = 1;
  if (depth > 64 || !nd[k].parent) return NULL;
  if (!strcmp(nd[k].parent, root)) { nd[k].path = strdup(nd[k].name); return nd[k].path; }
  int up = map_get(ids, nd[k].parent);
  if (up < 0) return NULL;
  const char *u = node_path(nd, ids, up, root, depth + 1);
  if (!u) return NULL;
  size_t n = strlen(u) + strlen(nd[k].name) + 2;
  nd[k].path = malloc(n);
  if (nd[k].path) snprintf(nd[k].path, n, "%s/%s", u, nd[k].name);
  return nd[k].path;
}
// Everything below the folder: files (by path) into pl->r, folders (by path) into *folders.
static int list_remote(Plan *pl, Map *folders_by_path, char ***folder_ids, int *nfold, const char **why) {
  Node *nd = NULL;
  int nn = 0, cap = 0;
  char token_p[300] = "";
  for (;;) {
    char url[700], te[600] = "";
    if (token_p[0]) url_enc(token_p, te, sizeof te);
    snprintf(url, sizeof url, "%s?q=trashed%%20%%3D%%20false&pageSize=1000&spaces=drive&fields=nextPageToken,files(id,name,mimeType,parents,md5Checksum,size)%s%s",
             FILES, te[0] ? "&pageToken=" : "", te);
    Json j;
    uint8_t *raw = NULL;
    if (call_json(url, NULL, &j, &raw, why) != 0) { for (int k = 0; k < nn; k++) { free(nd[k].id); free(nd[k].name); free(nd[k].parent); free(nd[k].md5); free(nd[k].path); } free(nd); return -1; }
    int files = json_get(&j, 0, "files");
    for (int k = 0; files >= 0 && k < j.t[files].size; k++) {
      int f = json_at(&j, files, k);
      if (nn == cap) { cap = cap ? cap * 2 : 1024; Node *x = realloc(nd, cap * sizeof *x); if (!x) break; nd = x; }
      char id[80], name[400], parent[80], mime[80], md5[40], size[24];
      json_gs(&j, f, "id", id, sizeof id);
      json_gs(&j, f, "name", name, sizeof name);
      json_str(&j, json_at(&j, json_get(&j, f, "parents"), 0), parent, sizeof parent);
      json_gs(&j, f, "mimeType", mime, sizeof mime);
      json_gs(&j, f, "md5Checksum", md5, sizeof md5);
      json_gs(&j, f, "size", size, sizeof size);
      if (!id[0] || strchr(name, '/')) continue;
      nd[nn++] = (Node){strdup(id), strdup(name), parent[0] ? strdup(parent) : NULL, strdup(md5), atoll(size), !strcmp(mime, FOLDER_MIME), NULL, 0};
    }
    json_gs(&j, 0, "nextPageToken", token_p, sizeof token_p);
    json_free(&j);
    free(raw);
    if (!token_p[0] || cancel) break;
  }
  Map ids;
  map_init(&ids, nn);
  // (two with one path: always the same one wins, the smaller id, as the PC sorts them)
  for (int k = 0; k < nn; k++) if (nd[k].id) map_put(&ids, nd[k].id, k);
  int *order = malloc((nn + 1) * sizeof *order);
  for (int k = 0; order && k < nn; k++) order[k] = k;
  for (int a = 1; order && a < nn; a++) { int x = order[a], b = a - 1; while (b >= 0 && strcmp(nd[order[b]].id, nd[x].id) > 0) { order[b + 1] = order[b]; b--; } order[b + 1] = x; }
  Map seen;
  map_init(&seen, nn);
  for (int o = 0; order && o < nn; o++) {
    int k = order[o];
    const char *p = node_path(nd, &ids, k, pl->root_id, 0);
    if (!p || map_get(&seen, (char *)p) >= 0) continue;
    if (nd[k].folder) {
      char **fi = realloc(*folder_ids, (*nfold + 1) * sizeof **folder_ids);
      if (!fi) continue;
      *folder_ids = fi;
      fi[*nfold] = strdup(nd[k].id);
      map_put(&seen, (char *)p, 0);
      map_put(folders_by_path, strdup(p), (*nfold)++);
    } else if (nd[k].md5 && nd[k].md5[0] && ok_path(p)) {
      if (pl->nr == pl->capr) { int c = pl->capr ? pl->capr * 2 : 256; Remote *x = realloc(pl->r, c * sizeof *x); if (!x) continue; pl->r = x; pl->capr = c; }
      map_put(&seen, (char *)p, 0);
      pl->r[pl->nr++] = (Remote){strdup(p), strdup(nd[k].id), strdup(nd[k].md5), nd[k].size};
    }
  }
  map_free(&seen);
  map_free(&ids);
  free(order);
  for (int k = 0; k < nn; k++) { free(nd[k].id); free(nd[k].name); free(nd[k].parent); free(nd[k].md5); free(nd[k].path); }
  free(nd);
  return cancel ? -1 : 0;
}

static int make_folder(const char *name, const char *parent, char *id_out, size_t cap, const char **why) {
  char ne[600], meta[800];
  json_esc(name, ne, sizeof ne);
  snprintf(meta, sizeof meta, "{\"name\":\"%s\",\"mimeType\":\"" FOLDER_MIME "\",\"parents\":[\"%s\"]}", ne, parent);
  static const char *const hs[] = {"Content-Type: application/json; charset=UTF-8", NULL};
  LvReq rq = {.method = "POST", .headers = hs, .body = meta, .body_len = strlen(meta)};
  Json j;
  uint8_t *raw = NULL;
  char fu[240];
    snprintf(fu, sizeof fu, "%s?fields=id", FILES);
    if (call_json(fu, &rq, &j, &raw, why) != 0) return -1;
  json_gs(&j, 0, "id", id_out, cap);
  json_free(&j);
  free(raw);
  return id_out[0] ? 0 : -1;
}
static int folder_for(const char *rel_dir, const char *root, Map *by_path, char ***ids, int *nids, char *out, size_t cap, const char **why) {
  if (!rel_dir[0]) { snprintf(out, cap, "%s", root); return 0; }
  int k = map_get(by_path, rel_dir);
  if (k >= 0) { snprintf(out, cap, "%s", (*ids)[k]); return 0; }
  char up[900], parent[80];
  snprintf(up, sizeof up, "%s", rel_dir);
  char *sl = strrchr(up, '/');
  const char *name = rel_dir;
  if (sl) { *sl = 0; name = rel_dir + (sl - up) + 1; } else up[0] = 0;
  if (folder_for(up, root, by_path, ids, nids, parent, sizeof parent, why) != 0) return -1;
  if (make_folder(name, parent, out, cap, why) != 0) return -1;
  char **fi = realloc(*ids, (*nids + 1) * sizeof **ids);
  if (!fi) return 0;
  *ids = fi;
  fi[*nids] = strdup(out);
  map_put(by_path, strdup(rel_dir), (*nids)++);
  return 0;
}

// A file up, in resumable pieces: a new one into parent, or over existing. Its Drive id and md5 out.
static int upload(const char *path, const char *name, const char *parent, const char *existing, char *id_out, char *md5_out, const char **why) {
  struct stat st;
  if (stat(path, &st) != 0) { *why = "The file is gone"; return -1; }
  uint64_t total = (uint64_t)st.st_size;
  char ne[600], meta[900], url[600], len_h[64], loc[1500];
  json_esc(name, ne, sizeof ne);
  if (existing) { snprintf(meta, sizeof meta, "{}"); snprintf(url, sizeof url, "%s/%s?uploadType=resumable&fields=id,md5Checksum", UPLOAD, existing); }
  else { snprintf(meta, sizeof meta, "{\"name\":\"%s\",\"parents\":[\"%s\"]}", ne, parent); snprintf(url, sizeof url, "%s?uploadType=resumable&fields=id,md5Checksum", UPLOAD); }
  snprintf(len_h, sizeof len_h, "X-Upload-Content-Length: %llu", (unsigned long long)total);
  const char *hs[] = {"Content-Type: application/json; charset=UTF-8", len_h, NULL};
  LvReq rq = {.method = existing ? "PATCH" : "POST", .headers = hs, .body = meta, .body_len = strlen(meta), .location = loc, .location_cap = sizeof loc};
  uint8_t *o = NULL;
  size_t n;
  int st0 = call(url, &rq, &o, &n, why);
  free(o);
  if (st0 != 200 || !loc[0]) { if (!*why || !**why) *why = "Drive didn't take the file"; return -1; }
  uint64_t at = 0;
  for (;;) {
    uint64_t piece = total - at < CHUNK ? total - at : CHUNK;
    char range[96];
    if (total == 0) snprintf(range, sizeof range, "Content-Range: bytes */0");
    else snprintf(range, sizeof range, "Content-Range: bytes %llu-%llu/%llu", (unsigned long long)at, (unsigned long long)(at + piece - 1), (unsigned long long)total);
    const char *h2[] = {range, NULL};
    LvReq put = {.method = "PUT", .headers = h2, .body_path = total ? path : NULL, .body_off = at, .body_n = piece};
    o = NULL;
    // (the upload's own address needs no token, but one does no harm: the same call, with retries)
    int s = call(loc, &put, &o, &n, why);
    if (s == 308) { free(o); at += piece; snprintf(drive.file, sizeof drive.file, "%s (%d%%)", name, (int)(at * 100 / (total ? total : 1))); continue; }
    if (s == 200 || s == 201) {
      Json j;
      int ok = o && json_parse(&j, (const char *)o, n) == 0;
      if (ok) { json_gs(&j, 0, "id", id_out, 80); json_gs(&j, 0, "md5Checksum", md5_out, 40); json_free(&j); }
      free(o);
      return id_out[0] ? 0 : -1;
    }
    free(o);
    if (!*why || !**why) *why = "The upload was cut off";
    return -1;
  }
}
static int download(const char *id, const char *to, const char **why) {
  char url[300];
  snprintf(url, sizeof url, "%s/%s?alt=media", FILES, id);
  for (int attempt = 0; attempt < 3; attempt++) {
    if (refresh_token(why) != 0) return -1;
    char auth[2200];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
    const char *hs[] = {auth, NULL};
    LvReq rq = {.headers = hs};
    size_t n;
    int st = lv_http_req_file(http, url, &rq, to, &n, &cancel, NULL, why);
    if (st == 200) return 0;
    if (st == 401) { pthread_mutex_lock(&amx); token[0] = 0; pthread_mutex_unlock(&amx); continue; }
    if ((st == 429 || st >= 500 || st < 0) && attempt < 2 && !cancel) { sceKernelDelayThread(1500000 << attempt); continue; }
    return -1;
  }
  return -1;
}
static int trash_remote(const char *id, const char **why) {
  char url[300];
  snprintf(url, sizeof url, "%s/%s", FILES, id);
  static const char *const hs[] = {"Content-Type: application/json; charset=UTF-8", NULL};
  LvReq rq = {.method = "PATCH", .headers = hs, .body = "{\"trashed\":true}", .body_len = 16};
  uint8_t *o = NULL;
  size_t n;
  int st = call(url, &rq, &o, &n, why);
  free(o);
  return st == 200 || st == 404 ? 0 : -1;
}

// ---------- one sync ----------
enum { A_UP, A_DOWN, A_DEL_HERE, A_DEL_THERE, A_CONFLICT };
typedef struct { const char *path; int act, l, r, m; } Step;
static char sync_root[600], sync_folder[80];
static int sync_make;
static Plan *sort_plan;
static int64_t step_size(const Step *s) {
  if (s->act == A_DEL_HERE || s->act == A_DEL_THERE) return -1;
  if (s->act == A_UP) return sort_plan->l[s->l].size;
  return sort_plan->r[s->r].size;
}
static int cmp_steps(const void *a, const void *b) {
  int64_t x = step_size(a), y = step_size(b);
  return x < y ? -1 : x > y;
}

static void mkdirs_for(const char *file) {
  char p[1300];
  snprintf(p, sizeof p, "%s", file);
  for (char *s = strchr(p + 5, '/'); s; s = strchr(s + 1, '/')) { *s = 0; sceIoMkdir(p, 0777); *s = '/'; }
}
static void conflict_name(const char *path, char *o, size_t cap) {
  const char *slash = strrchr(path, '/'), *dot = strrchr(path, '.');
  if (!dot || (slash && dot < slash) || dot == (slash ? slash + 1 : path)) snprintf(o, cap, "%s" CONFLICT, path);
  else snprintf(o, cap, "%.*s" CONFLICT "%s", (int)(dot - path), path, dot);
}
static void to_trash(const char *full, const char *rel) {
  sceIoMkdir(DIRP, 0777);
  sceIoMkdir(DIRP "/trash", 0777);
  char to[1300], flat[900];
  snprintf(flat, sizeof flat, "%s", rel);
  for (char *s = flat; *s; s++) if (*s == '/') *s = '_';
  snprintf(to, sizeof to, DIRP "/trash/%lld_%s", (long long)time(NULL), flat);
  if (rename(full, to) != 0) remove(full);
}

static void *sync_main(void *arg) {
  sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << 2); // (off the drawing thread's core)
  (void)arg;
  if (!http) http = lv_http_new();
  Plan pl;
  memset(&pl, 0, sizeof pl);
  const char *why = NULL;
  Map lm = {0}, rm = {0}, mm = {0}, fby = {0};
  char **fids = NULL;
  int nf = 0;
  Step *steps = NULL;
  int ns = 0;
  manifest_load(sync_root, &pl);
  char old_root[80];
  snprintf(old_root, sizeof old_root, "%s", pl.root_id);
  say("Looking in your Google Drive…");
  {
    int r = ensure_root(&pl, sync_folder, sync_make, &why);
    if (r == 1) { snprintf(drive.message, sizeof drive.message, "There's no \u201c%s\u201d folder in your Drive yet: sync it from the PC first.", sync_folder); why = drive.message; goto fail; }
    if (r != 0) goto fail;
  }
  if (strcmp(old_root, pl.root_id)) {
    // another (or a new) Drive folder: nothing is known about it, so nothing is deleted this time
    for (int k = 0; k < pl.nm; k++) { free(pl.m[k].path); free(pl.m[k].id); free(pl.m[k].md5); }
    pl.nm = 0;
  }
  map_init(&fby, 64);
  if (list_remote(&pl, &fby, &fids, &nf, &why) != 0) goto fail;
  say("Looking at the vault on the memory card…");
  walk(sync_root, "", &pl, 0);
  if (cancel) { why = "Sync stopped"; goto fail; }
  // an encrypted vault in Drive must have been made with the same password as this one
  {
    char kc[700];
    snprintf(kc, sizeof kc, "%s/_app/key.check", sync_root);
    FILE *f = fopen(kc, "rb");
    if (f) {
      uint8_t mine[512];
      size_t mn = fread(mine, 1, sizeof mine, f);
      fclose(f);
      for (int k = 0; k < pl.nr; k++) {
        if (strcmp(pl.r[k].path, "_app/key.check")) continue;
        char tmp[700];
        snprintf(tmp, sizeof tmp, DIRP "/key.check.part");
        sceIoMkdir(DIRP, 0777);
        if (download(pl.r[k].id, tmp, &why) != 0) goto fail;
        FILE *g = fopen(tmp, "rb");
        uint8_t theirs[512];
        size_t tn = g ? fread(theirs, 1, sizeof theirs, g) : 0;
        if (g) fclose(g);
        remove(tmp);
        if (tn != mn || memcmp(mine, theirs, mn)) { why = "The vault in your Google Drive was made with another password. Use the same password everywhere."; goto fail; }
      }
    }
  }
  map_init(&lm, pl.nl);
  map_init(&rm, pl.nr);
  map_init(&mm, pl.nm);
  for (int k = 0; k < pl.nl; k++) map_put(&lm, pl.l[k].path, k);
  for (int k = 0; k < pl.nr; k++) map_put(&rm, pl.r[k].path, k);
  for (int k = 0; k < pl.nm; k++) if (pl.m[k].path) map_put(&mm, pl.m[k].path, k);
  // the plan: every path on either side, or in the last sync
  {
    int cap = pl.nl + pl.nr + pl.nm + 1;
    steps = malloc(cap * sizeof *steps);
    Map all;
    if (!steps || map_init(&all, cap) != 0) { why = "Not enough memory"; goto fail; }
    for (int side = 0; side < 3; side++) {
      int cnt = side == 0 ? pl.nl : side == 1 ? pl.nr : pl.nm;
      for (int k = 0; k < cnt; k++) {
        const char *p = side == 0 ? pl.l[k].path : side == 1 ? pl.r[k].path : pl.m[k].path;
        if (!p || map_get(&all, p) >= 0) continue;
        map_put(&all, (char *)p, 0);
        int l = map_get(&lm, p), r = map_get(&rm, p), m = map_get(&mm, p);
        int lc = l >= 0 && (m < 0 || pl.m[m].size != pl.l[l].size || pl.m[m].mtime != pl.l[l].mtime);
        int rc = r >= 0 && (m < 0 || strcmp(pl.m[m].md5, pl.r[r].md5));
        int act;
        if (l < 0 && r < 0) { synced_drop(&pl, &mm, p); continue; }
        else if (l >= 0 && r < 0) act = m >= 0 && !lc ? A_DEL_HERE : A_UP;
        else if (l < 0) act = m >= 0 && !rc ? A_DEL_THERE : A_DOWN;
        else if (!lc && !rc) continue;
        else if (lc && !rc) act = A_UP;
        else if (rc && !lc) act = A_DOWN;
        else act = A_CONFLICT;
        steps[ns++] = (Step){p, act, l, r, m};
      }
    }
    map_free(&all);
  }
  // a sync that would delete most of one side is a mistake somewhere: stop and say so
  {
    int here = 0, there = 0;
    int64_t down = 0;
    for (int k = 0; k < ns; k++) {
      here += steps[k].act == A_DEL_HERE;
      there += steps[k].act == A_DEL_THERE;
      if (steps[k].act == A_DOWN || steps[k].act == A_CONFLICT) down += pl.r[steps[k].r].size;
    }
    if (here > 20 && here * 2 > pl.nl) {
      snprintf(drive.message, sizeof drive.message, "Stopped: this would delete %d of the %d files on the Vita because they're gone from Drive.", here, pl.nl);
      why = drive.message;
      goto fail;
    }
    if (there > 20 && there * 2 > pl.nr) {
      snprintf(drive.message, sizeof drive.message, "Stopped: this would delete %d of the %d files in Drive because they're gone from the Vita.", there, pl.nr);
      why = drive.message;
      goto fail;
    }
    int64_t room = free_room(sync_root);
    if (room >= 0 && down > room - 32 * 1024 * 1024) {
      snprintf(drive.message, sizeof drive.message, "Not enough room on the memory card: %lld MB to come, %lld MB free.", (long long)(down >> 20), (long long)(room >> 20));
      why = drive.message;
      goto fail;
    }
    drive.bytes_left = down;
  }
  // (deletions, then small files first: the index, the thumbnails; the big videos last)
  sort_plan = &pl;
  qsort(steps, ns, sizeof *steps, cmp_steps);
  drive.total = ns;
  say("Syncing…");
  int since_save = 0;
  for (int k = 0; k < ns && !cancel; k++) {
    Step *s = &steps[k];
    const char *p = s->path;
    char full[1300];
    snprintf(full, sizeof full, "%s/%s", sync_root, p);
    const char *slash = strrchr(p, '/');
    const char *name = slash ? slash + 1 : p;
    char dir[900];
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - p) : 0, p);
    snprintf(drive.file, sizeof drive.file, "%s", p);
    const char *e = NULL;
    int ok = 0;
    if (s->act == A_CONFLICT) {
      // the same file after all (a first sync of a copied vault): just noted
      char md5[33];
      if (pl.l[s->l].size == pl.r[s->r].size && file_md5(full, md5) == 0 && !strcmp(md5, pl.r[s->r].md5)) {
        synced_set(&pl, &mm, p, pl.r[s->r].id, pl.l[s->l].size, pl.l[s->l].mtime, md5);
        ok = 1;
        goto next;
      }
    }
    switch (s->act) {
      case A_UP: {
        char parent[80], id[80] = "", md5[40] = "";
        if (folder_for(dir, pl.root_id, &fby, &fids, &nf, parent, sizeof parent, &e) == 0 &&
            upload(full, name, parent, s->r >= 0 ? pl.r[s->r].id : NULL, id, md5, &e) == 0) {
          synced_set(&pl, &mm, p, id, pl.l[s->l].size, pl.l[s->l].mtime, md5);
          drive.up++;
          ok = 1;
        }
        break;
      }
      case A_DOWN: {
        char tmp[1320];
        snprintf(tmp, sizeof tmp, "%s.drive.part", full);
        mkdirs_for(full);
        if (download(pl.r[s->r].id, tmp, &e) == 0) {
          // (changed here since the look round: Drive's copy goes beside it instead)
          struct stat st;
          int same = s->l < 0 ? stat(full, &st) != 0 : stat(full, &st) == 0 && st.st_size == pl.l[s->l].size && mtime_of(&st) == pl.l[s->l].mtime;
          if (!same) {
            char c[1320];
            conflict_name(full, c, sizeof c);
            remove(c);
            rename(tmp, c);
            drive.conflicts++;
          } else {
            char old[1320];
            snprintf(old, sizeof old, "%s.drive.old", full);
            remove(old);
            if (s->l >= 0) rename(full, old);
            if (rename(tmp, full) == 0) {
              remove(old);
              if (stat(full, &st) == 0) synced_set(&pl, &mm, p, pl.r[s->r].id, st.st_size, mtime_of(&st), pl.r[s->r].md5);
              ok = 1;
            } else {
              if (s->l >= 0) rename(old, full);
              remove(tmp);
              e = "It couldn't be put in place";
            }
          }
          drive.down++;
          drive.bytes_left -= pl.r[s->r].size;
        }
        break;
      }
      case A_CONFLICT: {
        // the Vita's copy wins; Drive's is kept beside it
        char tmp[1320], c[1320], id[80] = "", md5[40] = "";
        snprintf(tmp, sizeof tmp, "%s.drive.part", full);
        conflict_name(full, c, sizeof c);
        if (download(pl.r[s->r].id, tmp, &e) == 0) {
          remove(c);
          rename(tmp, c);
          drive.bytes_left -= pl.r[s->r].size;
          if (upload(full, name, "", pl.r[s->r].id, id, md5, &e) == 0) {
            synced_set(&pl, &mm, p, id, pl.l[s->l].size, pl.l[s->l].mtime, md5);
            ok = 1;
          }
          drive.conflicts++;
        }
        break;
      }
      case A_DEL_HERE:
        to_trash(full, p);
        synced_drop(&pl, &mm, p);
        drive.here++;
        ok = 1;
        break;
      case A_DEL_THERE:
        if (trash_remote(pl.r[s->r].id, &e) == 0) { synced_drop(&pl, &mm, p); drive.there++; ok = 1; }
        break;
    }
  next:
    if (!ok) {
      if (e && (strstr(e, "full") || strstr(e, "signed you out"))) { why = e; manifest_save(sync_root, &pl); goto fail; }
      drive.failed++;
      snprintf(drive.message, sizeof drive.message, "%s: %s", p, e ? e : "failed");
      LOG("drive: %s: %s\n", p, e ? e : "failed");
    }
    drive.done = k + 1;
    if (++since_save >= 25) { manifest_save(sync_root, &pl); since_save = 0; }
  }
  manifest_save(sync_root, &pl);
  if (cancel) { why = "Sync stopped"; goto fail; }
  {
    time_t t = time(NULL);
    struct tm l;
    lv_local_tm(t, &l);
    pthread_mutex_lock(&amx);
    strftime(acct.last, sizeof acct.last, "%Y-%m-%d %H:%M", &l);
    save_account();
    pthread_mutex_unlock(&amx);
  }
  if (!drive.failed) {
    if (!ns) say("Already in step with Google Drive");
    else snprintf(drive.message, sizeof drive.message, "Done: %d up, %d down%s", drive.up, drive.down, drive.conflicts ? ", some kept from both sides" : "");
  }
  drive.state = DRV_DONE;
  goto out;
fail:
  if (why && why != drive.message) snprintf(drive.message, sizeof drive.message, "%s", why);
  else if (!why) say("The sync didn't finish");
  drive.state = DRV_FAILED;
out:
  free(steps);
  map_free(&lm); map_free(&rm); map_free(&mm);
  for (int k = 0; k < fby.cap; k++) if (fby.k && fby.k[k]) free(fby.k[k]);
  map_free(&fby);
  for (int k = 0; k < nf; k++) free(fids[k]);
  free(fids);
  for (int k = 0; k < pl.nl; k++) free(pl.l[k].path);
  for (int k = 0; k < pl.nr; k++) { free(pl.r[k].path); free(pl.r[k].id); free(pl.r[k].md5); }
  for (int k = 0; k < pl.nm; k++) { free(pl.m[k].path); free(pl.m[k].id); free(pl.m[k].md5); }
  free(pl.l); free(pl.r); free(pl.m);
  return NULL;
}
void drive_sync(const char *root, const char *folder, int make) {
  if (drive.state == DRV_CODE || drive.state == DRV_SYNCING) return;
  memset((void *)&drive, 0, sizeof drive);
  snprintf(sync_root, sizeof sync_root, "%s", root);
  snprintf(sync_folder, sizeof sync_folder, "%s", folder);
  sync_make = make;
  sceIoMkdir(root, 0777);
  drive.state = DRV_SYNCING;
  say("Starting…");
  if (start_worker(sync_main, NULL) != 0) { say("The sync couldn't start"); drive.state = DRV_FAILED; }
}
