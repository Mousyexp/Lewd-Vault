// The LiveArea page (sce_sys/livearea/contents/template.xml): its left frame opens the Google Drive
// vault and its right frame the vault on the card, straight away; the strip along the bottom shows
// the vault's numbers. Compiled inside main.c (#include "livearea.c").
//
// A frame's target is psgm:play?titleid=LWDV00001&param=drive (or local): the system starts the app
// with that param (sceAppMgrGetAppParam), or, when it's already running, sends it an app event.
// "The Google Drive vault" is the one Drive's "Get it from Drive" made (its folder ends in "(Drive)"),
// else one that has been synced with Drive; "the vault on the card" is the last one opened that isn't.
#include <psp2/appmgr.h>

static void close_viewer(void); // (main.c)
static int launch_to; // 0 as usual, 1 the Drive vault, 2 the vault on the card
static int la_target(const char *param) {
  if (strstr(param, "drive")) return 1;
  if (strstr(param, "local")) return 2;
  return 0;
}
static int is_drive_vault(const char *root) {
  size_t n = strlen(root);
  if (n > 8 && !strcmp(root + n - 8, " (Drive)")) return 1;
  return drive_has_manifest(root);
}
// The vault a target means ("" when there's none of that kind on the card).
static const char *la_vault(int target) {
  find_vaults();
  if (target == 1) {
    for (int k = 0; k < nfound; k++) { size_t n = strlen(found[k]); if (n > 8 && !strcmp(found[k] + n - 8, " (Drive)")) return found[k]; }
    for (int k = 0; k < nfound; k++) if (drive_has_manifest(found[k])) return found[k];
    return "";
  }
  if (cfg.local_vault[0] && lv_is_vault(cfg.local_vault) && !is_drive_vault(cfg.local_vault)) return cfg.local_vault;
  if (cfg.vault[0] && lv_is_vault(cfg.vault) && !is_drive_vault(cfg.vault)) return cfg.vault;
  for (int k = 0; k < nfound; k++) if (!is_drive_vault(found[k])) return found[k];
  return "";
}
// The app starting (after the PIN, if there is one): the vault the LiveArea asked for, else as usual.
static void open_start_vault(void) {
  int t = launch_to;
  launch_to = 0;
  if (t) {
    const char *v = la_vault(t);
    if (v[0]) { open_vault(v); return; }
    // no Drive vault on the card yet: Google Drive's screen, to get it (a new vault of its own)
    if (t == 1) { screen = S_PICK; open_drive(S_PICK); say("No Google Drive vault on this Vita yet: get it from Drive"); return; }
  }
  if (cfg.vault[0] && lv_is_vault(cfg.vault)) open_vault(cfg.vault);
  else if (nfound == 1) open_vault(found[0]);
  else screen = S_PICK;
}
// A frame tapped while the app runs: that vault, now (never past the PIN lock or the disguise).
static void la_poll_events(void) {
  SceAppUtilAppEventParam ev;
  memset(&ev, 0, sizeof ev);
  if (sceAppUtilReceiveAppEvent(&ev) < 0 || ev.type != 5 /* (from the LiveArea) */) return;
  char b[1024] = "";
  sceAppUtilAppEventParseLiveArea(&ev, b);
  int t = la_target(b);
  if (!t || screen == S_PIN || screen == S_CALC || screen == S_COVER || screen == S_UNLOCKING || decoy_session) return;
  const char *v = la_vault(t);
  if (!v[0]) { if (t == 1) open_drive(screen); return; }
  if (loaded_vault && !strcmp(v, vault.root)) { screen = S_LIBRARY; return; }
  if (screen == S_VIEWER) close_viewer();
  save_edits_now();
  resume_save();
  open_vault(v);
}

// ---------- the vault's numbers on the LiveArea ----------
// (sceLiveAreaUpdateFrameSync rewrites a frame of the page; its pictures come from dirpathTop)
int sceLiveAreaUpdateFrameSync(const char *formatVer, const char *frameXmlStr, int frameXmlStrLen, const char *dirpathTop, unsigned int flag);
static void la_update_stats(void) {
  if (!loaded_vault || decoy_session) return;
  static int loaded;
  if (!loaded) { sceSysmoduleLoadModule(SCE_SYSMODULE_LIVEAREA); loaded = 1; }
  // (the strip's picture, next to the page it's written into)
  sceIoMkdir(DATA "/livearea", 0777);
  SceIoStat st;
  if (sceIoGetstat(DATA "/livearea/strip.png", &st) < 0) {
    size_t n = 0;
    uint8_t *b = lv_read_all("app0:sce_sys/livearea/contents/strip.png", NULL, &n);
    if (b) { FILE *f = fopen(DATA "/livearea/strip.png", "wb"); if (f) { fwrite(b, 1, n, f); fclose(f); } free(b); }
  }
  long long hearts = 0;
  double secs = 0;
  for (int k = 0; k < vault.nviews; k++) { hearts += vault.views[k].likes; secs += vault.views[k].seconds; }
  const char *nm = strrchr(vault.root, '/');
  char time_s[32], line[256];
  if (secs >= 3600) snprintf(time_s, sizeof time_s, "%d h %d min", (int)(secs / 3600), (int)secs % 3600 / 60);
  else snprintf(time_s, sizeof time_s, "%d min", (int)(secs / 60));
  // (only numbers and the folder's name: what's in the vault never shows outside the app)
  snprintf(line, sizeof line, "%s%s  ·  %d items  ·  %lld hearts  ·  %s looking", is_drive_vault(vault.root) ? "☁ " : "", nm ? nm + 1 : vault.root, vault.n, hearts, time_s);
  char xml[1200], esc[600];
  size_t o = 0;
  for (const char *p = line; *p && o + 6 < sizeof esc; p++) {
    if (*p == '&') { memcpy(esc + o, "&amp;", 5); o += 5; }
    else if (*p == '<') { memcpy(esc + o, "&lt;", 4); o += 4; }
    else esc[o++] = *p;
  }
  esc[o] = 0;
  int n = snprintf(xml, sizeof xml,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><update-frame><frame id=\"frame3\"><liveitem><background>strip.png</background>"
    "<text valign=\"center\" align=\"left\" text-align=\"left\" text-valign=\"center\" word-wrap=\"off\" ellipsis=\"on\" x=\"36\">"
    "<str size=\"22\" color=\"#fff5f8\" shadow=\"on\">%s</str></text></liveitem></frame></update-frame>", esc);
  int r = sceLiveAreaUpdateFrameSync("01.00", xml, n, DATA "/livearea", 0);
  LOG("livearea update: %08x\n", r);
  (void)r;
}
