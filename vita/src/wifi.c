// Wi-Fi transfer: files from a phone or a PC straight into the vault, over the local network. While
// the screen is open the Vita serves a small page at http://<its address>:8080 (a QR code takes a phone
// there); the page sends each file chosen (or dropped on it) as the body of a POST, the Vita writes it
// to the card, makes a card-sized thumbnail for a picture, and adds it to the vault as a Browser save
// is added (its row appended; encrypted in an Untraceable vault). Nothing is served but that page:
// no file of the vault can be read from outside. Compiled inside main.c (#include "wifi.c").
#include <arpa/inet.h>
#include <ctype.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <psp2/net/netctl.h>

#define WF_PORT 8080
#define WF_TMP DATA "/incoming.part"
static volatile int wf_run;
static int wf_started;
static pthread_t wf_thread;
static char wf_ip[32];
static volatile int wf_got, wf_failed, wf_busy;
static volatile int64_t wf_bytes, wf_total; // (the file coming now)
typedef struct { LvNew nw; LvAdded added; char name[200], cat[64], when[40], kind[8]; int vgen; } WfDone;
static WfDone wf_done[8];
static int nwf_done;
static pthread_mutex_t wfmx = PTHREAD_MUTEX_INITIALIZER;
static char wf_key[CV_N][2048]; // (conversion keys pasted on the page, for the drawing thread)
static int wf_key_set[CV_N];

static const char WF_PAGE[] =
  "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Lewd Vault on the Vita</title><style>body{font:16px system-ui,sans-serif;background:#141015;color:#eee;margin:0;padding:24px;max-width:640px}"
  "h1{font-size:22px}#d{border:2px dashed #e85d8a;border-radius:16px;padding:32px;text-align:center;margin:16px 0}"
  "label{background:#e85d8a;color:#140c10;padding:12px 20px;border-radius:24px;cursor:pointer;display:inline-block}"
  "input{display:none}li{margin:6px 0;list-style:none}small{color:#999}.ok{color:#8fd}.no{color:#f88}</style></head><body>"
  "<h1>Send to Lewd Vault on the Vita</h1><div id=d><label>Choose pictures and videos<input id=f type=file multiple "
  "accept='image/*,video/*,.webm,.mkv'></label><p><small>or drop them here. JPEG, PNG, WebP, GIF, MP4, WebM.</small></p></div><ul id=l></ul>"
  "<script>const l=document.getElementById('l');let q=Promise.resolve();"
  "function send(f){const li=document.createElement('li');li.textContent=f.name+' \\u2026';l.prepend(li);"
  "q=q.then(()=>new Promise(r=>{const x=new XMLHttpRequest();x.open('POST','/upload?name='+encodeURIComponent(f.name));"
  "x.upload.onprogress=e=>{if(e.lengthComputable)li.textContent=f.name+' '+Math.round(e.loaded*100/e.total)+'%'};"
  "x.onload=()=>{li.textContent=f.name+(x.status==200?' \\u2713 in the vault':' \\u2717 '+x.responseText);li.className=x.status==200?'ok':'no';r()};"
  "x.onerror=()=>{li.textContent=f.name+' \\u2717 the Vita stopped answering';li.className='no';r()};x.send(f)}))}"
  "document.getElementById('f').onchange=e=>[...e.target.files].forEach(send);"
  "const d=document.getElementById('d');d.ondragover=e=>e.preventDefault();d.ondrop=e=>{e.preventDefault();[...e.dataTransfer.files].forEach(send)};"
  "</script>"
  "<h2 style='font-size:18px;margin-top:32px'>Video conversion keys</h2><p><small>Offline copies of big videos are made 480p by a "
  "conversion service, with your own API key: paste one here (it's kept on the Vita's memory card only; the service sees the "
  "video's address). Tried in this order, when Settings \u203a Video conversion is on (it starts off).</small></p>"
  "<div id=k></div><script>const S=['CloudConvert (cloudconvert.com)','FreeConvert (freeconvert.com)','Rendi (rendi.dev)'];"
  "S.forEach((n,i)=>{const p=document.createElement('p');p.innerHTML='<b>'+n+'</b><br><textarea rows=2 style=\"width:100%;display:block;"
  "background:#221a24;color:#eee;border:1px solid #444;border-radius:8px\"></textarea><button>Save on the Vita</button> <small></small>';"
  "const t=p.querySelector('textarea'),s=p.querySelector('small');p.querySelector('button').onclick=()=>{fetch('/key?svc='+i,"
  "{method:'POST',body:t.value.trim()}).then(r=>r.text()).then(m=>s.textContent=m).catch(()=>s.textContent='the Vita stopped answering')};"
  "document.getElementById('k').appendChild(p)})</script></body></html>";

static void wf_reply(int c, int code, const char *type, const char *body, size_t n) {
  char h[256];
  int hn = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
                    code, code == 200 ? "OK" : code == 404 ? "Not Found" : "Error", type, (unsigned)n);
  send(c, h, hn, 0);
  for (size_t at = 0; at < n;) { int s = send(c, body + at, n - at > 16384 ? 16384 : n - at, 0); if (s <= 0) break; at += s; }
}
static void wf_text(int c, int code, const char *m) { wf_reply(c, code, "text/plain; charset=utf-8", m, strlen(m)); }
static int wf_hex(char x) { return x >= '0' && x <= '9' ? x - '0' : x >= 'a' && x <= 'f' ? x - 'a' + 10 : x >= 'A' && x <= 'F' ? x - 'A' + 10 : -1; }
// a name from the address, %-decoded, with no folders in it
static void wf_name(const char *q, char *out, size_t cap) {
  size_t o = 0;
  for (const char *p = q; *p && *p != '&' && *p != ' ' && o + 1 < cap; p++) {
    char ch = *p;
    if (ch == '%' && wf_hex(p[1]) >= 0 && wf_hex(p[2]) >= 0) { ch = (char)(wf_hex(p[1]) * 16 + wf_hex(p[2])); p += 2; }
    else if (ch == '+') ch = ' ';
    if (ch == '/' || ch == '\\' || ch == ':' || (unsigned char)ch < 32) ch = '_';
    out[o++] = ch;
  }
  out[o] = 0;
  if (!out[0] || !strcmp(out, ".") || !strcmp(out, "..")) snprintf(out, cap, "upload");
}
static const char *wf_kind(const char *name) {
  const char *d = strrchr(name, '.');
  if (!d) return NULL;
  if (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg") || !strcasecmp(d, ".png") || !strcasecmp(d, ".webp")) return "image";
  if (!strcasecmp(d, ".gif")) return "gif";
  if (!strcasecmp(d, ".mp4") || !strcasecmp(d, ".m4v") || !strcasecmp(d, ".mov") || !strcasecmp(d, ".webm") || !strcasecmp(d, ".mkv")) return "video";
  return NULL;
}

// One file in: the body onto the card, then into the vault.
static void wf_upload(int c, const char *query, long long len, const char *already, size_t already_n) {
  char name[200];
  const char *nq = strstr(query, "name=");
  wf_name(nq ? nq + 5 : "upload", name, sizeof name);
  const char *kind = wf_kind(name);
  if (!kind) { wf_text(c, 415, "Not a picture or video the vault takes"); return; }
  if (len <= 0 || len > 4000LL * 1024 * 1024) { wf_text(c, 411, "No size given"); return; }
  if (!loaded_vault) { wf_text(c, 409, "No vault is open on the Vita"); return; }
  long long room = free_room(vault.root);
  if (room >= 0 && room < len + 16 * 1024 * 1024) { wf_text(c, 507, "Not enough room on the memory card"); return; }
  FILE *f = fopen(WF_TMP, "wb");
  if (!f) { wf_text(c, 500, "The card can't be written"); return; }
  wf_busy = 1;
  wf_total = len;
  wf_bytes = 0;
  snprintf(wf_last, sizeof wf_last, "%s", name);
  long long got = 0;
  if (already_n) { size_t k = already_n > (size_t)len ? (size_t)len : already_n; fwrite(already, 1, k, f); got = k; }
  static char buf[65536];
  int ok = 1;
  while (got < len && wf_run) {
    int r = recv(c, buf, len - got > (long long)sizeof buf ? (int)sizeof buf : (int)(len - got), 0);
    if (r <= 0) { ok = 0; break; }
    if (fwrite(buf, 1, r, f) != (size_t)r) { ok = 0; break; }
    got += r;
    wf_bytes = got;
  }
  if (fclose(f) != 0) ok = 0;
  if (!ok || got != len) { remove(WF_TMP); wf_busy = 0; wf_failed++; if (wf_run) wf_text(c, 500, "The file didn't all arrive"); return; }
  // a picture: its size and a card-sized thumbnail (as the Browser's saves have)
  WfDone d;
  memset(&d, 0, sizeof d);
  uint8_t *thumb = NULL;
  size_t tn = 0;
  int w = 0, h = 0, tw = 0, th = 0;
  if (!strcmp(kind, "image") && len < 48LL * 1024 * 1024) {
    size_t n = 0;
    uint8_t *b = lv_read_all(WF_TMP, NULL, &n);
    if (b) {
      lv_image_size(b, n, &w, &h);
      LvPixels px;
      int cw, ch;
      cover_size(b, n, &cw, &ch);
      if (lv_decode(b, n, cw, ch, &px) == 0) { thumb = lv_encode_jpeg(&px, 85, &tn); tw = px.w; th = px.h; free(px.rgba); }
      free(b);
    }
  }
  snprintf(d.name, sizeof d.name, "%s", name);
  snprintf(d.cat, sizeof d.cat, "%s", cfg.save_cat[0] ? cfg.save_cat : "Wi-Fi");
  snprintf(d.kind, sizeof d.kind, "%s", kind);
  now_iso(d.when, sizeof d.when);
  const char *ext = strrchr(name, '.') + 1;
  char e2[8];
  snprintf(e2, sizeof e2, "%s", ext);
  for (char *p = e2; *p; p++) *p = (char)tolower((unsigned char)*p);
  if (!strcmp(e2, "jpeg")) snprintf(e2, sizeof e2, "jpg");
  LvNew nw = {.name = d.name, .category = d.cat, .kind = d.kind, .tags = "", .source = "", .added = d.when, .width = w, .height = h, .from = WF_TMP};
  saver_flush(); // (a rewrite of index.csv waiting to be written goes first: this appends to it)
  pthread_mutex_lock(&vmx);
  const char *why = NULL;
  int r = loaded_vault ? lv_vault_add_files(&vault, &nw, NULL, (size_t)len, e2, thumb, tn, tw, th, &d.added, &why) : -1;
  d.vgen = vgen;
  pthread_mutex_unlock(&vmx);
  free(thumb);
  remove(WF_TMP);
  wf_busy = 0;
  if (r != 0) { wf_failed++; wf_text(c, 500, why ? why : "It couldn't be added"); return; }
  d.nw = nw;
  // (the loaded vault gets it on the drawing thread: wf_take)
  pthread_mutex_lock(&wfmx);
  if (nwf_done < 8) wf_done[nwf_done++] = d;
  pthread_mutex_unlock(&wfmx);
  wf_got++;
  wf_text(c, 200, "ok");
}

static void wf_serve(int c) {
  // the request's head (and maybe the start of its body)
  static char req[8192];
  size_t n = 0;
  char *end = NULL;
  while (n < sizeof req - 1 && wf_run) {
    int r = recv(c, req + n, sizeof req - 1 - n, 0);
    if (r <= 0) return;
    n += r;
    req[n] = 0;
    if ((end = strstr(req, "\r\n\r\n"))) break;
  }
  if (!end) return;
  *end = 0;
  const char *body = end + 4;
  size_t body_n = n - (size_t)(body - req);
  char method[8] = "", path[1024] = "";
  sscanf(req, "%7s %1023s", method, path);
  long long len = -1;
  for (char *l = strstr(req, "\r\n"); l; l = strstr(l + 2, "\r\n"))
    if (!strncasecmp(l + 2, "Content-Length:", 15)) len = atoll(l + 17);
  if (!strcmp(method, "GET") && (!strcmp(path, "/") || !strncmp(path, "/?", 2))) wf_reply(c, 200, "text/html; charset=utf-8", WF_PAGE, sizeof WF_PAGE - 1);
  else if (!strcmp(method, "POST") && !strncmp(path, "/upload", 7)) wf_upload(c, path, len, body, body_n);
  else if (!strcmp(method, "POST") && !strncmp(path, "/key?svc=", 9)) {
    // a conversion service's key, pasted on the page: handed to the drawing thread (wf_take)
    int svc = atoi(path + 9);
    static char key[2048];
    size_t have = body_n < sizeof key - 1 ? body_n : sizeof key - 1;
    memcpy(key, body, have);
    while (len > 0 && (long long)have < len && have < sizeof key - 1 && wf_run) { int r = recv(c, key + have, sizeof key - 1 - have, 0); if (r <= 0) break; have += (size_t)r; }
    key[have] = 0;
    while (have && (key[have - 1] == ' ' || key[have - 1] == '\n' || key[have - 1] == '\r')) key[--have] = 0;
    if (svc < 0 || svc >= CV_N || len > (long long)sizeof key - 1) { wf_text(c, 400, "Not a key the Vita can keep"); return; }
    pthread_mutex_lock(&wfmx);
    snprintf(wf_key[svc], sizeof wf_key[svc], "%s", key);
    wf_key_set[svc] = 1;
    pthread_mutex_unlock(&wfmx);
    wf_text(c, 200, have ? "\u2713 saved on the Vita" : "\u2713 removed from the Vita");
  }
  else wf_text(c, 404, "Nothing here");
}

static void *wf_main(void *arg) {
  pin_to_core(2); // (off the drawing thread's core)
  (void)arg;
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { snprintf(wf_last, sizeof wf_last, "No network socket"); wf_run = 0; return NULL; }
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(WF_PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 4) != 0) {
    snprintf(wf_last, sizeof wf_last, "Port %d is taken", WF_PORT);
    close(s);
    wf_run = 0;
    return NULL;
  }
  while (wf_run) {
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(s, &rs);
    struct timeval tv = {0, 300000};
    if (select(s + 1, &rs, NULL, NULL, &tv) <= 0) continue;
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int c = accept(s, (struct sockaddr *)&from, &fl); // (with somewhere to put the address: NULL is refused)
    if (c < 0) continue;
    struct timeval to = {20, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    wf_serve(c);
    close(c);
  }
  close(s);
  return NULL;
}

static void wf_start(void) {
  if (wf_run) return;
  lv_net_base(); // (the rest on the worker that fetches)
  SceNetCtlInfo info;
  memset(&info, 0, sizeof info);
  if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0) snprintf(wf_ip, sizeof wf_ip, "%s", info.ip_address);
  else wf_ip[0] = 0;
  wf_got = wf_failed = 0;
  wf_last[0] = 0;
  wf_run = 1;
  if (wf_started) { pthread_join(wf_thread, NULL); wf_started = 0; }
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 512 * 1024); // (it makes thumbnails)
  wf_started = pthread_create(&wf_thread, &at, wf_main, NULL) == 0;
  if (!wf_started) wf_run = 0;
}
static void wf_stop(void) {
  wf_run = 0;
  if (wf_started) { pthread_join(wf_thread, NULL); wf_started = 0; }
}
// (on the drawing thread: what came in, into the loaded vault)
static void wf_take(void) {
  // (nothing from the Wi-Fi page: back at once, no lock: most frames, and every one while it's off)
  int keys = 0;
  for (int s = 0; s < CV_N; s++) keys |= wf_key_set[s];
  if (!nwf_done && !keys) return;
  pthread_mutex_lock(&wfmx);
  if (keys && !conv_loaded) conv_keys_load(); // (the others kept: saving writes them all)
  for (int s = 0; s < CV_N; s++)
    if (wf_key_set[s]) { wf_key_set[s] = 0; snprintf(conv_keys[s], sizeof conv_keys[s], "%s", wf_key[s]); conv_keys_save(); sfx(SFX_DONE); say(conv_keys[s][0] ? "A conversion key came from the Wi-Fi page" : "A conversion key was removed"); }
  int n = nwf_done;
  WfDone got[8];
  memcpy(got, wf_done, n * sizeof *got);
  nwf_done = 0;
  pthread_mutex_unlock(&wfmx);
  for (int k = 0; k < n; k++) {
    WfDone *d = &got[k];
    if (d->vgen != vgen || !loaded_vault) continue;
    d->nw.name = d->name; d->nw.category = d->cat; d->nw.kind = d->kind; d->nw.added = d->when; d->nw.tags = ""; d->nw.source = "";
    int before = vault.n;
    pthread_mutex_lock(&vmx);
    int ok = lv_vault_add_memory(&vault, &d->nw, &d->added) == 0;
    pthread_mutex_unlock(&vmx);
    if (ok) library_grew(before);
    sfx(SFX_SAVED);
  }
}

static void draw_wifi(void) {
  text(30, 44, TEXT, 26, "Wi-Fi transfer");
  if (!wf_run && !wf_last[0]) wf_start();
  char url[64];
  snprintf(url, sizeof url, "http://%s:%d", wf_ip[0] ? wf_ip : "?", WF_PORT);
  if (wf_ip[0] && wf_run) draw_qr(url, 30, 76, 300);
  int x = wf_ip[0] && wf_run ? 360 : 30;
  static int online = 1;
  if (frame_no % 60 == 0) online = lv_net_online(); // (once a second, not every frame)
  if (!online && !wf_ip[0]) text(x, 110, ACC2, 18, "Not connected: turn on Wi-Fi in the Vita's settings");
  else if (!wf_run) text(x, 110, ACC2, 18, wf_last[0] ? wf_last : "It couldn't start");
  else {
    text(x, 110, DIM, 17, "On a phone or PC on the same Wi-Fi, open");
    text(x, 150, ACC, 30, url);
    text(x, 184, DIM, 17, "(or scan the code) and send pictures and videos.");
    char b[200];
    snprintf(b, sizeof b, "They go into “%s”.  %d in so far%s", cfg.save_cat[0] ? cfg.save_cat : "Wi-Fi", wf_got, wf_failed ? ", some didn't" : "");
    text(x, 230, TEXT, 17, b);
    if (wf_busy && wf_total > 0) {
      spinner(x + 494, 257, 10);
      float f = (float)wf_bytes / (float)wf_total;
      rrect(x, 252, 480, 10, 5, SURF2);
      rrect(x, 252, 480 * f < 10 ? 10 : 480 * f, 10, 5, ACC);
      text_fit(x, 286, FAINT, 15, wf_last, 480);
    } else if (wf_last[0]) {
      snprintf(b, sizeof b, "Last: %s", wf_last);
      text_fit(x, 286, FAINT, 15, b, 480);
    }
    text(x, 330, FAINT, 14, "Only this page is served: nothing in the vault can be read from outside.");
    text(x, 352, FAINT, 14, "The Vita stays awake while this is open. ○ stops it.");
  }
  if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_BACK); wf_stop(); wf_last[0] = 0; screen = wf_return; }
  draw_bar();
}
