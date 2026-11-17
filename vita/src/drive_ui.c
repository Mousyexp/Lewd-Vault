// The Google Drive screen (drive.c does the work): the key, signing in with a QR code, a sync with its
// progress, and a vault fetched from Drive onto a Vita that has none. Compiled inside main.c.
#include "third_party/qrcodegen.h"

static int drive_return;      // (the screen to go back to)
static int drive_sel;
static int drive_was;         // (the state last frame: a sync just ended is noticed once)
static int drive_fetching;    // (a sync into a new folder from the picker)
static char drive_fetch_root[512];
static char drive_new_id[200];
static int drive_ask_secret;  // (the Client ID typed: the secret next)

static void open_drive(int from) {
  lv_net_base(); // (the rest on the worker that fetches)
  drive_return = from;
  drive_sel = 0;
  screen = S_DRIVE;
}
static const char *drive_folder_of_vault(void) { return vault.untraceable ? "Lewd Vault (encrypted)" : "Lewd Vault"; }

// The QR code, made once per address: black squares on white with the quiet zone round them.
static void draw_qr(const char *text, int x, int y, int size) {
  static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
  static char made[160];
  static int ok;
  if (strcmp(made, text)) {
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    ok = qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true);
    snprintf(made, sizeof made, "%s", text);
  }
  if (!ok) return;
  int n = qrcodegen_getSize(qr);
  int m = size / (n + 8); // (4 modules of white on each side)
  if (m < 1) m = 1;
  int full = m * (n + 8);
  rrect(x, y, full, full, 10, RGBA(255, 255, 255, 255));
  for (int r = 0; r < n; r++)
    for (int c = 0; c < n; c++)
      if (qrcodegen_getModule(qr, c, r)) vita2d_draw_rectangle(x + (c + 4) * m, y + (r + 4) * m, m, m, RGBA(0, 0, 0, 255));
}

// After a sync of the open vault: Drive's copies folded in and the vault read again.
static void reopen_after_sync(void) {
  uint8_t key[32];
  int has = vault.has_key;
  memcpy(key, vault.key, 32);
  char root[512];
  snprintf(root, sizeof root, "%s", vault.root);
  vault.views_dirty = 0; // (written before the sync; Drive's may be newer now)
  close_vault();
  pthread_mutex_lock(&vmx);
  lv_vault_init(&vault, root);
  if (has) { memcpy(vault.key, key, 32); vault.has_key = 1; }
  lv_vault_merge_conflicts(&vault, NULL, NULL);
  pthread_mutex_unlock(&vmx);
  memset(key, 0, sizeof key);
  load_state = 0;
  screen = S_UNLOCKING;
}

static int drive_button(int x, int y, int w, const char *label, int on, int primary) {
  rrect(x, y, w, 44, 22, primary ? (on ? ACC : with_alpha(ACC, 130)) : on ? SURF2 : SURF);
  if (on && !primary) rrect(x, y, 5, 44, 3, ACC);
  text_center(x + w / 2, y + 29, primary ? RGBA(20, 12, 16, 255) : TEXT, 17, label);
  return hit(x, y, w, 44);
}

static void draw_drive(void) {
  text(30, 44, TEXT, 26, "Google Drive");
  int busy = drive.state == DRV_SYNCING || drive.state == DRV_CODE;
  // a sync that just ended: the vault is read again (or, fetched from the picker, opened)
  if (drive_was == DRV_SYNCING && !drive.signing && (drive.state == DRV_DONE || drive.state == DRV_FAILED)) {
    if (drive_fetching) {
      drive_fetching = 0;
      sfx(drive.state == DRV_DONE ? SFX_DONE : SFX_ERROR);
      if (lv_is_vault(drive_fetch_root)) { find_vaults(); say(drive.message); open_vault(drive_fetch_root); drive_was = drive.state; return; }
    } else if (loaded_vault) {
      sfx(drive.state == DRV_DONE ? SFX_DONE : SFX_ERROR);
      if (drive.down || drive.conflicts || drive.here) { say(drive.message); drive_was = drive.state; reopen_after_sync(); return; }
    }
  }
  if (drive_was == DRV_CODE && drive.state == DRV_DONE) sfx(SFX_DONE);
  drive_was = drive.state;
  if (drive_ask_secret && !ime_open) { drive_ask_secret = 0; ime_start(8, "Client secret", "", 0); }

  int y = 80;
  if (!drive_has_key()) {
    // ---- no key yet ----
    const char *lines[] = {
      "Lewd Vault keeps a vault in step with the \"Lewd Vault\" folder your PC syncs to Google Drive.",
      "The Vita signs in with a code, so it needs its own key from your PC's Google Cloud project:",
      "1. console.cloud.google.com, the same project as the PC's key: Credentials, Create credentials,",
      "   OAuth client ID, type \"TVs and Limited Input devices\".",
      "2. Type its Client ID and Client secret here (or put them, one a line, in",
      "   ux0:data/LWDV00001/google.txt). It only ever reaches the files Lewd Vault made.",
    };
    for (int k = 0; k < 6; k++) text(30, y + k * 28, k < 2 ? DIM : FAINT, 16, lines[k]);
    y += 6 * 28 + 20;
    if (drive_button(30, y, 260, "Type the key", 1, 1) || ((in.pressed & SCE_CTRL_CROSS) && !ime_open)) ime_start(7, "Client ID (….apps.googleusercontent.com)", "", 0);
  } else if (drive.state == DRV_CODE) {
    // ---- signing in: the QR code and the code ----
    char url[160];
    snprintf(url, sizeof url, "%s", drive.url[0] ? drive.url : "https://www.google.com/device");
    draw_qr(url, 30, 76, 300);
    int x = 360;
    text(x, 110, DIM, 17, "On your phone, scan the code (or open");
    text(x, 136, ACC, 18, url);
    text(x, 166, DIM, 17, "then type this code and allow Lewd Vault:");
    text(x, 236, TEXT, 54, drive.user_code);
    int64_t left = (int64_t)(drive.code_until - sceKernelGetProcessTimeWide()) / 1000000;
    if (left < 0) left = 0;
    char b[80];
    snprintf(b, sizeof b, "Waiting for the phone…  the code lasts %lld:%02lld more", (long long)(left / 60), (long long)(left % 60));
    text(x, 280, FAINT, 15, b);
    text(x, 304, FAINT, 15, "It only reaches the files Lewd Vault made in your Drive.");
    if (drive_button(x, 340, 200, "Stop", 1, 0) || (in.pressed & SCE_CTRL_CIRCLE)) { drive_cancel(); in.pressed &= ~SCE_CTRL_CIRCLE; }
    in.pressed &= ~SCE_CTRL_CIRCLE;
  } else if (drive.state == DRV_SYNCING) {
    // ---- busy: asking for a code, or a sync ----
    spinner(44, y + 4, 12);
    text(66, y + 10, TEXT, 20, drive.message);
    if (!drive.signing) {
      float f = drive.total ? drive.done / (float)drive.total : 0;
      rrect(30, y + 40, W - 60, 12, 6, SURF2);
      rrect(30, y + 40, (W - 60) * f < 12 ? 12 : (W - 60) * f, 12, 6, ACC);
      char b[200];
      snprintf(b, sizeof b, "%d of %d  ·  %d up  ·  %d down%s", drive.done, drive.total, drive.up, drive.down,
               drive.failed ? "  ·  some failed" : "");
      text(30, y + 80, DIM, 16, b);
      if (drive.bytes_left > 0) { snprintf(b, sizeof b, "%lld MB still to come", (long long)(drive.bytes_left >> 20)); text(30, y + 104, FAINT, 15, b); }
      text_fit(30, y + 128, FAINT, 14, drive.file, W - 60);
      text(30, y + 170, FAINT, 15, "Keep the Vita awake and on Wi-Fi. ○ stops (what's done stays done).");
    }
    if (in.pressed & SCE_CTRL_CIRCLE) { drive_cancel(); say("Stopping…"); }
    in.pressed &= ~SCE_CTRL_CIRCLE;
  } else if (!drive_signed_in()) {
    // ---- signed out ----
    text(30, y, DIM, 17, "Sign in once: the Vita shows a QR code and a short code for your phone.");
    if (drive.message[0]) text_fit(30, y + 28, drive.state == DRV_FAILED ? ACC2 : FAINT, 15, drive.message, W - 60);
    y += 60;
    int n = 2;
    if (drive_button(30, y, 300, "Sign in with your phone", drive_sel == 0, 1) || ((in.pressed & SCE_CTRL_CROSS) && drive_sel == 0)) { sfx(SFX_OPEN); drive_sign_in(); }
    if (drive_button(30, y + 56, 300, "Change the key", drive_sel == 1, 0) || ((in.pressed & SCE_CTRL_CROSS) && drive_sel == 1)) ime_start(7, "Client ID", drive_key_id(), 0);
    if (in.pressed & SCE_CTRL_DOWN) drive_sel = (drive_sel + 1) % n;
    if (in.pressed & SCE_CTRL_UP) drive_sel = (drive_sel + n - 1) % n;
  } else {
    // ---- signed in ----
    char b[300];
    if (drive_last_sync()[0]) snprintf(b, sizeof b, "Signed in  ·  last sync %s", drive_last_sync()); else snprintf(b, sizeof b, "Signed in");
    text(30, y, DIM, 17, b);
    if (drive.message[0]) text_fit(30, y + 26, drive.state == DRV_FAILED ? ACC2 : FAINT, 15, drive.message, W - 60);
    y += 56;
    const char *opts[4];
    int n = 0;
    char l1[120], l2[120];
    int fetch = drive_return == S_PICK || !loaded_vault; // (from the picker: a vault from Drive, not this one)
    if (!fetch) { snprintf(l1, sizeof l1, "Sync now with “%s”", drive_folder_of_vault()); opts[n++] = l1; }
    else {
      opts[n++] = "Get “Lewd Vault” from Drive";
      snprintf(l2, sizeof l2, "Get “Lewd Vault (encrypted)”");
      opts[n++] = l2;
    }
    int sign_out = n;
    opts[n++] = "Sign out";
    if (drive_sel >= n) drive_sel = 0;
    int go = -1;
    for (int k = 0; k < n; k++) if (drive_button(30, y + k * 56, 440, opts[k], drive_sel == k, k == 0)) go = k;
    if (in.pressed & SCE_CTRL_CROSS) go = drive_sel;
    if (in.pressed & SCE_CTRL_DOWN) drive_sel = (drive_sel + 1) % n;
    if (in.pressed & SCE_CTRL_UP) drive_sel = (drive_sel + n - 1) % n;
    text(500, y + 18, FAINT, 14, "Both ways, as on the PC: what changed on one side goes to");
    text(500, y + 38, FAINT, 14, "the other. When both changed a file, the Vita's copy wins");
    text(500, y + 58, FAINT, 14, "and Drive's is kept beside it (the lists are merged).");
    text(500, y + 78, FAINT, 14, "Nothing is deleted on a first sync.");
    if (go == sign_out) { sfx(SFX_CLOSE); drive_sign_out(); drive_sel = 0; }
    else if (go >= 0 && !fetch) {
      // everything written first, then nothing more until it's done
      save_edits_now();
      views_save_async();
      saver_flush();
      resume_save();
      sfx(SFX_START);
      drive_sync(vault.root, drive_folder_of_vault(), 1);
    } else if (go >= 0) {
      const char *folder = go == 0 ? "Lewd Vault" : "Lewd Vault (encrypted)";
      sceIoMkdir("ux0:data/LewdVault", 0777);
      snprintf(drive_fetch_root, sizeof drive_fetch_root, "ux0:data/LewdVault/%s (Drive)", go == 0 ? "Lewd Vault" : "Encrypted vault");
      drive_fetching = 1;
      sfx(SFX_START);
      drive_sync(drive_fetch_root, folder, 0);
    }
  }
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (!busy && (in.pressed & SCE_CTRL_CIRCLE)) { sfx(SFX_BACK); screen = drive_return; if (screen == S_PICK) find_vaults(); }
  if (loaded_vault && drive_return != S_PICK) draw_bar();
}
