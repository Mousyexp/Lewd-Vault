// The comic reader: one engine for the Library's books and the Comics tab's chapters (compiled inside
// main.c, after show.c). Each gives it its pages (ComicSrc); it draws them and takes the buttons and
// touch, as the reading options say (cfg.c*):
//   Reading      pages, one at a time / a continuous strip, scrolled
//   Orientation  landscape / portrait (the Vita held on its side: its right edge up)
//   Page turn    slide / fade / zoom / none
//   Direction    left to right / right to left (manga)
//   Taps         the sides turn pages / anywhere: the next / only the controls
//   Double tap   zoom / nothing
//   Rear pad     a swipe on the back turns pages / nothing
// Everything is worked out in "reading" coordinates (what the reader sees as across and down) and
// turned onto the screen only when drawn, so portrait is the same code.

typedef struct {
  int n, cur;                      // the pages, and the one shown
  vita2d_texture *(*tex)(int k);   // its picture, or NULL (not here yet)
  float (*aspect)(int k);          // its height over its width, for one not here yet
  void (*go)(int k);               // k becomes the page shown (its picture asked for)
  int (*now)(void);                // the page shown now (a source may not have moved: pages still coming)
  void (*close)(void);
  const char *title;
  int strip;                       // (a webtoon's: read as a continuous strip, whatever the setting)
} ComicSrc;

static const char *const RD_MODE[] = {"Pages", "Continuous strip"};
static const char *const RD_ORIENT[] = {"Landscape", "Portrait (on its side)"};
static const char *const RD_ANIM[] = {"Slide", "Fade", "Zoom", "None"};
static const char *const RD_DIR[] = {"Left to right", "Right to left (manga)"};
static const char *const RD_TAP[] = {"Sides turn pages", "Anywhere: the next", "Only the controls"};
static const char *const RD_DBL[] = {"Zoom", "Nothing", "Fit width"};
static const char *const RD_BACK[] = {"Swipe turns pages", "Nothing"};
enum { RO_MODE, RO_ORIENT, RO_ANIM, RO_DIR, RO_TAP, RO_DBL, RO_BACK, RO_N };
static const char *const RO_ICON[RO_N] = {"pages", "rotate", "swipe", "direction", "tap", "zoom", "touchpad"};
static const char *const RO_NAME[RO_N] = {"Reading", "Orientation", "Page turn", "Direction", "Taps", "Double tap", "Rear touchpad"};
static int *ro_val(int k) {
  switch (k) {
    case RO_MODE: return &cfg.cmode; case RO_ORIENT: return &cfg.corient; case RO_ANIM: return &cfg.canim;
    case RO_DIR: return &cfg.cdir; case RO_TAP: return &cfg.ctap; case RO_DBL: return &cfg.cdbl; default: return &cfg.cback;
  }
}
static int ro_count(int k) { static const int N[RO_N] = {2, 2, 4, 2, 3, 3, 2}; return N[k]; }
static const char *ro_text(int k) {
  int v = *ro_val(k);
  switch (k) {
    case RO_MODE: return RD_MODE[v]; case RO_ORIENT: return RD_ORIENT[v]; case RO_ANIM: return RD_ANIM[v];
    case RO_DIR: return RD_DIR[v]; case RO_TAP: return RD_TAP[v]; case RO_DBL: return RD_DBL[v]; default: return RD_BACK[v];
  }
}

// ----- the reading options: a panel, from the reader (△ or its button) or from Settings -----
static int ro_open, ro_sel;
static void draw_reading_options(void) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 160));
  const int w = 560, rh = 56, x = (W - w) / 2, y = 26, h = RO_N * rh + 70;
  rrect(x, y, w, h, 22, SURF);
  icon("book", x + 30, y + 30, ACC);
  text(x + 50, y + 37, TEXT, 20, "Reading comics");
  text(x + w - 24 - text_w(13, "←→ change  ·  ○ done"), y + 36, FAINT, 13, "←→ change  ·  ○ done");
  int d = 0;
  for (int k = 0; k < RO_N; k++) {
    int ry = y + 58 + k * rh, on = k == ro_sel;
    if (on) rrect(x + 10, ry, w - 20, rh - 6, 14, SURF2);
    int cy = ry + (rh - 6) / 2;
    rrect(x + 22, cy - 16, 32, 32, 10, with_alpha(ACC, on ? 255 : 45));
    icon(RO_ICON[k], x + 38, cy, on ? RGBA(20, 12, 16, 255) : ACC);
    text(x + 68, cy + 6, TEXT, 17, RO_NAME[k]);
    const char *v = ro_text(k);
    int vw = text_w(16, v);
    text(x + w - 54 - vw, cy + 6, ACC, 16, v);
    text(x + w - 40, cy + 7, on ? ACC : FAINT, 20, "›");
    if (on) focus_ring(x + 10, ry, w - 20, rh - 6);
    if (hit(x, ry, w, rh)) { ro_sel = k; d = in.tap_x < x + w / 2 ? -1 : 1; }
  }
  if (in.pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) ro_sel = (ro_sel + 1) % RO_N;
  if (in.pressed & SCE_CTRL_UP) ro_sel = (ro_sel + RO_N - 1) % RO_N;
  if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) d = 1;
  if (in.pressed & SCE_CTRL_LEFT) d = -1;
  if (d) { int *v = ro_val(ro_sel), n = ro_count(ro_sel); *v = (*v + d + n) % n; sfx(SFX_TOGGLE); cfg_save(); }
  if ((in.pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE)) || (in.tap && (in.tap_x < x || in.tap_x > x + w || in.tap_y > y + h))) { sfx(SFX_CLOSE); ro_open = 0; }
  in.pressed = 0; in.tap = 0; in.touching = 0; in.dbl = 0; in.flicked = 0;
}

// ----- the engine -----
static struct {
  const void *who;               // (the source it was showing: its state starts afresh for another)
  float zoom, pan_x, pan_y;      // (pages: zoomed in, and where)
  float swipe;                   // (pages: a page being dragged along)
  float off, vel;                // (strip: where the page shown starts down the screen, and its fling)
  int anim_dir, anim_from;       // (pages: the turn being animated, and the page before)
  unsigned anim_at, chrome_until;
  float back_sw;                 // (the rear touchpad's swipe so far)
  float fitw;                    // (pages: the zoom at which the page shown fills the width)
} rd;
#define RD_GAP (s->strip ? 0 : 8) // (between pages in a strip; a webtoon's bands meet)
static int rd_portrait(void) { return cfg.corient == 1; }
static float rd_rw(void) { return rd_portrait() ? H : W; } // (the reading space: across)
static float rd_rh(void) { return rd_portrait() ? W : H; } // (and down)
// a picture into a rectangle of the reading space, turned onto the screen
static void rd_draw(vita2d_texture *t, float rx, float ry, float rw, float rh, int alpha) {
  if (!t || rw < 1 || rh < 1) return;
  float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
  if (tw < 1 || th < 1) return;
  unsigned tint = RGBA(255, 255, 255, alpha);
  if (!rd_portrait()) { vita2d_draw_texture_tint_scale(t, rx, ry, rw / tw, rh / th, tint); return; }
  // (portrait: across the reading space is down the screen, down it is leftwards)
  float cx = W - (ry + rh / 2), cy = rx + rw / 2;
  vita2d_draw_texture_tint_scale_rotate(t, cx, cy, rw / tw, rh / th, 1.5707963f, tint);
}
// a page's height in the strip (its width the reading space's)
static float rd_len(ComicSrc *s, int k) {
  vita2d_texture *t = s->tex(k);
  float a = t ? vita2d_texture_get_height(t) / fmaxf(1, vita2d_texture_get_width(t)) : s->aspect(k);
  if (a < 0.2f || a > 30) a = 1.45f;
  return rd_rw() * a;
}
static void rd_go(ComicSrc *s, int k) { s->go(k); s->cur = s->now(); }
static void rd_turn(ComicSrc *s, int dir) { // (dir: +1 the next page, -1 the one before)
  int k = s->cur + dir, was = s->cur;
  rd_go(s, k); // (past the ends, the source may go on to another chapter)
  if (s->cur == was) return;
  rd.anim_from = was; rd.anim_dir = dir; rd.anim_at = frame_no;
  rd.zoom = 1; rd.pan_x = rd.pan_y = 0; rd.swipe = 0;
  sfx(SFX_TICK);
}
// What the reader's own input does, in reading terms. Returns 1 when the reader was closed.
static int comic_reader(ComicSrc *s) {
  if (rd.who != s->title) { memset(&rd, 0, sizeof rd); rd.who = s->title; rd.zoom = 1; rd.chrome_until = frame_no + 180; }
  if (ro_open) { draw_reading_options(); return 0; }
  const float RW = rd_rw(), RH = rd_rh();
  int por = rd_portrait(), rtl = cfg.cdir == 1;
  // the input, turned into reading space
  float rdx = por ? in.drag_dy : in.drag_dx, rdy = por ? -in.drag_dx : in.drag_dy;
  float rfx = por ? in.fvy : in.fvx, rfy = por ? -in.fvx : in.fvy;
  float rtx = por ? in.tap_y : in.tap_x, rty = por ? W - in.tap_x : in.tap_y;
  float rmx = por ? in.ty - in.down_y : in.tx - in.down_x, rmy = por ? in.down_x - in.tx : in.ty - in.down_y;
  // the buttons, as the reader holds the Vita: in portrait the D-pad's Down is their right
  unsigned b_next = por ? SCE_CTRL_DOWN : SCE_CTRL_RIGHT, b_prev = por ? SCE_CTRL_UP : SCE_CTRL_LEFT;
  unsigned b_down = por ? SCE_CTRL_LEFT : SCE_CTRL_DOWN, b_up = por ? SCE_CTRL_RIGHT : SCE_CTRL_UP;
  if (rtl) { unsigned x = b_next; b_next = b_prev; b_prev = x; }
  float stick = por ? -in.lx : in.ly;
  int top_band = in.tap && in.tap_y < 56 && frame_no < rd.chrome_until; // (the controls' own taps)

  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 255));
  if (cfg.cmode == 1 || s->strip) {
    // ===== a continuous strip =====
    if (in.touching) { rd.off += rdy; rd.vel = 0; }
    else if (in.flicked && fabsf(rfy) > fabsf(rfx)) rd.vel = rfy * 1.5f;
    rd.off += (rd.vel - stick * 18) * ft;
    rd.vel *= decay(0.94f);
    if (fabsf(rd.vel) < 0.2f) rd.vel = 0;
    if (in.pressed & b_down) rd.vel = -RH * 0.06f;
    if (in.pressed & b_up) rd.vel = RH * 0.06f;
    // the page shown follows: the next once this one's mostly gone up, the one before once it's low
    for (int g = 0; g < 4; g++) {
      float len = rd_len(s, s->cur);
      int was = s->cur;
      if (rd.off + len < RH * 0.35f && s->cur < s->n - 1) { rd_go(s, s->cur + 1); if (s->cur != was) rd.off += len + RD_GAP; else break; }
      else if (rd.off > RH * 0.65f && s->cur > 0) { rd_go(s, s->cur - 1); if (s->cur != was) rd.off -= rd_len(s, s->cur) + RD_GAP; else break; }
      else break;
    }
    // (not past the first page's top, nor the last's foot)
    float lenc = rd_len(s, s->cur);
    if (s->cur == 0 && rd.off > 0) { rd.off *= in.touching ? 1 : 0.7f; if (!in.touching) rd.vel = 0; }
    if (s->cur == s->n - 1) { float lo = lenc > RH ? RH - lenc : 0; if (rd.off < lo) { rd.off = in.touching ? rd.off : lo + (rd.off - lo) * 0.7f; if (!in.touching) rd.vel = 0; } }
    // the pages: this one, those after it down the screen, those before it up
    float y = rd.off;
    for (int k = s->cur; k < s->n && y < RH; k++) {
      float len = rd_len(s, k);
      vita2d_texture *t = s->tex(k);
      if (t) rd_draw(t, 0, y, RW, len, 255);
      else if (!por) { rrect(20, y, RW - 40, len, 12, RGBA(30, 26, 32, 255)); spinner(RW / 2, y + fminf(len, RH) / 2, 16); }
      y += len + RD_GAP;
    }
    y = rd.off;
    for (int k = s->cur - 1; k >= 0 && y > 0; k--) {
      float len = rd_len(s, k);
      y -= len + RD_GAP;
      vita2d_texture *t = s->tex(k);
      if (t) rd_draw(t, 0, y, RW, len, 255);
    }
    if (in.tap && !top_band) rd.chrome_until = frame_no < rd.chrome_until ? frame_no : frame_no + 240;
    if (in.pressed & (SCE_CTRL_RTRIGGER)) { rd.off = 0; rd_go(s, s->cur + 1); }
    if (in.pressed & (SCE_CTRL_LTRIGGER)) { rd.off = 0; rd_go(s, s->cur - 1); }
  } else {
    // ===== pages, one at a time =====
    vita2d_texture *t = s->tex(s->cur);
    float e = 1;
    if (rd.anim_at && cfg.canim != 3) { float p = fminf(1, (frame_no - rd.anim_at) / 14.0f); e = 1 - (1 - p) * (1 - p) * (1 - p); if (p >= 1) rd.anim_at = 0; }
    else rd.anim_at = 0;
    // the turn's direction on screen: the next comes from the right (from the left right to left)
    float side = (rd.anim_dir > 0 ? 1 : -1) * (rtl ? -1 : 1);
    // the page before, going out
    if (rd.anim_at && rd.anim_from != s->cur) {
      vita2d_texture *o = s->tex(rd.anim_from);
      if (o) {
        float ow = vita2d_texture_get_width(o), oh = vita2d_texture_get_height(o), os = fminf(RW / ow, RH / oh);
        float dw = ow * os, dh = oh * os, ox = (RW - dw) / 2, oy = (RH - dh) / 2;
        if (cfg.canim == 0) rd_draw(o, ox - side * RW * e, oy, dw, dh, 255);
        else rd_draw(o, ox, oy, dw, dh, (int)(255 * (1 - e)));
      }
    }
    if (t) {
      float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
      int tall = th > tw * 2.2f;
      float fit = tall ? RW / tw : fminf(RW / tw, RH / th);
      rd.fitw = fmaxf(1, (RW / tw) / fit);
      float sc = fit * rd.zoom;
      float dw = tw * sc, dh = th * sc;
      float mx = fmaxf(0, (dw - RW) / 2), my = fmaxf(0, (dh - RH) / 2);
      rd.pan_x = fminf(mx, fmaxf(-mx, rd.pan_x));
      rd.pan_y = fminf(my, fmaxf(-my, rd.pan_y));
      float x = (RW - dw) / 2 + rd.pan_x + rd.swipe, y = (RH - dh) / 2 + rd.pan_y;
      int alpha = 255;
      if (rd.anim_at) {
        if (cfg.canim == 0) x += side * RW * (1 - e);
        else if (cfg.canim == 1) alpha = (int)(255 * e);
        else if (cfg.canim == 2) { float z = 0.9f + 0.1f * e; x += dw * (1 - z) / 2; y += dh * (1 - z) / 2; dw *= z; dh *= z; alpha = (int)(255 * e); }
      }
      rd_draw(t, x, y, dw, dh, alpha);
      // a tall page: the stick and ↑↓ go down it
      if (tall && rd.zoom < 1.01f) { rd.pan_y -= stick * 16; if (in.pressed & b_down) rd.pan_y -= RH * 0.7f; if (in.pressed & b_up) rd.pan_y += RH * 0.7f; }
    } else spinner(W / 2, H / 2, 24);
    // touch: drag the page along to turn it, drag when zoomed, pinch
    static float pd0, z0;
    if (in.touching && in.tcount >= 2) {
      float d = hypotf(in.t2x - in.tx, in.t2y - in.ty);
      if (pd0 == 0) { pd0 = d; z0 = rd.zoom; } else rd.zoom = fminf(4, fmaxf(1, z0 * d / pd0));
    } else if (in.touching) {
      pd0 = 0;
      if (rd.zoom > 1.01f) { rd.pan_x += rdx; rd.pan_y += rdy; }
      else if (fabsf(rmx) > fabsf(rmy)) rd.swipe += rdx;
      else rd.pan_y += rdy;
    } else {
      pd0 = 0;
      int fw = rtl ? -1 : 1;
      if (rd.swipe < -110) { rd.swipe = 0; rd_turn(s, fw); }
      else if (rd.swipe > 110) { rd.swipe = 0; rd_turn(s, -fw); }
      rd.swipe *= decay(0.6f);
      if (rd.zoom < 1.01f) rd.zoom = 1;
    }
    // taps: as chosen
    // (fit width: the page's width the screen's, from its top: a page's panels at a readable size)
    if (in.dbl && cfg.cdbl == 2) { rd.zoom = rd.zoom > 1.01f ? 1 : rd.fitw; rd.pan_x = 0; rd.pan_y = 1e5f; }
    else if (in.dbl && cfg.cdbl == 0) { rd.zoom = rd.zoom > 1.5f ? 1 : 2.5f; rd.pan_x = (RW / 2 - rtx) * (rd.zoom - 1); rd.pan_y = (RH / 2 - rty) * (rd.zoom - 1); }
    else if (in.tap && !top_band && rd.zoom < 1.01f) {
      int fw = rtl ? -1 : 1;
      if (cfg.ctap == 0 && rtx < RW / 3) rd_turn(s, -fw);
      else if (cfg.ctap == 0 && rtx > RW * 2 / 3) rd_turn(s, fw);
      else if (cfg.ctap == 1 && rtx < RW / 5) rd_turn(s, -1); // (anywhere: the next; the very edge, back)
      else if (cfg.ctap == 1) rd_turn(s, 1);
      else rd.chrome_until = frame_no < rd.chrome_until ? frame_no : frame_no + 240;
    }
    if (in.pressed & b_next) rd_turn(s, 1);
    if (in.pressed & b_prev) rd_turn(s, -1);
    if (in.pressed & SCE_CTRL_RTRIGGER) rd_turn(s, 1);
    if (in.pressed & SCE_CTRL_LTRIGGER) rd_turn(s, -1);
    if (in.pressed & SCE_CTRL_CROSS) {
      if (cfg.cdbl == 2) { rd.zoom = rd.zoom > 1.01f ? 1 : rd.fitw; rd.pan_x = 0; rd.pan_y = 1e5f; }
      else { rd.zoom = rd.zoom > 1.5f ? 1 : 2; rd.pan_x = rd.pan_y = 0; }
    }
    if (rd.zoom > 1.01f) { rd.pan_x -= (por ? -in.ly : in.lx) * 12; rd.pan_y -= stick * 12; }
  }
  // the rear touchpad: a swipe along it turns the page (or scrolls the strip)
  if (cfg.cback == 0) {
    if (in.back_on) rd.back_sw += por ? in.bdy : in.bdx; else rd.back_sw = 0;
    if (fabsf(rd.back_sw) > 150) {
      int dir = rd.back_sw < 0 ? 1 : -1;
      rd.back_sw = 0;
      if (cfg.cmode == 1 || s->strip) rd.vel = -dir * RH * 0.05f; else rd_turn(s, rtl ? -dir : dir);
    }
  }
  // the controls, along the top for a while after a tap (not turned: read in either hold)
  if (frame_no < rd.chrome_until) {
    int a = frame_no + 30 > rd.chrome_until ? (int)((rd.chrome_until - frame_no) * 8) : 240;
    if (a > 240 || a < 0) a = 240;
    grad(0, 0, W, 64, RGBA(0, 0, 0, 255), 64 * a / 300, 0, 0);
    rrect(12, 10, 44, 40, 20, RGBA(0, 0, 0, a / 2));
    icon("back", 34, 30, with_alpha(TEXT, a));
    if (hit(8, 4, 56, 52)) { sfx(SFX_BACK); s->close(); return 1; }
    char pn[64];
    snprintf(pn, sizeof pn, "%d / %d", s->cur + 1, s->n);
    text_fit(70, 36, with_alpha(TEXT, a), 16, s->title ? s->title : "", W - 360);
    text_center(W - 170, 36, with_alpha(DIM, a), 15, pn);
    rrect(W - 112, 10, 100, 40, 20, RGBA(0, 0, 0, a / 2));
    icon("sliders", W - 92, 30, with_alpha(ACC, a));
    text(W - 76, 36, with_alpha(TEXT, a), 14, "Options");
    if (hit(W - 116, 4, 108, 52)) { sfx(SFX_OPEN); ro_open = 1; in.tap = 0; }
  } else {
    // (just the page, faint, at the foot)
    char pn[32];
    snprintf(pn, sizeof pn, "%d / %d", s->cur + 1, s->n);
    int pw = text_w(13, pn) + 20;
    rrect(W / 2 - pw / 2, H - 28, pw, 22, 11, RGBA(0, 0, 0, 140));
    text_center(W / 2, H - 12, DIM, 13, pn);
  }
  if (in.pressed & SCE_CTRL_TRIANGLE) { sfx(SFX_OPEN); ro_open = 1; ro_sel = 0; }
  if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_BACK); s->close(); return 1; }
  return 0;
}

// ===== the Library's books =====
// Their pages are the Library's list (a book open); the full-size pictures come one at a time from
// the thumbnail thread (ask_big) into the screen's three rotating buffers, so three are kept here in
// the order they came, and the next pages are asked for once the one shown is here.
static struct { int item; vita2d_texture *t; } lc_cache[3];
static int lc_next, lc_asked = -1;
static unsigned lc_asked_gen, lc_asked_at;
static vita2d_texture *lc_find(int item) {
  vita2d_texture *t = NULL;
  for (int k = 0; k < 3; k++) if (lc_cache[k].item == item && lc_cache[k].t) t = lc_cache[k].t;
  return t;
}
static void lc_take(void) {
  // (a full picture come for the viewer: kept here, in the slot of the buffer it went in)
  if (big_tex && big_item >= 0) {
    int k = lc_next;
    lc_next = (lc_next + 1) % 3;
    if (lc_cache[k].t && lc_cache[k].t != big_tex) release_tex(lc_cache[k].t);
    lc_cache[k].item = big_item; lc_cache[k].t = big_tex;
    big_tex = NULL; // (the reader's now: not let go when the next comes)
    if (lc_asked == big_item) lc_asked = -1;
  }
  if (lc_asked >= 0 && (frame_no - lc_asked_at > 240 || (big_failed && lc_asked_gen == (unsigned)big_gen))) lc_asked = -1;
}
static void lc_forget(void) {
  for (int k = 0; k < 3; k++) { if (lc_cache[k].t) release_tex(lc_cache[k].t); lc_cache[k].t = NULL; lc_cache[k].item = -1; }
  lc_asked = -1;
}
static vita2d_texture *lc_tex(int k) {
  if (k < 0 || k >= nshown) return NULL;
  vita2d_texture *t = lc_find(shown[k]);
  return t ? t : thumb_of(shown[k]);
}
static float lc_aspect(int k) {
  if (k < 0 || k >= nshown) return 1.45f;
  const LvItem *it = &vault.items[shown[k]];
  return it->width > 0 && it->height > 0 ? (float)it->height / it->width : 1.45f;
}
static void lc_go(int k) {
  if (k < 0 || k >= nshown || k == vk) return;
  show_item(k); // (counted as looked at, as any page; it asks for its picture)
  lc_asked = shown[k]; lc_asked_gen = big_gen; lc_asked_at = frame_no;
}
static void lc_close(void) { lc_forget(); close_viewer(); }
static int lc_now(void) { return vk; }
static int lib_reading_comic(void) { return !slideshow && vk >= 0 && vk < nshown && is_comic(shown[vk]); }
static void draw_lib_comic(void) {
  // (back after being away: what's kept may have been drawn over in the shared buffers since)
  static unsigned last_drawn;
  if (frame_no - last_drawn > 1) lc_forget();
  last_drawn = frame_no;
  lc_take();
  // the pictures wanted: this page, then the next ones (two in a strip), one at a time
  if (lc_asked < 0) {
    for (int d = 0; d <= (cfg.cmode == 1 ? 2 : 1); d++) {
      int k = vk + d;
      if (k >= nshown) break;
      if (!lc_find(shown[k])) { ask_big(shown[k]); lc_asked = shown[k]; lc_asked_gen = big_gen; lc_asked_at = frame_no; break; }
    }
  }
  // (and the thumbnails of those around, to show meanwhile)
  int want[6], nw = 0;
  for (int d = -1; d <= 3 && nw < 6; d++) { int k = vk + d; if (k >= 0 && k < nshown && !thumb_of(shown[k]) && tstate[shown[k]] != 3) want[nw++] = shown[k]; }
  if (nw) ask_thumbs(want, nw);
  static ComicSrc src;
  src.n = nshown; src.cur = vk; src.tex = lc_tex; src.aspect = lc_aspect; src.go = lc_go; src.now = lc_now; src.close = lc_close;
  src.title = lf.coll[0] ? lf.coll : vault.items[shown[vk]].collection;
  comic_reader(&src);
}
