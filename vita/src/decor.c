// The little anime girls of the PC and the phones (art/decor, made Vita-sized by tools/make-decor.py
// into app0:decor): standing beside empty screens and peeking from corners, breathing gently, and a
// bounce when one is tapped. Drawn in rose, they're turned to the accent's colour the way the PC turns
// them (CSS hue-rotate's matrix), greys for a grey accent; made again when the accent changes.
// Settings › Decorations turns them off. Compiled inside main.c.
enum { DEC_BIKINI, DEC_CORSET, DEC_CROPTOP, DEC_SONICO, DEC_SUIT, DEC_TEARJU, DEC_YORUICHI, DEC_PEACE, DEC_N };
static const char *const DEC_FILE[DEC_N] = {"bikini", "corset", "croptop", "sonico", "suit", "tearju", "yoruichi", "peace"};
static vita2d_texture *dec_tex[DEC_N];
static unsigned dec_acc[DEC_N];
static unsigned dec_bounce_at[DEC_N];
static int dec_failed[DEC_N];
// (the accent's hue against rose's, 349°: how far to turn; grey: -1000)
static float dec_turn(void) {
  float r = (ACC & 255) / 255.f, g = ((ACC >> 8) & 255) / 255.f, b = ((ACC >> 16) & 255) / 255.f;
  float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn, h = 0;
  if (d > 0) h = mx == r ? fmodf((g - b) / d, 6) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
  h = fmodf(h * 60 + 360, 360);
  float l = (mx + mn) / 2, s = d > 0 ? d / (1 - fabsf(2 * l - 1)) : 0;
  if (s < 0.2f) return -1000;
  float t = fmodf(h - 349 + 540, 360) - 180;
  return fabsf(t) < 6 ? 0 : t;
}
static void dec_tint(LvPixels *px) {
  float t = dec_turn();
  if (t == 0) return;
  float m[9];
  if (t <= -999) { // (grey, as CSS grayscale(1))
    const float g[9] = {0.2126f, 0.7152f, 0.0722f, 0.2126f, 0.7152f, 0.0722f, 0.2126f, 0.7152f, 0.0722f};
    memcpy(m, g, sizeof m);
  } else {
    float a = cosf(t * 3.14159265f / 180), b = sinf(t * 3.14159265f / 180);
    const float h[9] = {0.213f + 0.787f * a - 0.213f * b, 0.715f - 0.715f * a - 0.715f * b, 0.072f - 0.072f * a + 0.928f * b,
                        0.213f - 0.213f * a + 0.143f * b, 0.715f + 0.285f * a + 0.140f * b, 0.072f - 0.072f * a - 0.283f * b,
                        0.213f - 0.213f * a - 0.787f * b, 0.715f - 0.715f * a + 0.715f * b, 0.072f + 0.928f * a + 0.072f * b};
    memcpy(m, h, sizeof m);
  }
  for (size_t i = 0, n = (size_t)px->w * px->h; i < n; i++) {
    uint8_t *p = px->rgba + i * 4;
    if (!p[3]) continue;
    float r = p[0], g = p[1], b = p[2];
    float nr = m[0] * r + m[1] * g + m[2] * b, ng = m[3] * r + m[4] * g + m[5] * b, nb = m[6] * r + m[7] * g + m[8] * b;
    p[0] = (uint8_t)(nr < 0 ? 0 : nr > 255 ? 255 : nr);
    p[1] = (uint8_t)(ng < 0 ? 0 : ng > 255 ? 255 : ng);
    p[2] = (uint8_t)(nb < 0 ? 0 : nb > 255 ? 255 : nb);
  }
}
static vita2d_texture *dec_get(int k) {
  if (k < 0 || k >= DEC_N || dec_failed[k]) return NULL;
  if (dec_tex[k] && dec_acc[k] == ACC) return dec_tex[k];
  char path[64];
  snprintf(path, sizeof path, "app0:decor/%s.webp", DEC_FILE[k]);
  size_t n = 0;
  uint8_t *b = lv_read_all(path, NULL, &n);
  LvPixels px;
  memset(&px, 0, sizeof px);
  if (!b || lv_decode(b, n, k == DEC_PEACE ? 1024 : 400, k == DEC_PEACE ? 1024 : 400, &px) != 0 || !px.rgba) { free(b); dec_failed[k] = 1; return dec_tex[k]; }
  free(b);
  dec_tint(&px);
  vita2d_texture *t = upload(&px);
  free(px.rgba);
  if (dec_tex[k]) free_later(dec_tex[k]);
  dec_tex[k] = t;
  dec_acc[k] = ACC;
  return t;
}
// One standing with her feet at (cx, bottom): breathing, a bounce when tapped (a tap on her says 1).
// h: how tall (her picture's height is 340); clip_y: nothing of her drawn below it (a peek: 0 none).
static int decor_draw(int k, float cx, float bottom, float h, int alpha) {
  if (cfg.nodecor) return 0;
  vita2d_texture *t = dec_get(k);
  if (!t) return 0;
  float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
  int sheet = k == DEC_PEACE; // (an animation: 25 frames in a 5 x 5 sheet, 70 ms each)
  float fx = 0, fy = 0;
  if (sheet) { int f = (int)(frame_no * 1000 / 60 / 70) % 25; tw /= 5; th /= 5; fx = (f % 5) * tw; fy = (f / 5) * th; }
  float sc = h / th;
  int still = alpha < 0; // (a negative alpha: drawn still, no breathing or bobbing)
  if (still) alpha = -alpha;
  float t_s = still ? 0 : frame_no / 60.0f;
  float breath = 1 + 0.012f * sinf(t_s * 1.6f + k);       // (a slow breath: taller and back)
  float bob = still ? 0 : sinf(t_s * 1.1f + k * 1.7f) * 2.5f;
  if (still) breath = 1;
  float hop = 0, squash = 1;
  if (dec_bounce_at[k]) {
    float u = (frame_no - dec_bounce_at[k]) / 36.0f;       // (a hop: up, down, a little squash as she lands)
    if (u >= 1) dec_bounce_at[k] = 0;
    else { hop = sinf(u * 3.14159f) * 26 * (1 - u * 0.3f); squash = 1 + 0.06f * sinf(u * 6.28318f) * (1 - u); }
  }
  float w = tw * sc / squash, hh = th * sc * breath * squash;
  float x = cx - w / 2, y = bottom - hh - hop + bob;
  if (sheet) vita2d_draw_texture_tint_part_scale(t, x, y, fx, fy, tw, th, w / tw, hh / th, RGBA(255, 255, 255, alpha));
  else vita2d_draw_texture_tint_scale(t, x, y, w / tw, hh / th, RGBA(255, 255, 255, alpha));
  if (in.tap && in.tap_x >= x && in.tap_x < x + w && in.tap_y >= y && in.tap_y < bottom) {
    dec_bounce_at[k] = frame_no ? frame_no : 1;
    sfx(SFX_TICK);
    in.tap = 0;
    return 1;
  }
  return 0;
}
