// The phone's "Hide now" disguise and its touch lock, on the Vita. Compiled inside main.c.
//
// SELECT hides the app (Settings → SELECT shows): the plain clock as before, or a calculator that
// really calculates. With a PIN lock, typing the PIN and pressing = gets back in; without one, holding
// the display for a second does (something nobody tries by accident), as on the phone.
//
// Touch lock: hold L and R together for a second and every touch and button does nothing (for the
// time chosen in Settings, or until L and R are held again): a picture, video or slideshow carries on
// while the Vita is propped up or handed over. SELECT still hides the app.

// ---------- the calculator ----------
static char calc_disp[24] = "0", calc_typed[16];
static double calc_acc;
static char calc_op;
static int calc_fresh = 1, calc_sel = 12;
static unsigned calc_hold;
static void calc_show(double v) {
  if (fabs(v) >= 1e12 || (v != 0 && fabs(v) < 1e-6)) snprintf(calc_disp, sizeof calc_disp, "%.6g", v);
  else { snprintf(calc_disp, sizeof calc_disp, "%.10f", v); char *e = calc_disp + strlen(calc_disp) - 1; while (*e == '0') *e-- = 0; if (*e == '.') *e = 0; }
  if (!strcmp(calc_disp, "-0")) strcpy(calc_disp, "0");
}
static double calc_apply(double a, char op, double b) {
  switch (op) { case '+': return a + b; case '-': return a - b; case '*': return a * b; case '/': return b != 0 ? a / b : NAN; }
  return b;
}
static void calc_reset(void) { strcpy(calc_disp, "0"); calc_typed[0] = 0; calc_acc = 0; calc_op = 0; calc_fresh = 1; }
static int calc_return;
static void calc_open(int from) { calc_return = from; calc_reset(); screen = S_CALC; }
static void calc_leave(void) {
  sfx(SFX_START);
  calc_reset();
  screen = calc_return;
}
static void calc_key(const char *k) {
  char c = k[0];
  if (c >= '0' && c <= '9') {
    if (calc_fresh || !strcmp(calc_disp, "0")) calc_disp[0] = 0, calc_fresh = 0;
    size_t n = strlen(calc_disp);
    if (n < 14) { calc_disp[n] = c; calc_disp[n + 1] = 0; }
    n = strlen(calc_typed);
    if (n < sizeof calc_typed - 1) { calc_typed[n] = c; calc_typed[n + 1] = 0; }
  } else if (c == '.') {
    if (calc_fresh) { strcpy(calc_disp, "0"); calc_fresh = 0; }
    if (!strchr(calc_disp, '.') && strlen(calc_disp) < 14) strcat(calc_disp, ".");
  } else if (c == 'C') calc_reset();
  else if (!strcmp(k, "±")) { double v = -atof(calc_disp); calc_show(v); }
  else if (c == '%') calc_show(atof(calc_disp) / 100);
  else if (c == '=') {
    // the PIN, then =: back in (the digits typed since the last operator or clear)
    if (pin.on && calc_typed[0]) {
      char h[65];
      pin_hash(calc_typed, pin.salt, h);
      if (!strcmp(h, pin.hash)) { calc_leave(); return; }
    }
    if (calc_op) { double r = calc_apply(calc_acc, calc_op, atof(calc_disp)); if (isnan(r)) strcpy(calc_disp, "Error"); else calc_show(r); calc_op = 0; }
    calc_fresh = 1;
    calc_typed[0] = 0;
  } else {
    // + - × ÷
    char op = !strcmp(k, "×") ? '*' : !strcmp(k, "÷") ? '/' : c;
    if (calc_op && !calc_fresh) { double r = calc_apply(calc_acc, calc_op, atof(calc_disp)); if (isnan(r)) { strcpy(calc_disp, "Error"); r = 0; } else calc_show(r); calc_acc = r; }
    else calc_acc = atof(calc_disp);
    calc_op = op;
    calc_fresh = 1;
    calc_typed[0] = 0;
  }
}
static void draw_calc(void) {
  // (a plain system-looking calculator: black, grey keys, orange operators)
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 255));
  static const char *keys[20] = {"C", "±", "%", "÷", "7", "8", "9", "×", "4", "5", "6", "-", "1", "2", "3", "+", "0", ".", "", "="};
  int kw = 112, kh = 74, gx = W - 4 * (kw + 8) - 30, gy = 108;
  // the display, on the left
  text(40, 60, RGBA(140, 140, 140, 255), 18, "Calculator");
  int dw = text_w(64, calc_disp);
  text(gx - 40 - (dw > gx - 80 ? gx - 80 : dw), 300, RGBA(255, 255, 255, 255), 64, calc_disp);
  if (calc_op) { char o[4] = {calc_op == '*' ? 'x' : calc_op, 0}; text(gx - 70, 200, RGBA(255, 160, 10, 255), 28, o); }
  // holding the display (touch, or △ held) for a second: back in, when there's no PIN
  int holding = (in.touching && in.tx < gx - 20 && in.ty > 200 && in.ty < 360) || (in.now & SCE_CTRL_TRIANGLE);
  if (holding) { if (!calc_hold) calc_hold = frame_no; else if (!pin.on && frame_no - calc_hold > 60) { calc_hold = 0; calc_leave(); return; } }
  else calc_hold = 0;
  for (int k = 0; k < 20; k++) {
    if (!keys[k][0]) continue;
    int col = k % 4, row = k / 4;
    int x = gx + col * (kw + 8), y = gy + row * (kh + 8), w = k == 16 ? kw * 2 + 8 : kw;
    int op = col == 3, top = row == 0 && col < 3;
    unsigned bg = op ? RGBA(255, 159, 10, 255) : top ? RGBA(165, 165, 165, 255) : RGBA(51, 51, 51, 255);
    if (k == calc_sel) bg = op ? RGBA(255, 200, 120, 255) : RGBA(110, 110, 110, 255);
    rrect(x, y, w, kh, kh / 2, bg);
    text_center(x + w / 2, y + kh / 2 + 12, top ? RGBA(0, 0, 0, 255) : RGBA(255, 255, 255, 255), 32, keys[k]);
    if (hit(x, y, w, kh)) { calc_sel = k; calc_key(keys[k]); }
  }
  // the D-pad goes round the keys, ✕ presses
  int col = calc_sel % 4, row = calc_sel / 4;
  if (in.pressed & SCE_CTRL_RIGHT) col = (col + 1) % 4;
  if (in.pressed & SCE_CTRL_LEFT) col = (col + 3) % 4;
  if (in.pressed & SCE_CTRL_DOWN) row = (row + 1) % 5;
  if (in.pressed & SCE_CTRL_UP) row = (row + 4) % 5;
  calc_sel = row * 4 + col;
  if (calc_sel == 18) calc_sel = (in.pressed & SCE_CTRL_LEFT) ? 16 : (in.pressed & SCE_CTRL_RIGHT) ? 19 : 16;
  if (in.pressed & SCE_CTRL_CROSS) calc_key(keys[calc_sel]);
  if (in.pressed & SCE_CTRL_CIRCLE) calc_key("C");
}

// ---------- touch lock ----------
static const int TL_SECS[] = {30, 60, 120, 0};
static const char *TL_NAMES[] = {"30 s", "1 min", "2 min", "Until I unlock"};
static int tl_on;
static unsigned tl_until, tl_hold, tl_poked;
// Called once a frame, after the input is read: 1 when the frame's input is to be dropped.
static int touch_lock_filter(void) {
  int lr = (in.now & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) == (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER);
  if (lr) {
    if (!tl_hold) tl_hold = frame_no;
    else if (frame_no - tl_hold == 60) {
      tl_on = !tl_on;
      if (tl_on) { int s = TL_SECS[cfg.tlock < 0 || cfg.tlock > 3 ? 0 : cfg.tlock]; tl_until = s ? frame_no + s * 60 : 0; sfx(SFX_TOGGLE); say("Touch lock: hold L and R to unlock"); }
      else { sfx(SFX_START); say("Unlocked"); }
    }
  } else tl_hold = 0;
  if (tl_on && tl_until && frame_no >= tl_until) { tl_on = 0; sfx(SFX_START); }
  if (!tl_on) return lr; // (L and R held together: neither goes anywhere meanwhile)
  if (in.pressed || in.tap) tl_poked = frame_no;
  return 1;
}
static void draw_touch_lock(void) {
  if (!tl_on) return;
  char b[48];
  if (tl_until) { unsigned s = (tl_until - frame_no) / 60 + 1; snprintf(b, sizeof b, "Locked  %u:%02u", s / 60, s % 60); }
  else snprintf(b, sizeof b, "Locked");
  int w = text_w(15, b) + 52, x = W / 2 - w / 2, poked = frame_no - tl_poked < 40;
  rrect(x, 8, w, 30, 15, RGBA(0, 0, 0, poked ? 220 : 150));
  rrect(x + 14, 19, 12, 10, 2, ACC);
  circle(x + 20, 18, 4, ACC);
  circle(x + 20, 18, 2, RGBA(0, 0, 0, 200));
  text(x + 34, 29, TEXT, 15, b);
  if (poked) text_center(W / 2, 56, with_alpha(TEXT, 200), 14, "Touch is locked · hold L and R to unlock");
  if (tl_hold) { float f = (frame_no - tl_hold) / 60.0f; rrect(x, 40, w * (f > 1 ? 1 : f), 3, 1, ACC); }
}
