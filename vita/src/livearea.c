// The LiveArea page (sce_sys/livearea/contents/template.xml): its left frame opens the Google Drive
// vault and its right frame the vault on the card, straight away. Compiled inside main.c (#include "livearea.c").
//
// A frame's target is psgm:play?titleid=LWDV00001&param=drive (or local): the system starts the app
// with that param (sceAppMgrGetAppParam), or, when it's already running, sends it an app event.
// "The Google Drive vault" is the one Drive's "Get it from Drive" made (its folder ends in "(Drive)"),
// else one that has been synced with Drive; "the vault on the card" is the last one opened that isn't.
#include <psp2/appmgr.h>

static void close_viewer(void); // (main.c)
static int launch_to; // 0 as usual, 1 the Drive vault, 2 the vault on the card
static int la_from_frame; // (started by a frame: the test kit then skips its menu)
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
  LOG("livearea event: %s -> %d\n", b, t);
  if (!t || screen == S_PIN || screen == S_CALC || screen == S_COVER || screen == S_UNLOCKING || decoy_session) return;
  const char *v = la_vault(t);
  if (!v[0]) { if (t == 1) open_drive(screen); return; }
  if (loaded_vault && !strcmp(v, vault.root)) { screen = S_LIBRARY; return; }
  if (screen == S_VIEWER) close_viewer();
  save_edits_now();
  resume_save();
  open_vault(v);
}

// ---------- the strip there was along the bottom ----------
// (an install from before kept the vault's numbers the app wrote into frame3: emptied once, so the
//  page is just its picture, the gate and the two frames)
int sceLiveAreaUpdateFrameSync(const char *formatVer, const char *frameXmlStr, int frameXmlStrLen, const char *dirpathTop, unsigned int flag);
static void la_update_stats(void) {
  static int done;
  if (done) return;
  done = 1;
  SceIoStat st;
  if (sceIoGetstat(DATA "/livearea/strip.png", &st) < 0) return; // (never written: nothing to empty)
  sceSysmoduleLoadModule(SCE_SYSMODULE_LIVEAREA);
  static const char xml[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?><update-frame><frame id=\"frame3\"><liveitem></liveitem></frame></update-frame>";
  int r = sceLiveAreaUpdateFrameSync("01.00", xml, (int)sizeof xml - 1, DATA "/livearea", 0);
  LOG("livearea strip emptied: %08x\n", r);
  if (r >= 0) { sceIoRemove(DATA "/livearea/strip.png"); sceIoRmdir(DATA "/livearea"); }
}
