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
  int len, decoy_len;  // (how many digits each has: the pad's dots; 0 not known, from before it was kept)
} pin;
static char pin_typed[8];
static int pin_sel = 4;       // (the key the D-pad is on: 0-9 are the digits' keys, 10 back, 11 OK)
static int pin_return = -1;   // (the screen behind the lock; -1: the app starting)
static unsigned pin_wait_until;
static int pin_fail_shake;
// (the animations: when the lock went up, when each dot and key last changed, the unlock's start)
static unsigned pin_shown, pin_dot_at[6], pin_dot_gone_at[6], pin_key_at[12], pin_ok_at;
static int pin_ok_kind; // (0 none; 1 the PIN, 2 the decoy's: what happens once the unlock has played)
static void pin_finish(void);

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
  fprintf(f, "%d\n%d\n%s\n%s\n%s\n%s\n%d\n%lld\n%d\n%d\n", pin.on, pin.on_wake, pin.hash, pin.salt, pin.decoy_hash, pin.decoy_vault, pin.wrong, pin.wrong_at, pin.len, pin.decoy_len);
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
  pin_line(f, b, sizeof b); pin.len = atoi(b);
  pin_line(f, b, sizeof b); pin.decoy_len = atoi(b);
  if (pin.len < 4 || pin.len > 6) pin.len = 0;
  if (pin.decoy_len < 4 || pin.decoy_len > 6) pin.decoy_len = 0;
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
  if (decoy) pin.decoy_len = (int)strlen(p); else pin.len = (int)strlen(p);
  // (turned on: it locks when the Vita wakes too, by default; a new PIN keeps the choice made)
  if (!decoy) { if (!pin.on) pin.on_wake = 1; pin.on = 1; }
  pin_save();
}
// The lock goes up: in front of the screen it's on (or before the vault opens, at start).
static void pin_lock(int from) {
  if (!pin.on) return;
  pin_return = from;
  pin_typed[0] = 0;
  pin_shown = frame_no; pin_ok_at = 0; pin_ok_kind = 0;
  memset(pin_dot_at, 0, sizeof pin_dot_at); memset(pin_dot_gone_at, 0, sizeof pin_dot_gone_at);
  pin_sel = 4;
  screen = S_PIN;
}

static void pin_try(void) {
  if (frame_no < pin_wait_until || pin_ok_kind) return;
  char h[65];
  pin_hash(pin_typed, pin.salt, h);
  int ok = !strcmp(h, pin.hash), decoy = !ok && pin.decoy_hash[0] && !strcmp(h, pin.decoy_hash);
  if (!ok && !decoy) {
    for (int k = 0; k < (int)strlen(pin_typed) && k < 6; k++) pin_dot_gone_at[k] = frame_no; // (they fall away)
    pin_typed[0] = 0;
    sfx(SFX_ERROR);
    pin_fail_shake = 20;
    pin.wrong++;
    pin.wrong_at = (long long)time(NULL);
    pin_save();
    if (pin.wrong >= 5) pin_wait_until = frame_no + 30 * 60;
    return;
  }
  sfx(SFX_START);
  // (right: the dots light up in turn and the page fades, then the vault: pin_finish)
  pin_ok_at = frame_no ? frame_no : 1;
  pin_ok_kind = decoy ? 2 : 1;
}
static void pin_finish(void) {
  int decoy = pin_ok_kind == 2;
  pin_ok_kind = 0; pin_ok_at = 0;
  pin_typed[0] = 0; // (the digits gone from memory once it's open)
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

static float pin_clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float pin_out(float t) { t = pin_clamp01(t); return 1 - (1 - t) * (1 - t) * (1 - t); }             // (ease out)
static float pin_back(float t) { t = pin_clamp01(t); float c = 1.9f; return 1 + (c + 1) * powf(t - 1, 3) + c * powf(t - 1, 2); } // (a little past, then back)
// The keys' pictures, made once at the size they're drawn (no scaling, so their edges stay smooth):
// the key (a disc), its rim (light from above: bright at the top, faint at the bottom) and its sheen
// (a soft glow across its top half). White, tinted as they're drawn.
#define PIN_KD 80 // (a key's diameter)
static vita2d_texture *pin_key_tex, *pin_rim_tex, *pin_sheen_tex;
static void pin_make_keys(void) {
  if (pin_key_tex) return;
  const int S = PIN_KD + 4;
  pin_key_tex = tex_new(S, S); pin_rim_tex = tex_new(S, S); pin_sheen_tex = tex_new(S, S);
  if (!pin_key_tex || !pin_rim_tex || !pin_sheen_tex) return;
  vita2d_texture *ts[3] = {pin_key_tex, pin_rim_tex, pin_sheen_tex};
  for (int t = 0; t < 3; t++) {
    uint8_t *d = vita2d_texture_get_datap(ts[t]);
    unsigned stride = vita2d_texture_get_stride(ts[t]);
    for (int y = 0; y < S; y++)
      for (int x = 0; x < S; x++) {
        float dx = x + 0.5f - S / 2.0f, dy = y + 0.5f - S / 2.0f, r = PIN_KD / 2.0f, dist = sqrtf(dx * dx + dy * dy);
        float inside = fminf(fmaxf(r - dist + 0.5f, 0), 1); // (one pixel of soft edge)
        float al = 0;
        if (t == 0) al = inside;
        else if (t == 1) { // (a 1.5 px ring, brighter at the top)
          float ring = fminf(fmaxf(1.5f - fabsf(dist - (r - 1.0f)) + 0.5f, 0), 1);
          al = ring * (0.35f + 0.65f * fminf(fmaxf(0.5f - dy / (2 * r), 0), 1));
        } else { // (a soft glow in the top half, falling away to nothing by the middle)
          float up = fminf(fmaxf(-dy / r, 0), 1);
          al = inside * up * up * 0.9f;
        }
        uint8_t *px = d + y * stride + x * 4;
        px[0] = px[1] = px[2] = 255;
        px[3] = (uint8_t)(al * 255);
      }
    vita2d_texture_set_filters(ts[t], SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
  }
}
static void pin_tex_at(vita2d_texture *t, float cx, float cy, float sc, unsigned col) {
  if (!t) return;
  float S = vita2d_texture_get_width(t);
  vita2d_draw_texture_tint_scale(t, cx - S * sc / 2, cy - S * sc / 2, sc, sc, col);
}
static void draw_pin(void) {
  pin_make_keys();
  // (the background: the frame is cleared to it already) soft lights drifting behind it all
  for (int k = 0; k < 7; k++) {
    float ph = frame_no * (0.004f + k * 0.0011f) + k * 1.7f;
    float bx = W * (0.12f + 0.13f * k) + sinf(ph) * 40, by = H * (0.25f + 0.09f * (k % 5)) + cosf(ph * 1.3f) * 30;
    circle(bx, by, 40 + (k % 3) * 22, with_alpha(ACC, 5 + (k % 3) * 2));
  }
  unsigned since = frame_no - pin_shown;
  float in_t = pin_out(since / 24.0f); // (the page easing in)
  float ok_t = pin_ok_at ? (frame_no - pin_ok_at) / 40.0f : 0; // (the unlock: 0 to 1)
  if (pin_ok_at && ok_t >= 1) { pin_finish(); return; }
  float shake = pin_fail_shake > 0 ? sinf(pin_fail_shake * 1.7f) * pin_fail_shake * 0.8f : 0;
  if (pin_fail_shake > 0) pin_fail_shake--;
  float fade = pin_ok_at ? 1 - pin_out((ok_t - 0.45f) / 0.55f) : 1; // (the page fading as it opens)
  int a = (int)(255 * in_t * fade);
  // Centred, as a phone's: the heart, the title and the dots along the top, the pad below
  const float cx = W / 2.0f, rise = (1 - in_t) * 24;
  // a heart above the title: breathing; a beat when a digit goes in; a bigger one as it opens
  {
    int n0 = (int)strlen(pin_typed);
    unsigned last = 0;
    for (int k = 0; k < n0 && k < 6; k++) if (pin_dot_at[k] > last) last = pin_dot_at[k];
    float beat = last ? fmaxf(0, 1 - (frame_no - last) / 12.0f) : 0;
    float sc = 1 + 0.05f * sinf(frame_no * 0.06f) + 0.18f * beat + (pin_ok_at ? 0.6f * pin_out(ok_t) : 0);
    circle(cx, 38 + rise, 22 * sc, with_alpha(ACC, (int)(36 * in_t * fade)));
    icon("heart_fill", cx, 38 + rise, with_alpha(ACC, a)); // (the icon at its own size: crisp)
  }
  const char *title = pin_ok_at ? "Welcome back" : frame_no < pin_wait_until ? "Too many tries" : "Enter your PIN";
  text_center(cx, 84 + rise, with_alpha(TEXT, a), 20, title);
  // the dots: each pops in when typed, drops away when taken back or wrong; lit in turn when right
  int n = (int)strlen(pin_typed);
  const float dy0 = 112 + rise, dgap = 24;
  int nd = pin.len ? pin.len : n > 4 ? n : 4; // (as many as the PIN has; not known: as typed, at least four)
  if (pin_ok_at && !pin.len) nd = n > 0 ? n : 4;
  for (int k = 0; k < nd; k++) {
    float dx = cx - dgap * (nd - 1) / 2.0f + k * dgap + shake;
    int filled = k < n || pin_ok_at;
    circle(dx, dy0, 6, with_alpha(TEXT, (int)(70 * in_t * fade)));
    circle(dx, dy0, 4.5f, with_alpha(BG, a)); // (an open ring until it's typed)
    if (filled) {
      float pop = pin_dot_at[k] ? pin_back((frame_no - pin_dot_at[k]) / 10.0f) : 1;
      unsigned col = pin_fail_shake > 0 ? ACC2 : ACC;
      if (pin_ok_at) { float w = pin_clamp01(ok_t * 6 - k * 0.5f); pop = 1 + 0.35f * sinf(w * 3.14159f); col = mix_col(ACC, TEXT, w * 0.6f); }
      circle(dx, dy0, 7 * pop, with_alpha(col, a));
    } else if (pin_dot_gone_at[k] && frame_no - pin_dot_gone_at[k] < 14) {
      float g = (frame_no - pin_dot_gone_at[k]) / 14.0f;
      circle(dx, dy0 + g * g * 22, 7 * (1 - g), with_alpha(pin_fail_shake > 0 ? ACC2 : ACC, (int)(a * (1 - g))));
    }
  }
  if (frame_no < pin_wait_until) {
    // (a bar running down the wait, under the dots)
    float left = (pin_wait_until - frame_no) / (30.0f * 60);
    rrect_round(cx - 70, dy0 + 18, 140, 4, 2, with_alpha(TEXT, a / 6));
    rrect_round(cx - 70, dy0 + 18, 140 * left, 4, 2, with_alpha(ACC2, a));
    char m[48];
    snprintf(m, sizeof m, "wait %u s", (pin_wait_until - frame_no) / 60 + 1);
    text_center(cx, dy0 + 40, with_alpha(ACC2, a), 13, m);
  }
  // the pad: 1-9, then back, 0, OK; round glass keys that come in one after another and squash when pressed
  static const char *keys[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "⌫", "0", "OK"};
  const float colx = 108, rowy = 90, top = 192;
  for (int k = 0; k < 12; k++) {
    float kin = pin_out((since - (k % 3) * 2 - (k / 3) * 3) / 18.0f);
    float kcx = cx + ((k % 3) - 1) * colx, kcy = top + (k / 3) * rowy + (1 - kin) * 36;
    int on = k == pin_sel && !pin_ok_at, digit = k != 9 && k != 11;
    float pr = pin_key_at[k] ? (frame_no - pin_key_at[k]) / 12.0f : 1; // (0 just pressed .. 1 done)
    float sq = pr < 1 ? 1 - 0.07f * sinf(pin_clamp01(pr) * 3.14159f) : 1;
    int ka = (int)(255 * kin * fade);
    if (pr < 1) { // (a ripple from the key)
      float rr = pin_out(pr);
      pin_tex_at(pin_key_tex, kcx, kcy, sq + rr * 0.35f, with_alpha(ACC, (int)(55 * (1 - pr) * kin)));
    }
    if (on) pin_tex_at(pin_key_tex, kcx, kcy, sq * 1.12f, with_alpha(ACC, ka / 5)); // (a glow round the chosen one)
    if (digit || on) {
      unsigned body = on ? mix_col(SURF2, ACC, 0.85f) : mix_col(SURF2, ACC, 0.10f);
      if (pr < 1) body = mix_col(body, TEXT, 0.25f * (1 - pr));
      pin_tex_at(pin_key_tex, kcx, kcy, sq, with_alpha(body, digit ? ka * 235 / 255 : ka * 200 / 255));
      pin_tex_at(pin_sheen_tex, kcx, kcy, sq, with_alpha(TEXT, ka / 9));
      pin_tex_at(pin_rim_tex, kcx, kcy, sq, with_alpha(TEXT, on ? ka / 2 : ka / 4));
    }
    unsigned tc = on ? RGBA(20, 12, 16, 255) : digit ? TEXT : DIM;
    text_center(kcx, kcy + (digit ? 12 : 7), with_alpha(tc, ka), digit ? 32 : 19, keys[k]);
    if (!pin_ok_at && in.tap && (in.tap_x - kcx) * (in.tap_x - kcx) + (in.tap_y - kcy) * (in.tap_y - kcy) < 44 * 44) { pin_sel = k; in.pressed |= SCE_CTRL_CROSS; }
  }
  if (pin_ok_at) return; // (opening: no more keys)
  if (in.pressed & SCE_CTRL_RIGHT) pin_sel = pin_sel % 3 == 2 ? pin_sel - 2 : pin_sel + 1;
  if (in.pressed & SCE_CTRL_LEFT) pin_sel = pin_sel % 3 == 0 ? pin_sel + 2 : pin_sel - 1;
  if (in.pressed & SCE_CTRL_DOWN) pin_sel = (pin_sel + 3) % 12;
  if (in.pressed & SCE_CTRL_UP) pin_sel = (pin_sel + 9) % 12;
  if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_LEFT | SCE_CTRL_UP | SCE_CTRL_DOWN)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_CROSS) {
    pin_key_at[pin_sel] = frame_no ? frame_no : 1;
    if (pin_sel == 9) { if (n) { pin_dot_gone_at[n - 1] = frame_no; pin_typed[n - 1] = 0; } sfx(SFX_BACK); }
    else if (pin_sel == 11) pin_try();
    else if (n < 6) { pin_typed[n] = (char)(pin_sel == 10 ? '0' : '1' + pin_sel); pin_typed[n + 1] = 0; pin_dot_at[n] = frame_no ? frame_no : 1; sfx(SFX_TICK); }
  }
  if ((in.pressed & SCE_CTRL_CIRCLE) && n) { pin_dot_gone_at[n - 1] = frame_no; pin_typed[n - 1] = 0; pin_key_at[9] = frame_no ? frame_no : 1; sfx(SFX_BACK); }
  if (in.pressed & SCE_CTRL_START) pin_try();
  // (six digits: tried at once, once its pop has shown)
  // (all its digits: tried at once, once the last one's pop has shown; the decoy's length too)
  int tn = (int)strlen(pin_typed);
  if (tn && (tn == 6 || tn == pin.len || tn == pin.decoy_len) && frame_no - pin_dot_at[tn - 1] > 6) pin_try();
}

// ---------- stats ----------
static void draw_stats(void) {
  text(30, 44, TEXT, 26, "Your stats");
  // (the totals: worked out on opening, and again only if the vault changed, not every frame over
  //  every item)
  static long long views, hearts;
  static double secs;
  static int kinds[3], never, top[5], months[12], month_lbl[12], s_n = -1, s_nviews = -1, s_vgen = -1;
  static unsigned s_frame, opened_at;
  int fresh = frame_no - s_frame > 2 || s_n != vault.n || s_nviews != vault.nviews || s_vgen != vgen;
  if (frame_no - s_frame > 2) opened_at = frame_no;
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
    // (the last twelve months: how many were added in each; added is yyyymmddhhmmss)
    time_t now = time(NULL);
    struct tm l;
    lv_local_tm(now, &l);
    int ym_now = (l.tm_year + 1900) * 12 + l.tm_mon;
    for (int k = 0; k < 12; k++) { months[k] = 0; month_lbl[k] = (ym_now - 11 + k) % 12; }
    for (int i = 0; i < vault.n; i++) {
      int64_t ad = vault.hot[i].added;
      int y = (int)(ad / 10000000000LL), m = (int)(ad / 100000000LL % 100);
      int idx = y * 12 + (m - 1) - (ym_now - 11);
      if (y > 1900 && m >= 1 && m <= 12 && idx >= 0 && idx < 12) months[idx]++;
    }
  }
  float grow = fminf(1, (frame_no - opened_at) / 30.0f);
  grow = 1 - (1 - grow) * (1 - grow) * (1 - grow); // (the bars rise as it opens)
  char b[160];
  // the totals, small, across the top
  {
    char v[4][48];
    const char *nm[4] = {"items", "times opened", "hearts", "spent looking"};
    snprintf(v[0], 48, "%d", vault.n);
    snprintf(v[1], 48, "%lld", views);
    snprintf(v[2], 48, "%lld", hearts);
    if (secs >= 3600) snprintf(v[3], 48, "%d h %d min", (int)(secs / 3600), (int)secs % 3600 / 60);
    else snprintf(v[3], 48, "%d min", (int)(secs / 60));
    for (int k = 0; k < 4; k++) {
      int x = 30 + k * 228, ty = 60;
      rrect(x, ty, 216, 62, 16, SURF);
      text(x + 16, ty + 32, ACC, 22, v[k]);
      text(x + 16, ty + 52, DIM, 13, nm[k]);
    }
  }
  // added, month by month: twelve bars
  {
    int x0 = 30, y0 = 134, pw = 560, ph = 186;
    rrect(x0, y0, pw, ph, 18, SURF);
    text(x0 + 16, y0 + 28, TEXT, 16, "Added, month by month");
    int mx = 1;
    for (int k = 0; k < 12; k++) if (months[k] > mx) mx = months[k];
    static const char *const MON = "JFMAMJJASOND";
    float bw = (pw - 32 - 11 * 10) / 12.0f, base = y0 + ph - 26, hmax = ph - 74;
    for (int k = 0; k < 12; k++) {
      float bx = x0 + 16 + k * (bw + 10), bh = months[k] ? fmaxf(6, hmax * months[k] / mx * grow) : 3;
      rrect_round(bx, base - hmax, bw, hmax, 6, with_alpha(TEXT, 10));
      rrect_round(bx, base - bh, bw, bh, fminf(6, bh / 2), k == 11 ? ACC : with_alpha(ACC, 150));
      if (months[k]) { snprintf(b, sizeof b, "%d", months[k]); text_center((int)(bx + bw / 2), (int)(base - bh - 6), DIM, 11, b); }
      char ml[2] = {MON[month_lbl[k]], 0};
      text_center((int)(bx + bw / 2), (int)base + 18, k == 11 ? TEXT : FAINT, 12, ml);
    }
  }
  // pictures, GIFs, videos: a ring
  {
    int x0 = 602, y0 = 134, pw = 328, ph = 186;
    rrect(x0, y0, pw, ph, 18, SURF);
    text(x0 + 16, y0 + 28, TEXT, 16, "What's in it");
    float ccx = x0 + 86, ccy = y0 + 106, rr = 52;
    int tot = kinds[0] + kinds[1] + kinds[2];
    unsigned col[3] = {ACC, ACC2, mix_col(ACC, TEXT, 0.55f)};
    const char *nm[3] = {"Pictures", "GIFs", "Videos"};
    float a0 = -1.5708f;
    const int N = 120;
    for (int d = 0; d < N; d++) circle(ccx + cosf(a0 + d * 6.2832f / N) * rr, ccy + sinf(a0 + d * 6.2832f / N) * rr, 9, with_alpha(TEXT, 12));
    for (int k = 0; k < 3 && tot; k++) {
      float part = kinds[k] / (float)tot * grow;
      int n = (int)(part * N);
      for (int d = 0; d < n; d++) { float an = a0 + (d + 0.5f) * 6.2832f / N; circle(ccx + cosf(an) * rr, ccy + sinf(an) * rr, 9, col[k]); }
      a0 += part * 6.2832f;
    }
    snprintf(b, sizeof b, "%d", tot);
    text_center((int)ccx, (int)ccy + 4, TEXT, 20, b);
    text_center((int)ccx, (int)ccy + 22, FAINT, 11, "in all");
    for (int k = 0; k < 3; k++) {
      int ly = y0 + 62 + k * 34;
      circle(x0 + 172, ly - 5, 6, col[k]);
      snprintf(b, sizeof b, "%s  %d", nm[k], kinds[k]);
      text(x0 + 186, ly, TEXT, 14, b);
    }
    snprintf(b, sizeof b, "%d never opened", never);
    text(x0 + 172, y0 + 168, FAINT, 12, b);
  }
  // the most opened, and the categories
  int y = 344;
  text(30, y, DIM, 16, "Most opened");
  for (int k = 0; k < 5 && top[k] >= 0; k++) {
    const LvItem *it = &vault.items[top[k]];
    snprintf(b, sizeof b, "%lld×", lv_views_of(&vault, it));
    text(30, y + 26 + k * 24, ACC, 14, b);
    text_fit(80, y + 26 + k * 24, TEXT, 14, it->name[0] ? it->name : it->file, 380);
  }
  text(490, y, DIM, 16, "Categories");
  int cmax = 1;
  for (int k = 0; k < vault.ncats && k < 5; k++) if (vault.cat_counts[k] > cmax) cmax = vault.cat_counts[k];
  for (int k = 0; k < vault.ncats && k < 5; k++) {
    float f = vault.cat_counts[k] / (float)cmax * grow;
    rrect_round(490, y + 12 + k * 24, 440, 18, 9, with_alpha(TEXT, 10));
    rrect_round(490, y + 12 + k * 24, fmaxf(18, 440 * f), 18, 9, with_alpha(ACC, 150));
    snprintf(b, sizeof b, "%s  %d", vault.cats[k], vault.cat_counts[k]);
    text(500, y + 26 + k * 24, TEXT, 13, b);
  }
  if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_BACK); screen = stats_back; }
  draw_bar();
}
