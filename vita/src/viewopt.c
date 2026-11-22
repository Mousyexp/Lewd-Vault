// How a picture or a video sits in the full views (the Library's and the Browser's): its size (fit
// to the screen, fill it, its own pixels, the screen's width; a video can also be stretched) and the
// way round the Vita is held (portrait: the picture turned a quarter, as the comic reader turns
// pages). Settings (Viewing) and the full views' own panel (↑ on a picture, or the size button at
// the top) change them. Part of main.c (included before the viewers).

enum { VF_FIT, VF_FILL, VF_ORIG, VF_WIDTH, VF_STRETCH };
static const char *const VF_NAME[] = {"Fit to screen", "Fill the screen", "Original size", "Fit width", "Stretch to fill"};
static const char *const VF_SHORT[] = {"Fit", "Fill", "Original", "Width", "Stretch"};
static const char *const VOR_NAME[] = {"Landscape", "Portrait (on its side)"};
enum { VO_IFIT, VO_IORIENT, VO_VFIT, VO_VORIENT, VO_N };
static const char *const VO_ROW[VO_N] = {"Pictures: size", "Pictures: orientation", "Videos: size", "Videos: orientation"};
static const char *const VO_ICON[VO_N] = {"zoom", "rotate", "film", "rotate"};
static int *vo_val(int k) {
  switch (k) { case VO_IFIT: return &cfg.ifit; case VO_IORIENT: return &cfg.iorient; case VO_VFIT: return &cfg.vfit; default: return &cfg.vorient; }
}
static int vo_count(int k) { return k == VO_IFIT ? 4 : k == VO_VFIT ? 5 : 2; }
static const char *vo_text(int k) { int v = *vo_val(k); return k == VO_IORIENT || k == VO_VORIENT ? VOR_NAME[v] : VF_NAME[v]; }

// (what the full view shows now: a video's frame or a picture)
static int view_mode(int video) { return video ? cfg.vfit : cfg.ifit; }
static int view_tall; // (the picture shown is taller than wide: the full views set it before asking)
// (Settings › Turn portrait pictures: a tall picture turned to fill the screen held on its side)
static int view_portrait(int video) { return (video ? cfg.vorient == 1 : cfg.iorient == 1) || (cfg.autorot && view_tall); }

// The scale for a texture tw x th whose own pixels are ow x oh (0: not known), into a space aw x ah
// (the view's own: across and down as it's held). Returns how much bigger than "fit" that is.
static float view_fit(int mode, float tw, float th, float ow, float oh, float aw, float ah, float *sx, float *sy) {
  float fit = fminf(aw / tw, ah / th), s = fit;
  switch (mode) {
    case VF_FILL: s = fmaxf(aw / tw, ah / th); break;
    case VF_ORIG: s = ow > 0 && oh > 0 ? ow / tw : fit; break; // (each of its own pixels a screen pixel)
    case VF_WIDTH: s = aw / tw; break;
    case VF_STRETCH: *sx = aw / tw; *sy = ah / th; return fmaxf(*sx, *sy) / fit;
  }
  *sx = *sy = s;
  return s / fit;
}
// Draws t (tw x th of it shown: a video's frame can be a bigger texture) at its scale, its centre
// moved by (ox, oy) in the view's space; in portrait the view's across is the screen's down (turned
// clockwise, as the comic reader does it).
static void view_draw(vita2d_texture *t, int por, float tw, float th, float ox, float oy, float sx, float sy, float area_h, unsigned tint) {
  if (!por) { vita2d_draw_texture_tint_scale(t, (W - tw * sx) / 2 + ox, (area_h - th * sy) / 2 + oy, sx, sy, tint); return; }
  // (turned about the texture's own centre: moved so the shown part's centre is the view's)
  float ex = (vita2d_texture_get_width(t) - tw) / 2 * sx, ey = (vita2d_texture_get_height(t) - th) / 2 * sy;
  vita2d_draw_texture_tint_scale_rotate(t, W / 2.0f - oy - ey, area_h / 2.0f + ox + ex, sx, sy, 1.5707963f, tint);
}
// A drag, a stick or a tap turned into the view's space
static float view_dx(int por, float dx, float dy) { return por ? dy : dx; }
static float view_dy(int por, float dx, float dy) { return por ? -dx : dy; }

// ----- the panel -----
static int vo_open, vo_sel;
static void view_options_open(int video) { vo_open = 1; vo_sel = video ? VO_VFIT : VO_IFIT; sfx(SFX_OPEN); }
static void draw_view_options(void) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 160));
  const int w = 560, rh = 60, x = (W - w) / 2, h = VO_N * rh + 70, y = (H - h) / 2;
  rrect(x, y, w, h, 22, SURF);
  icon("image", x + 30, y + 30, ACC);
  text(x + 50, y + 37, TEXT, 20, "Viewing");
  text(x + w - 24 - text_w(13, "←→ change  ·  ○ done"), y + 36, FAINT, 13, "←→ change  ·  ○ done");
  int d = 0;
  for (int k = 0; k < VO_N; k++) {
    int ry = y + 58 + k * rh, on = k == vo_sel;
    if (on) rrect(x + 10, ry, w - 20, rh - 6, 14, SURF2);
    int cy = ry + (rh - 6) / 2;
    rrect(x + 22, cy - 16, 32, 32, 10, with_alpha(ACC, on ? 255 : 45));
    icon(VO_ICON[k], x + 38, cy, on ? RGBA(20, 12, 16, 255) : ACC);
    text(x + 68, cy + 6, TEXT, 17, VO_ROW[k]);
    const char *v = vo_text(k);
    text(x + w - 54 - text_w(16, v), cy + 6, ACC, 16, v);
    text(x + w - 40, cy + 7, on ? ACC : FAINT, 20, "›");
    if (on) focus_ring(x + 10, ry, w - 20, rh - 6);
    if (hit(x, ry, w, rh)) { vo_sel = k; d = in.tap_x < x + w / 2 ? -1 : 1; }
  }
  if (in.pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) vo_sel = (vo_sel + 1) % VO_N;
  if (in.pressed & SCE_CTRL_UP) vo_sel = (vo_sel + VO_N - 1) % VO_N;
  if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) d = 1;
  if (in.pressed & SCE_CTRL_LEFT) d = -1;
  if (d) { int *v = vo_val(vo_sel), n = vo_count(vo_sel); *v = (*v + d + n) % n; sfx(SFX_TOGGLE); cfg_save(); }
  if ((in.pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE)) || (in.tap && (in.tap_x < x || in.tap_x > x + w || in.tap_y < y || in.tap_y > y + h))) { sfx(SFX_CLOSE); vo_open = 0; }
  in.pressed = 0; in.tap = 0; in.touching = 0; in.dbl = 0; in.flicked = 0;
}
// the top bar's button: a badge with the size's name, its right edge at rx
static void view_btn_rect(int video, int rx, int *bx, int *bw) { *bw = text_w(13, VF_SHORT[view_mode(video)]) + 40; *bx = rx - *bw; }
static void view_btn_draw(int video, int a, int rx) {
  int bx, bw;
  view_btn_rect(video, rx, &bx, &bw);
  rrect(bx, 16, bw, 28, 14, with_alpha(SURF2, a * 3 / 4));
  icon(view_portrait(video) ? "rotate" : "zoom", bx + 16, 30, with_alpha(TEXT, a));
  text(bx + 30, 35, with_alpha(TEXT, a), 13, VF_SHORT[view_mode(video)]);
}
static int view_btn_hit(int video, int rx) {
  int bx, bw;
  view_btn_rect(video, rx, &bx, &bw);
  return in.tap && in.tap_y < 60 && in.tap_x >= bx - 6 && in.tap_x < bx + bw + 6;
}

// One finger's drag in a full view: moves a picture bigger than the screen (zoomed in, or by its
// size: past its edge sideways, a swipe to the next), and in portrait a swipe along the view.
// Returns 0 when it's neither, for the view's own gestures.
static int view_drag(int por, float zoom, float mx, float my, float *pan_x, float *pan_y, float *swipe) {
  float vdx = view_dx(por, in.drag_dx, in.drag_dy), vdy = view_dy(por, in.drag_dx, in.drag_dy);
  float mvx = view_dx(por, in.tx - in.down_x, in.ty - in.down_y), mvy = view_dy(por, in.tx - in.down_x, in.ty - in.down_y);
  int across = fabsf(mvx) > fabsf(mvy);
  if (zoom > 1.01f) { *pan_x += vdx; *pan_y += vdy; return 1; }
  if (mx > 1 || my > 1) {
    int at_edge = mx <= 1 || (vdx > 0 && *pan_x >= mx - 0.5f) || (vdx < 0 && *pan_x <= -mx + 0.5f);
    if (across && (fabsf(*swipe) > 0.5f || at_edge)) *swipe += vdx;
    else { *pan_x += vdx; *pan_y += vdy; }
    return 1;
  }
  if (por) { if (across) *swipe += vdx; return 1; } // (portrait: no swipe down to close, no pull-up)
  return 0;
}
// where a tap is in the view's space (its centre's offset, for zooming in on it)
static void view_tap_off(int por, float aw, float ah, float *ox, float *oy) {
  float tx = por ? in.tap_y : in.tap_x, ty = por ? W - in.tap_x : in.tap_y;
  *ox = aw / 2 - tx; *oy = ah / 2 - ty;
}

// Two fingers in a full view: zoom towards the point between them (the picture under the fingers
// stays under them) and move with them. d0 0: the pinch just began. pan is the picture's centre
// moved from the view's, in the view's space; zoom 1 fits.
static void view_pinch(int por, float aw, float ah, float *zoom, float *pan_x, float *pan_y, float *d0, float *z0, float *pmx, float *pmy) {
  float d = hypotf(in.t2x - in.tx, in.t2y - in.ty);
  float sx = (in.tx + in.t2x) / 2.0f, sy = (in.ty + in.t2y) / 2.0f;
  float vx = por ? sy : sx, vy = por ? W - sx : sy; // (the midpoint in the view's space)
  float mx = vx - aw / 2, my = vy - ah / 2;
  if (*d0 <= 0 || d < 1) { *d0 = d > 1 ? d : 1; *z0 = *zoom; *pmx = mx; *pmy = my; return; }
  float zold = *zoom, znew = fminf(6, fmaxf(1, *z0 * d / *d0));
  // (what was under the fingers last frame: kept under them now, at the new size)
  float cx = (*pmx - *pan_x) / zold, cy = (*pmy - *pan_y) / zold;
  *pan_x = mx - cx * znew;
  *pan_y = my - cy * znew;
  *zoom = znew;
  *pmx = mx; *pmy = my;
  if (znew <= 1.001f) { *pan_x = 0; *pan_y = 0; }
}
