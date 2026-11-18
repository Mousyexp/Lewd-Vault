// The phone's PIN lock and its stats page, on the Vita. Compiled inside main.c (#include "pin.c").
//
// PIN lock: a PIN pad when the app starts and (by choice) when the Vita wakes. A decoy PIN opens
// another vault instead (a harmless one, chosen in Settings), and nothing on the screen tells the two
// apart. Wrong tries are counted and told after the next right one; after five, it waits 30 s between
// tries. The PINs are kept as salted, many-times-hashed SHA-256 in ux0:data/LWDV00001/pin.txt. It's a
// lock on the app, not encryption: an Untraceable vault is what keeps the files themselves safe.
//
// Stats: what the vault holds and how it's been looked at, from index.csv and views.csv.

static struct {
  int on, on_wake;
  char hash[65], salt[33], decoy_hash[65], decoy_vault[512];
  int wrong;          // (wrong tries since the last right one)
  long long wrong_at; // (the last one's time)
} pin;
static char pin_typed[8];
static int pin_sel = 4;       // (the key the D-pad is on: 0-9 are the digits' keys, 10 back, 11 OK)
static int pin_return = -1;   // (the screen behind the lock; -1: the app starting)
static unsigned pin_wait_until;
static int pin_fail_shake;

static void pin_hash(const char *p, const char *salt, char out[65]) {
  uint8_t d[32];
  char buf[128];
  snprintf(buf, sizeof buf, "lewd-vault/pin/%s/%s", salt, p);
  lv_sha256((const uint8_t *)buf, strlen(buf), d);
  for (int k = 0; k < 4999; k++) { uint8_t x[64]; memcpy(x, d, 32); memcpy(x + 32, salt, 32); lv_sha256(x, 64, d); }
  for (int k = 0; k < 32; k++) snprintf(out + 2 * k, 3, "%02x", d[k]);
}
static void pin_save(void) {
  FILE *f = fopen(DATA "/pin.txt.part", "w");
  if (!f) return;
  fprintf(f, "%d\n%d\n%s\n%s\n%s\n%s\n%d\n%lld\n", pin.on, pin.on_wake, pin.hash, pin.salt, pin.decoy_hash, pin.decoy_vault, pin.wrong, pin.wrong_at);
  fclose(f);
  sceIoRemove(DATA "/pin.txt");
  sceIoRename(DATA "/pin.txt.part", DATA "/pin.txt");
}
// (a whole line, whatever the room: a hash that fills its buffer mustn't leave its newline for the next)
static void pin_line(FILE *f, char *o, size_t cap) {
  char b[700];
  o[0] = 0;
  if (!fgets(b, sizeof b, f)) return;
  b[strcspn(b, "\r\n")] = 0;
  snprintf(o, cap, "%s", b);
}
static void pin_load(void) {
  FILE *f = fopen(DATA "/pin.txt", "r");
  if (!f) return;
  char b[600];
  pin_line(f, b, sizeof b); pin.on = atoi(b);
  pin_line(f, b, sizeof b); pin.on_wake = atoi(b);
  pin_line(f, pin.hash, sizeof pin.hash);
  pin_line(f, pin.salt, sizeof pin.salt);
  pin_line(f, pin.decoy_hash, sizeof pin.decoy_hash);
  pin_line(f, pin.decoy_vault, sizeof pin.decoy_vault);
  pin_line(f, b, sizeof b); pin.wrong = atoi(b);
  pin_line(f, b, sizeof b); pin.wrong_at = atoll(b);
  fclose(f);
  if (strlen(pin.hash) != 64 || !pin.salt[0]) pin.on = 0;
}
static void pin_set(const char *p, int decoy) {
  if (!pin.salt[0]) {
    uint8_t r[16];
    lv_random(r, sizeof r);
    for (int k = 0; k < 16; k++) snprintf(pin.salt + 2 * k, 3, "%02x", r[k]);
  }
  pin_hash(p, pin.salt, decoy ? pin.decoy_hash : pin.hash);
  if (!decoy) pin.on = 1;
  pin_save();
}
// The lock goes up: in front of the screen it's on (or before the vault opens, at start).
static void pin_lock(int from) {
  if (!pin.on) return;
  pin_return = from;
  pin_typed[0] = 0;
  pin_sel = 4;
  screen = S_PIN;
}

static void pin_try(void) {
  if (frame_no < pin_wait_until) return;
  char h[65];
  pin_hash(pin_typed, pin.salt, h);
  int ok = !strcmp(h, pin.hash), decoy = !ok && pin.decoy_hash[0] && !strcmp(h, pin.decoy_hash);
  pin_typed[0] = 0;
  if (!ok && !decoy) {
    sfx(SFX_ERROR);
    pin_fail_shake = 20;
    pin.wrong++;
    pin.wrong_at = (long long)time(NULL);
    pin_save();
    if (pin.wrong >= 5) pin_wait_until = frame_no + 30 * 60;
    return;
  }
  sfx(SFX_START);
  if (decoy) {
    // the decoy's vault, as if it were the one (wrong tries aren't told: whoever this is)
    decoy_session = 1;
    if (pin.decoy_vault[0] && lv_is_vault(pin.decoy_vault)) { open_vault(pin.decoy_vault); return; }
    screen = S_PICK;
    find_vaults();
    return;
  }
  if (decoy_session) {
    // (the real PIN after a decoy: back to the real vault)
    decoy_session = 0;
    find_vaults();
    if (cfg.vault[0] && lv_is_vault(cfg.vault)) { open_vault(cfg.vault); return; }
  }
  if (pin.wrong) {
    char m[120], when[32];
    struct tm l;
    lv_local_tm((time_t)pin.wrong_at, &l);
    strftime(when, sizeof when, "%d %b %H:%M", &l);
    snprintf(m, sizeof m, "%d wrong %s while you were away (the last at %s)", pin.wrong, pin.wrong == 1 ? "PIN" : "PINs", when);
    say(m);
    pin.wrong = 0;
    pin_save();
  }
  if (pin_return < 0) {
    // the app starting: now the vault (the one the LiveArea asked for, if it did)
    find_vaults();
    open_start_vault();
  } else screen = pin_return;
}

static void draw_pin(void) {
  // (the background: the frame is cleared to it already)
  float shake = pin_fail_shake > 0 ? sinf(pin_fail_shake * 1.7f) * pin_fail_shake * 0.8f : 0;
  if (pin_fail_shake > 0) pin_fail_shake--;
  text_center(W / 2 - 230, 150, TEXT, 26, "Lewd Vault");
  text_center(W / 2 - 230, 186, DIM, 17, "Enter your PIN");
  for (int k = 0; k < 6; k++) {
    int filled = k < (int)strlen(pin_typed);
    circle(W / 2 - 230 - 75 + k * 30 + shake, 236, filled ? 9 : 8, filled ? ACC : with_alpha(TEXT, 60));
  }
  if (frame_no < pin_wait_until) {
    char m[64];
    snprintf(m, sizeof m, "Too many tries: wait %u s", (pin_wait_until - frame_no) / 60 + 1);
    text_center(W / 2 - 230, 290, ACC2, 16, m);
  }
  // the pad: 1-9, then back, 0, OK
  static const char *keys[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "⌫", "0", "OK"};
  int px = W / 2 + 40, py = 70, kw = 110, kh = 92;
  for (int k = 0; k < 12; k++) {
    int x = px + (k % 3) * (kw + 10), y = py + (k / 3) * (kh + 10);
    int on = k == pin_sel;
    rrect(x, y, kw, kh, 18, on ? with_alpha(ACC, 200) : SURF);
    text_center(x + kw / 2, y + kh / 2 + 11, on ? RGBA(20, 12, 16, 255) : TEXT, 30, keys[k]);
    if (hit(x, y, kw, kh)) { pin_sel = k; in.pressed |= SCE_CTRL_CROSS; }
  }
  if (in.pressed & SCE_CTRL_RIGHT) pin_sel = pin_sel % 3 == 2 ? pin_sel - 2 : pin_sel + 1;
  if (in.pressed & SCE_CTRL_LEFT) pin_sel = pin_sel % 3 == 0 ? pin_sel + 2 : pin_sel - 1;
  if (in.pressed & SCE_CTRL_DOWN) pin_sel = (pin_sel + 3) % 12;
  if (in.pressed & SCE_CTRL_UP) pin_sel = (pin_sel + 9) % 12;
  if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_LEFT | SCE_CTRL_UP | SCE_CTRL_DOWN)) sfx(SFX_TICK);
  int n = (int)strlen(pin_typed);
  if (in.pressed & SCE_CTRL_CROSS) {
    if (pin_sel == 9) { if (n) pin_typed[n - 1] = 0; sfx(SFX_BACK); }
    else if (pin_sel == 11) pin_try();
    else if (n < 6) { pin_typed[n] = (char)(pin_sel == 10 ? '0' : '1' + pin_sel); pin_typed[n + 1] = 0; sfx(SFX_TICK); }
  }
  if ((in.pressed & SCE_CTRL_CIRCLE) && n) { pin_typed[n - 1] = 0; sfx(SFX_BACK); }
  if (in.pressed & SCE_CTRL_START) pin_try();
  // (six digits: tried at once)
  if (strlen(pin_typed) == 6) pin_try();
}

// ---------- stats ----------
static void draw_stats(void) {
  text(30, 44, TEXT, 26, "Your stats");
  // (the totals: worked out on opening, and again only if the vault changed, not every frame over
  //  every item)
  static long long views, hearts;
  static double secs;
  static int kinds[3], never, top[5], s_n = -1, s_nviews = -1, s_vgen = -1;
  static unsigned s_frame;
  int fresh = frame_no - s_frame > 2 || s_n != vault.n || s_nviews != vault.nviews || s_vgen != vgen;
  s_frame = frame_no;
  if (fresh) {
    s_n = vault.n; s_nviews = vault.nviews; s_vgen = vgen;
    views = hearts = 0; secs = 0; never = 0; kinds[0] = kinds[1] = kinds[2] = 0;
    for (int i = 0; i < vault.n; i++) {
      kinds[vault.hot[i].kind < 3 ? vault.hot[i].kind : 0]++;
      if (!lv_views_of(&vault, &vault.items[i])) never++;
    }
    for (int k = 0; k < vault.nviews; k++) { views += vault.views[k].views; hearts += vault.views[k].likes; secs += vault.views[k].seconds; }
    for (int k = 0; k < 5; k++) top[k] = -1;
    for (int i = 0; i < vault.n; i++) {
      long long v = lv_views_of(&vault, &vault.items[i]);
      if (!v) continue;
      for (int k = 0; k < 5; k++) {
        if (top[k] < 0 || v > lv_views_of(&vault, &vault.items[top[k]])) { memmove(&top[k + 1], &top[k], (4 - k) * sizeof *top); top[k] = i; break; }
      }
    }
  }
  char b[160];
  int y = 92;
  struct { const char *name; char v[64]; } tiles[6];
  snprintf(tiles[0].v, 64, "%d", vault.n); tiles[0].name = "items";
  snprintf(tiles[1].v, 64, "%d · %d · %d", kinds[0], kinds[1], kinds[2]); tiles[1].name = "pictures · GIFs · videos";
  snprintf(tiles[2].v, 64, "%lld", views); tiles[2].name = "times opened";
  snprintf(tiles[3].v, 64, "%lld", hearts); tiles[3].name = "hearts";
  if (secs >= 3600) snprintf(tiles[4].v, 64, "%d h %d min", (int)(secs / 3600), (int)secs % 3600 / 60);
  else snprintf(tiles[4].v, 64, "%d min", (int)(secs / 60));
  tiles[4].name = "spent looking";
  snprintf(tiles[5].v, 64, "%d", never); tiles[5].name = "never opened";
  for (int k = 0; k < 6; k++) {
    int x = 30 + (k % 3) * 304, ty = y + (k / 3) * 92;
    rrect(x, ty, 292, 80, 16, SURF);
    text(x + 18, ty + 40, ACC, 26, tiles[k].v);
    text(x + 18, ty + 66, DIM, 15, tiles[k].name);
  }
  // the most opened, and the categories
  y += 200;
  text(30, y, DIM, 16, "Most opened");
  for (int k = 0; k < 5 && top[k] >= 0; k++) {
    const LvItem *it = &vault.items[top[k]];
    snprintf(b, sizeof b, "%lld×", lv_views_of(&vault, it));
    text(30, y + 28 + k * 26, ACC, 15, b);
    text_fit(80, y + 28 + k * 26, TEXT, 15, it->name[0] ? it->name : it->file, 380);
  }
  text(490, y, DIM, 16, "Categories");
  for (int k = 0; k < vault.ncats && k < 5; k++) {
    float f = vault.n ? vault.cat_counts[k] / (float)vault.n : 0;
    rrect(490, y + 14 + k * 26, 300 * f + 6, 18, 9, with_alpha(ACC, 150));
    snprintf(b, sizeof b, "%s  %d", vault.cats[k], vault.cat_counts[k]);
    text(500, y + 28 + k * 26, TEXT, 14, b);
  }
  if (ss.on && ss.nst) {
    snprintf(b, sizeof b, "Slideshow session: level %d reached", ss.st_hi[stage_now()]);
    text(30, H - BAR - 14, FAINT, 14, b);
  }
  if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_BACK); screen = S_SETTINGS; }
  draw_bar();
}
