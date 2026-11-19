// The rear touchpad: what each half of it does (Settings › Rear touchpad: a window with the Vita's
// back drawn, each half chosen on its own), and a sign at the top left whenever it's touched (shown at
// least two seconds, fading in and out). Compiled inside main.c, after the full views (their zoom).
enum { REAR_DEFAULT, REAR_BOTH, REAR_SCROLL, REAR_SWIPE, REAR_VOLUME, REAR_ZOOM, REAR_OFF, REAR_N };
static const char *const REAR_NAME[REAR_N] = {"", "Scroll and swipe", "Scroll only", "Swipe only", "Video volume", "Zoom", "Off"};
static const char *const REAR_ABOUT[REAR_N] = {"",
  "Up and down scroll lists; across goes to the next or the one before (as it was)",
  "Up and down scroll lists and pages; across does nothing",
  "Across goes to the next or the one before; up and down do nothing",
  "Up and down make the video louder or quieter",
  "Up and down zoom the picture in the full view",
  "That half does nothing at all (no accidental touches)"};
static const char *const REAR_ICON[REAR_N] = {"", "touchpad", "scroll", "swipe", "film", "zoom", "xmark"};
static int rear_of(int right) { int v = right ? cfg.rear_r : cfg.rear_l; return v > 0 && v < REAR_N ? v : REAR_BOTH; }
static unsigned rear_seen_at, rear_first_at; // (the last frame a finger was on it; when this touch began)
static int rear_half; // (the half the finger is on: 0 left, 1 right)
static float rear_vol_acc;
// After the input is read: the touchpad's movement kept or turned into what its half does
static void rear_apply(void) {
  if (!in.back_on) { rear_first_at = 0; return; }
  // (calmer: the touchpad's raw movement is small and jittery. Nothing until the finger has gone
  //  14 px one way; then locked to that way (up-down or across) for the touch; smoothed; a step
  //  back of under 3 px against the way it's going ignored; and twice the movement for scrolls)
  static float accx, accy, sdx, sdy;
  static int lock; // 0 not yet, 1 up and down, 2 across
  if (!rear_first_at) { rear_first_at = frame_no; rear_half = in.back_x0 >= W / 2; rear_vol_acc = 0; accx = accy = sdx = sdy = 0; lock = 0; }
  int what = rear_of(rear_half);
  if (what != REAR_OFF) rear_seen_at = frame_no;
  if (!lock) {
    accx += in.bdx; accy += in.bdy;
    if (fabsf(accy) > 14 && fabsf(accy) > 1.4f * fabsf(accx)) lock = 1;
    else if (fabsf(accx) > 14 && fabsf(accx) > 1.4f * fabsf(accy)) lock = 2;
    in.bdx = in.bdy = 0;
    if (!lock) return;
  }
  float rx = lock == 2 ? in.bdx : 0, ry = lock == 1 ? in.bdy : 0;
  if (sdy * ry < 0 && fabsf(ry) < 3) ry = 0;
  if (sdx * rx < 0 && fabsf(rx) < 3) rx = 0;
  sdy = sdy * 0.55f + ry * 0.45f;
  sdx = sdx * 0.55f + rx * 0.45f;
  in.bdy = sdy * 2.0f;
  in.bdx = sdx;
  switch (what) {
    case REAR_SCROLL: in.bdx = 0; break;
    case REAR_SWIPE: in.bdy = 0; break;
    case REAR_VOLUME: {
      rear_vol_acc -= in.bdy;
      int steps = (int)(rear_vol_acc / 14);
      if (steps) {
        rear_vol_acc -= steps * 14;
        cfg.volume += steps * 5;
        if (cfg.volume < 0) cfg.volume = 0;
        if (cfg.volume > 150) cfg.volume = 150;
        if (vid) lv_video_set_volume(vid, cfg.volume * 32768 / 100);
        if (wv_vid) lv_video_set_volume(wv_vid, cfg.volume * 32768 / 100);
        char m[40];
        snprintf(m, sizeof m, "Volume %d%%", cfg.volume);
        say(m);
        cfg_save();
      }
      in.bdx = in.bdy = 0;
      break;
    }
    case REAR_ZOOM: {
      float f = 1 - in.bdy * 0.01f;
      if (screen == S_VIEWER) zoom = fminf(6, fmaxf(1, zoom * f));
      else if (screen == S_WEBVIEW) wzoom = fminf(6, fmaxf(1, wzoom * f));
      in.bdx = in.bdy = 0;
      break;
    }
    case REAR_OFF: in.bdx = in.bdy = 0; in.back_on = 0; break;
  }
}
// The sign: glass at the top left, the touchpad's icon and what that half is doing; two seconds at
// least, fading in and out
static void rear_sign_draw(void) {
  if (!rear_seen_at || screen == S_CALC || screen == S_COVER || screen == S_PIN) return;
  unsigned since_start = frame_no - (rear_first_at ? rear_first_at : rear_seen_at), since_last = frame_no - rear_seen_at;
  int hold = 120; // (two seconds)
  float in_a = fminf(1, (since_start + 1) / 10.0f);
  float out_a = since_last < (unsigned)hold ? 1 : fmaxf(0, 1 - (since_last - hold) / 20.0f);
  if (rear_first_at == 0 && since_last >= (unsigned)hold + 20) { rear_seen_at = 0; return; }
  int a = (int)(220 * fminf(in_a, out_a));
  if (a <= 0) return;
  const char *what = REAR_NAME[rear_of(rear_half)];
  char l[80];
  snprintf(l, sizeof l, "Rear touchpad: %s", what);
  int w = text_w(14, l) + 58, x = 10, y = 64;
  rrect_round(x, y, w, 36, 18, RGBA(0, 0, 0, a * 120 / 255));
  rrect_round(x + 1, y + 1, w - 2, 34, 17, with_alpha(mix_col(SURF2, ACC, 0.2f), a * 170 / 255));
  rrect_round(x + 3, y + 2, w - 6, 14, 7, with_alpha(TEXT, a / 14));
  icon("touchpad", x + 22, y + 18, with_alpha(ACC, a));
  text(x + 40, y + 23, with_alpha(TEXT, a), 14, l);
}

// ----- the window: the Vita's back, each half chosen -----
static int rw_half, rw_sel; // (rw_open: main.c)
static void rear_window_open(void) { rw_open = 1; rw_half = 0; rw_sel = rear_of(0) - 1; sfx(SFX_OPEN); }
static void draw_rear_window(void) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 170));
  const int px = 24, py = 20, pw = W - 48, ph = H - 40;
  rrect(px, py, pw, ph, 24, SURF);
  icon("touchpad", px + 34, py + 32, ACC);
  text(px + 56, py + 40, TEXT, 22, "Rear touchpad");
  text(px + pw - 24 - text_w(13, "←→ half  ·  ↑↓ choose  ·  ○ done"), py + 38, FAINT, 13, "←→ half  ·  ↑↓ choose  ·  ○ done");
  // the Vita's back, as it is: the body, the camera, the touchpad with its two finger grips
  const float bx = px + 30, by = py + 70, bw = 420, bh = 236;
  rrect_round(bx - 2, by + 4, bw + 4, bh, bh / 2 - 6, RGBA(0, 0, 0, 90));
  rrect_round(bx, by, bw, bh, bh / 2 - 8, mix_col(SURF2, RGBA(0, 0, 0, 255), 0.3f));
  rrect_round(bx + 4, by + 3, bw - 8, bh * 0.4f, bh * 0.2f, with_alpha(TEXT, 10));
  circle(bx + bw / 2, by + 22, 7, RGBA(10, 10, 12, 255)); // (the camera)
  circle(bx + bw / 2, by + 22, 3, with_alpha(ACC, 120));
  text_center(bx + bw / 2, by + bh - 16, FAINT, 11, "PS Vita");
  const float tx = bx + 78, ty = by + 46, tw = bw - 156, th = bh - 90;
  for (int h = 0; h < 2; h++) {
    float hx = tx + h * tw / 2, on = h == rw_half;
    rrect(hx + (h ? 1 : 0), ty, tw / 2 - 1, th, 16, on ? with_alpha(ACC, 70) : with_alpha(BG, 255));
    if (on) ring9(hx + (h ? 1 : 0) - 3, ty - 3, tw / 2 + 5, th + 6, ACC);
    icon(REAR_ICON[rear_of(h)], hx + tw / 4, ty + th / 2 - 12, on ? ACC : DIM);
    text_center(hx + tw / 4, ty + th / 2 + 18, on ? TEXT : DIM, 12, REAR_NAME[rear_of(h)]);
    if (hit(hx, ty, tw / 2, th)) { rw_half = h; rw_sel = rear_of(h) - 1; sfx(SFX_TICK); }
  }
  // the finger grips either side, and a dot where a finger is on the real one now
  rrect_round(bx + 22, by + bh / 2 - 34, 30, 68, 15, with_alpha(BG, 200));
  rrect_round(bx + bw - 52, by + bh / 2 - 34, 30, 68, 15, with_alpha(BG, 200));
  text_center(bx + bw / 2, ty - 8, FAINT, 11, "your left hand's side  ·  your right hand's side");
  if (in.back_on) {
    float fxp = in.back_x0 >= 0 ? tx + (in.back_x0 / (float)W) * tw : tx;
    circle(fxp, ty + th / 2, 9, with_alpha(ACC, 200));
  }
  // the presets for the chosen half
  const float lx = bx + bw + 30, lw = px + pw - 24 - lx;
  text(lx, py + 86, DIM, 14, rw_half ? "The right half" : "The left half");
  for (int k = 0; k < REAR_N - 1; k++) {
    int v = k + 1, on = k == rw_sel, cur = rear_of(rw_half) == v;
    float ry = py + 100 + k * 46;
    rrect(lx, ry, lw, 40, 14, on ? SURF2 : with_alpha(SURF2, 110));
    if (cur) rrect(lx, ry, 5, 40, 2, ACC);
    icon(REAR_ICON[v], lx + 24, ry + 20, cur ? ACC : DIM);
    text_fit(lx + 44, ry + 26, cur ? TEXT : DIM, 15, REAR_NAME[v], lw - 80);
    if (cur) text(lx + lw - 30, ry + 27, ACC, 18, "\u2713");
    if (on) ring9(lx - 3, ry - 3, lw + 6, 46, ACC);
    if (hit(lx, ry, lw, 40)) { rw_sel = k; in.pressed |= SCE_CTRL_CROSS; }
  }
  text_fit(bx, by + bh + 34, FAINT, 13, REAR_ABOUT[rw_sel + 1], bw + 20);
  if (in.pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) { rw_half ^= 1; rw_sel = rear_of(rw_half) - 1; sfx(SFX_TICK); }
  if (in.pressed & SCE_CTRL_DOWN) { rw_sel = (rw_sel + 1) % (REAR_N - 1); sfx(SFX_TICK); }
  if (in.pressed & SCE_CTRL_UP) { rw_sel = (rw_sel + REAR_N - 2) % (REAR_N - 1); sfx(SFX_TICK); }
  if (in.pressed & SCE_CTRL_CROSS) {
    if (rw_half) cfg.rear_r = rw_sel + 1; else cfg.rear_l = rw_sel + 1;
    cfg_save();
    sfx(SFX_TOGGLE);
  }
  if ((in.pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE)) || (in.tap && (in.tap_x < px || in.tap_x > px + pw || in.tap_y < py || in.tap_y > py + ph))) { rw_open = 0; sfx(SFX_CLOSE); }
  in.pressed = 0; in.tap = 0; in.touching = 0;
}

// ---------- the same sign for the app's events ----------
// A save starting, done or failed; a slideshow starting or ending; Google Drive syncing and done;
// Favourites from Drive. Under the rear touchpad's sign when both show; the newest replaces the one
// before. Two and a half seconds at least, fading in and out.
static char ev_text[96];
static const char *ev_icon;
static unsigned ev_at;
static unsigned ev_col;
static void ev_notice(const char *ic, unsigned col, const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vsnprintf(ev_text, sizeof ev_text, fmt, a);
  va_end(a);
  ev_icon = ic; ev_col = col; ev_at = frame_no ? frame_no : 1;
}
// (each frame: what changed, said)
static void ev_watch(void) {
  static int was_slide = -1, was_drive = -1;
  if (was_slide >= 0 && slideshow != was_slide) ev_notice("play", ACC, slideshow ? "Slideshow started" : "Slideshow ended");
  was_slide = slideshow;
  int ds = drive.state;
  if (was_drive >= 0 && ds != was_drive && !drive.signing) {
    if (ds == DRV_SYNCING) ev_notice("refresh", ACC, "Google Drive: syncing\u2026");
    else if (ds == DRV_DONE) ev_notice("refresh", ACC, "Google Drive: in step");
    else if (ds == DRV_FAILED) ev_notice("xmark", ACC2, "Google Drive: the sync didn't finish");
  }
  was_drive = ds;
}
static void ev_sign_draw(void) {
  if (!ev_at || screen == S_CALC || screen == S_COVER || screen == S_PIN) return;
  unsigned since = frame_no - ev_at;
  const unsigned hold = 150; // (two and a half seconds)
  if (since > hold + 20) { ev_at = 0; return; }
  float in_a = fminf(1, (since + 1) / 10.0f), out_a = since < hold ? 1 : fmaxf(0, 1 - (since - hold) / 20.0f);
  float slide = (1 - fminf(1, since / 12.0f));
  int a = (int)(225 * fminf(in_a, out_a));
  if (a <= 0) return;
  int rear_up = rear_seen_at && frame_no - rear_seen_at < 140;
  int w = text_w(14, ev_text) + 58, x = 10 - (int)(slide * slide * 30), y = rear_up ? 106 : 64;
  rrect_round(x, y, w, 36, 18, RGBA(0, 0, 0, a * 120 / 255));
  rrect_round(x + 1, y + 1, w - 2, 34, 17, with_alpha(mix_col(SURF2, ev_col, 0.22f), a * 175 / 255));
  rrect_round(x + 3, y + 2, w - 6, 14, 7, with_alpha(TEXT, a / 14));
  icon(ev_icon ? ev_icon : "info", x + 22, y + 18, with_alpha(ev_col, a));
  text(x + 40, y + 23, with_alpha(TEXT, a), 14, ev_text);
}
