// The slideshow as the PC runs it, and Continue watching. Compiled inside main.c (#include "show.c").
//
// A session climbs the NSFW levels: the first stage holds the levels from the start up to "first
// stage up to", then one level at a time to the last, each for some minutes; slides go from slow to
// faster and faster (the pace eases in, as the PC's "accel" curve). While it runs, what's above the
// level reached is locked in the Library. Only time with the app open counts; a session left alone for
// six hours ends (the PC's SESSION_IDLE_HOURS). Its plan and its clock are in ux0:data/LWDV00001/
// slideshow.txt. "Simple" is the old slideshow: the same few seconds each, everything in the Library.
//
// Continue watching: where a video was left, in the vault's _app/kv/resume.json, as the PC keeps it
// ({"<id>": {"t": seconds, "at": ms since 1970}}; encrypted in an Untraceable vault), so a video
// stopped on the PC carries on on the Vita and the other way round.

// ---------- Continue watching ----------
typedef struct { char id[48]; int t; double at; } Resume;
static Resume *resumes;
static int nresume, capresume, resume_dirty;

static void resume_path(char *o, size_t n) { snprintf(o, n, "%s/_app/kv/resume.json", vault.root); }
static void resume_load(void) {
  free(resumes);
  resumes = NULL;
  nresume = capresume = resume_dirty = 0;
  char p[700];
  resume_path(p, sizeof p);
  size_t n = 0;
  uint8_t *b = lv_read_all(p, lv_key(&vault), &n);
  if (!b) return;
  Json j;
  if (json_parse(&j, (const char *)b, n) == 0) {
    if (j.n && j.t[0].type == J_OBJ && j.t[0].size > 0) {
      capresume = j.t[0].size;
      resumes = calloc(capresume, sizeof *resumes);
      for (int k = 0; resumes && k < capresume; k++) {
        int v = json_at(&j, 0, k);
        if (v < 1 || j.t[v].type != J_OBJ) continue;
        Resume *r = &resumes[nresume];
        json_str(&j, v - 1, r->id, sizeof r->id);
        r->t = (int)json_gn(&j, v, "t");
        r->at = json_gn(&j, v, "at");
        if (r->id[0] && r->t > 0) nresume++;
      }
      if (!resumes) capresume = 0;
    }
    json_free(&j);
  }
  free(b);
}
static int resume_find(const char *id) {
  for (int k = 0; k < nresume; k++) if (!strcmp(resumes[k].id, id)) return k;
  return -1;
}
static int resume_of(const LvItem *it) { int k = resume_find(it->id); return k < 0 ? 0 : resumes[k].t; }
// secs: where it stopped (0: it was watched to the end, or hardly begun: forgotten)
static void resume_set(const LvItem *it, int secs) {
  if (!it->id[0] || strlen(it->id) >= sizeof resumes->id) return;
  int k = resume_find(it->id);
  if (secs <= 0) {
    if (k < 0) return;
    resumes[k] = resumes[--nresume];
    resume_dirty = 1;
    return;
  }
  if (k < 0) {
    if (nresume == capresume) {
      int c = capresume ? capresume * 2 : 32;
      Resume *nr = realloc(resumes, c * sizeof *nr);
      if (!nr) return;
      resumes = nr;
      capresume = c;
    }
    k = nresume++;
    snprintf(resumes[k].id, sizeof resumes[k].id, "%s", it->id);
  }
  if (resumes[k].t == secs) return;
  resumes[k].t = secs;
  resumes[k].at = (double)time(NULL) * 1000.0;
  resume_dirty = 1;
}
static void resume_save(void) {
  if (!resume_dirty || !vault.root[0]) return;
  size_t cap = 16 + (size_t)nresume * 160; // (an id of up to 97 characters, escaped, and its numbers)
  char *s = malloc(cap);
  if (!s) return;
  size_t o = 0;
  s[o++] = '{';
  for (int k = 0; k < nresume; k++) {
    char id[100];
    size_t m = 0;
    for (const char *c = resumes[k].id; *c && m < sizeof id - 3; c++) {
      if (*c == '"' || *c == '\\') id[m++] = '\\';
      if ((unsigned char)*c >= 32) id[m++] = *c;
    }
    id[m] = 0;
    o += snprintf(s + o, cap - o, "%s\"%s\":{\"t\":%d,\"at\":%.0f}", k ? "," : "", id, resumes[k].t, resumes[k].at);
    if (o >= cap - 2) { free(s); return; }
  }
  s[o++] = '}';
  char p[700];
  snprintf(p, sizeof p, "%s/_app", vault.root);
  sceIoMkdir(p, 0777);
  snprintf(p, sizeof p, "%s/_app/kv", vault.root);
  sceIoMkdir(p, 0777);
  resume_path(p, sizeof p);
  if (lv_write_all(p, lv_key(&vault), s, o) == 0) resume_dirty = 0;
  free(s);
}

// ---------- the session ----------
enum { PRE_SIMPLE, PRE_GENTLE, PRE_STANDARD, PRE_FAST };
static const char *PRESETS[] = {"Simple", "Gentle", "Standard", "Fast"};
static const char *MOTIONS[] = {"None", "Slow pan", "Slow zoom", "Breathing"};
static const int SLEEPS[] = {0, 15, 30, 45, 60, 90};
static struct {
  int preset, lo, first, hi, below, motion, fade, sleep, lock;
  float minutes, s0, s1;
} so = {PRE_STANDARD, 1, 2, 10, 0, 0, 1, 0, 1, 5, 8, 1.5f};
static struct {
  int on;
  double ms;      // the session's time so far
  int nst, st_lo[10], st_hi[10];
  int stage;      // (the one last seen: a change is said out loud)
  int *q, qn, qpos, qstage; // the stage's items, shuffled, as positions in shown
  uint64_t last_us, saved_us;
  double play_ms; // the slideshow playing: for the sleep timer
} ss;

static void preset(int p) {
  so.preset = p;
  switch (p) {
    case PRE_GENTLE: so.first = 2; so.minutes = 8; so.s0 = 10; so.s1 = 3; so.fade = 1; so.motion = 1; break;
    case PRE_STANDARD: so.first = 2; so.minutes = 5; so.s0 = 8; so.s1 = 1.5f; so.fade = 1; so.motion = 1; break;
    case PRE_FAST: so.first = 2; so.minutes = 2; so.s0 = 4; so.s1 = 0.6f; so.fade = 0; so.motion = 0; break;
  }
}
static unsigned show_dirty_at;
static void show_write(void);
static void show_save(void) { show_dirty_at = frame_no ? frame_no : 1; } // (written a second later: main's loop)
static void show_write(void) {
  show_dirty_at = 0;
  FILE *f = fopen(DATA "/slideshow.txt.part", "w");
  if (!f) return;
  fprintf(f, "%d %d %d %d %d %d %d %d %d %.2f %.2f %.2f\n", so.preset, so.lo, so.first, so.hi, so.below, so.motion, so.fade, so.sleep, so.lock,
          so.minutes, so.s0, so.s1);
  fprintf(f, "%d %.0f %lld\n", ss.on, ss.ms, (long long)time(NULL));
  fclose(f);
  sceIoRemove(DATA "/slideshow.txt");
  sceIoRename(DATA "/slideshow.txt.part", DATA "/slideshow.txt");
}
static int has_level(int lo, int hi) {
  for (int k = 0; k < nshown; k++) { int l = vault.items[shown[k]].nsfw; if (l >= lo && l <= hi) return 1; }
  return 0;
}
// The stages from the plan: [lo .. first], then each level up to hi; the empty ones left out.
static void make_stages(void) {
  // (the plan's page asks every frame: worked out again only when the levels, the Library's list or
  //  the vault changed, not with a pass over every item for each level, sixty times a second)
  static int k_lo = -1, k_hi, k_first, k_n, k_vgen, k_vn;
  static const int *k_shown;
  if (k_lo == so.lo && k_hi == so.hi && k_first == so.first && k_n == nshown && k_shown == shown && k_vgen == vgen && k_vn == vault.n && ss.nst) return;
  k_lo = so.lo; k_hi = so.hi; k_first = so.first; k_n = nshown; k_shown = shown; k_vgen = vgen; k_vn = vault.n;
  int lo = so.lo < so.hi ? so.lo : so.hi, hi = so.lo < so.hi ? so.hi : so.lo;
  int first = so.first < lo ? lo : so.first > hi ? hi : so.first;
  ss.nst = 0;
  if (has_level(lo, first)) { ss.st_lo[0] = lo; ss.st_hi[0] = first; ss.nst = 1; }
  for (int l = first + 1; l <= hi && ss.nst < 10; l++)
    if (has_level(l, l)) { ss.st_lo[ss.nst] = l; ss.st_hi[ss.nst] = l; ss.nst++; }
}
static double level_ms(void) { return so.minutes * 60000.0; }
static int stage_now(void) {
  if (!ss.on || !ss.nst) return 0;
  int k = (int)(ss.ms / level_ms());
  return k >= ss.nst ? ss.nst - 1 : k;
}
// The highest level open now (10 with no session, or with the lock off)
static int unlocked_level(void) { return ss.on && so.lock && ss.nst ? ss.st_hi[stage_now()] : 10; }
static int is_locked(int i) {
  // (asked for every card every frame: the level open worked out once a frame)
  static unsigned at = ~0u;
  static int lvl;
  if (at != frame_no) { at = frame_no; lvl = unlocked_level(); }
  return vault.items[i].nsfw > lvl;
}
static void show_load(void) {
  FILE *f = fopen(DATA "/slideshow.txt", "r");
  if (!f) return;
  int on = 0;
  double ms = 0;
  long long seen = 0;
  if (fscanf(f, "%d %d %d %d %d %d %d %d %d %f %f %f", &so.preset, &so.lo, &so.first, &so.hi, &so.below, &so.motion, &so.fade, &so.sleep, &so.lock,
             &so.minutes, &so.s0, &so.s1) == 12 &&
      fscanf(f, "%d %lf %lld", &on, &ms, &seen) == 3 && on && time(NULL) - seen < 6 * 3600) {
    ss.on = 1;
    ss.ms = ms;
  }
  fclose(f);
  if (so.preset < 0 || so.preset > 3) so.preset = PRE_STANDARD;
  if (so.lo < 1 || so.lo > 10) so.lo = 1;
  if (so.hi < 1 || so.hi > 10) so.hi = 10;
  if (so.first < 1 || so.first > 10) so.first = 2;
  if (so.minutes < 0.5f || so.minutes > 120) so.minutes = 5;
  if (so.s0 < 0.5f || so.s0 > 120) so.s0 = 8;
  if (so.s1 < 0.3f || so.s1 > 120) so.s1 = 1.5f;
  if (so.motion < 0 || so.motion > 3) so.motion = 0;
  if (so.sleep < 0 || so.sleep > 5) so.sleep = 0;
  ss.stage = -1;
}
static void show_opened(void) { resume_load(); ss.nst = 0; ss.qn = 0; ss.qstage = -1; }
static void session_end(const char *why) {
  ss.on = 0;
  ss.ms = 0;
  ss.qn = 0;
  show_save();
  if (why) say(why);
}
static void session_start(void) {
  make_stages();
  if (!ss.nst) { sfx(SFX_ERROR); say("Nothing in the Library at these levels"); return; }
  ss.on = 1;
  ss.ms = 0;
  ss.stage = -1;
  ss.qn = 0;
  ss.qstage = -1;
  show_save();
}
// Seconds for the slide now: from the start's to the end's, gentle at first (the PC's "accel")
static float pace_now(void) {
  if (!ss.on || !ss.nst) return (float)cfg.interval;
  double p = ss.ms / (ss.nst * level_ms());
  if (p > 1) p = 1;
  return so.s0 + (so.s1 - so.s0) * (float)(p * p);
}
// The next slide, as a position in shown: the stage's items, shuffled, round and round (-1: none)
static int session_pick(int now_k) {
  int k = stage_now();
  if (ss.qstage != k || ss.qpos >= ss.qn) {
    free(ss.q);
    ss.q = malloc((nshown + 1) * sizeof *ss.q);
    ss.qn = ss.qpos = 0;
    ss.qstage = k;
    if (!ss.q) return -1;
    int lo = so.below ? 1 : ss.st_lo[k], hi = ss.st_hi[k];
    for (int n = 0; n < nshown; n++) { int l = vault.items[shown[n]].nsfw; if (l >= lo && l <= hi) ss.q[ss.qn++] = n; }
    for (int n = ss.qn - 1; n > 0; n--) { int j = rnd() % (n + 1), t = ss.q[n]; ss.q[n] = ss.q[j]; ss.q[j] = t; }
    if (ss.qn > 1 && ss.q[0] == now_k) { int t = ss.q[0]; ss.q[0] = ss.q[ss.qn - 1]; ss.q[ss.qn - 1] = t; } // (no instant repeat)
  }
  return ss.qn ? ss.q[ss.qpos++] : -1;
}
// Once a frame: the session's clock (only while the app is open), the level reached, the sleep timer.
// Returns 1 when the sleep timer ran out (the slideshow stops).
static int show_tick(int playing) {
  uint64_t now = sceKernelGetProcessTimeWide();
  uint64_t d = ss.last_us ? now - ss.last_us : 0;
  ss.last_us = now;
  if (d > 1000000) d = 1000000; // (asleep, or a long load: not counted)
  ss.play_ms = playing ? ss.play_ms + d / 1000.0 : 0;
  if (ss.on) {
    ss.ms += d / 1000.0;
    if (!ss.nst) make_stages();
    int k = stage_now();
    if (k != ss.stage) {
      if (ss.stage >= 0 && ss.nst) {
        char m[64];
        if (ss.st_lo[k] == ss.st_hi[k]) snprintf(m, sizeof m, "Level %d unlocked", ss.st_hi[k]);
        else snprintf(m, sizeof m, "Levels %d to %d", ss.st_lo[k], ss.st_hi[k]);
        sfx(SFX_MORE);
        say(m);
      }
      ss.stage = k;
    }
    if (now - ss.saved_us > 10000000) { ss.saved_us = now; show_save(); }
  }
  if (playing && so.sleep && ss.play_ms > SLEEPS[so.sleep] * 60000.0) { ss.play_ms = 0; return 1; }
  return 0;
}

// ---------- the Slideshow tab: the plan ----------

// The slideshow's page: on the left a card of what it will show (a few of the pictures, the kind,
// the levels as a bar, where the session is) with Start under it; on the right its settings as tiles,
// two across, each its name small and its value big with ‹ › either side. ↑↓ go through the tiles
// (Kind first, Start last), ←→ change one, ✕ or START starts; a tap on a tile's left or right half
// changes it down or up, a tap on a kind picks it.
static void draw_show_plan(void) {
  int levels = so.preset != PRE_SIMPLE;
  if (levels) make_stages();
  // --- the tiles ---
  enum { R_PRESET, R_FROM, R_FIRST, R_TO, R_MIN, R_PACE, R_MIX, R_INTERVAL, R_SHUFFLE, R_MOTION, R_FADE, R_SLEEP };
  struct { const char *name, *icon; char value[40]; int id; } t[12];
  int n = 0;
#define TILE(nm, ic, id_, ...) do { t[n].name = nm; t[n].icon = ic; t[n].id = id_; snprintf(t[n].value, sizeof t[n].value, __VA_ARGS__); n++; } while (0)
  if (levels) {
    TILE("Levels", "sliders", R_FROM, "%d to %d", so.lo, so.hi);
    TILE("First stage", "tag", R_FIRST, "up to %d", so.first);
    TILE("Each level", "refresh", R_MIN, "%g min", so.minutes);
    TILE("Each slide", "play", R_PACE, "%g → %g s", so.s0, so.s1);
    TILE("Show", "photos", R_MIX, "%s", so.below ? "It and below" : "That level");
  } else {
    TILE("Each picture", "refresh", R_INTERVAL, "%d s", cfg.interval);
    TILE("Order", "sort", R_SHUFFLE, "%s", cfg.shuffle ? "Shuffled" : "In order");
  }
  TILE("Motion", "image", R_MOTION, "%s", MOTIONS[so.motion]);
  TILE("Fade", "film", R_FADE, "%s", so.fade ? "On" : "Off");
  if (so.sleep) TILE("Sleep timer", "info", R_SLEEP, "%d min", SLEEPS[so.sleep]);
  else TILE("Sleep timer", "info", R_SLEEP, "Off");
#undef TILE
  // focus: 0 the kind, 1..n the tiles, n + 1 Start
  int nf = n + 2;
  if (plan_sel >= nf) plan_sel = nf - 1;
  if (plan_sel < 0) plan_sel = 0;

  // --- the left card ---
  const int cx = 24, cy = 18, cw = 400, ch = H - BAR - 30;
  rrect(cx, cy, cw, ch, 22, SURF);
  // a mosaic of what's in it: the first pictures shown (their thumbnails, when made)
  {
    const int mh = 138;
    for (int k = 0; k < 3; k++) {
      float x = cx + 12 + k * ((cw - 24 - 16) / 3.0f + 8), w = (cw - 24 - 16) / 3.0f;
      int item = k < nshown ? shown[(k * 7) % (nshown ? nshown : 1)] : -1;
      vita2d_texture *th = item >= 0 ? thumb_of(item) : NULL;
      if (th) draw_cover(th, x, cy + 12, w, mh, RGBA(255, 255, 255, 255));
      else rrect(x, cy + 12, w, mh, 12, with_alpha(k & 1 ? ACC2 : ACC, 50));
    }
    grad(cx + 12, cy + 12 + mh - 60, cw - 24, 60, SURF, 0, 255, 0);
    // the play mark over it
    circle(cx + cw / 2, cy + 12 + mh / 2, 30, RGBA(0, 0, 0, 140));
    vc_tri(cx + cw / 2 + 4, cy + 12 + mh / 2, 15, TEXT);
  }
  // the kinds, as a segmented control
  int ky = cy + 162, segw = cw - 24, sw4 = segw / 4;
  rrect(cx + 12, ky, segw, 36, 18, SURF2);
  rrect(cx + 12 + 3 + so.preset * sw4, ky + 3, sw4 - 6, 30, 15, ACC);
  for (int k = 0; k < 4; k++) {
    text_center(cx + 12 + k * sw4 + sw4 / 2, ky + 24, k == so.preset ? RGBA(20, 12, 16, 255) : TEXT, 15, PRESETS[k]);
    if (hit(cx + 12 + k * sw4, ky, sw4, 36) && k != so.preset) {
      if (ss.on) say("End the session to change its kind");
      else { sfx(SFX_TOGGLE); preset(k); show_save(); }
      plan_sel = 0;
    }
  }
  if (plan_sel == 0) focus_ring(cx + 12, ky, segw, 36);
  // what it will do, or where the session is
  char status[160];
  if (ss.on && ss.nst) {
    int k = stage_now();
    double left = (k + 1) * level_ms() - ss.ms;
    if (k < ss.nst - 1) snprintf(status, sizeof status, "Level %d reached · the next in %d:%02d", ss.st_hi[k], (int)(left / 60000), (int)(left / 1000) % 60);
    else snprintf(status, sizeof status, "The last level, %d", ss.st_hi[k]);
  } else if (levels) snprintf(status, sizeof status, "%d %s · %d min · %.1f s down to %.1f s", ss.nst, ss.nst == 1 ? "stage" : "stages", (int)(ss.nst * so.minutes + 0.5f), so.s0, so.s1);
  else snprintf(status, sizeof status, "Everything in the Library · %d s each", cfg.interval);
  text_fit(cx + 16, ky + 64, DIM, 15, status, cw - 32);
  // the levels: ten segments, those in the plan lit, those passed full, the one now filling
  if (levels) {
    int k = ss.on ? stage_now() : -1;
    float p = ss.on ? (float)((ss.ms - k * level_ms()) / level_ms()) : 0;
    if (p > 1) p = 1;
    float lx = cx + 16, lw = (cw - 32 - 9 * 4) / 10.0f, ly = ky + 80;
    for (int l = 1; l <= 10; l++) {
      float x = lx + (l - 1) * (lw + 4);
      int in_plan = 0;
      for (int s2 = 0; s2 < ss.nst; s2++) if (l >= ss.st_lo[s2] && l <= ss.st_hi[s2]) in_plan = 1;
      int passed = ss.on && l < ss.st_lo[k], now = ss.on && l >= ss.st_lo[k] && l <= ss.st_hi[k];
      rrect(x, ly, lw, 22, 6, in_plan ? SURF2 : with_alpha(SURF2, 80));
      if (passed) rrect(x, ly, lw, 22, 6, with_alpha(ACC, 160));
      if (now) rrect(x, ly, lw * (p < 0.3f ? 0.3f : p), 22, 6, ACC);
      char b[4];
      snprintf(b, sizeof b, "%d", l);
      text_center((int)(x + lw / 2), (int)ly + 16, in_plan ? TEXT : FAINT, 12, b);
    }
  }
  // what the kind is, in a few words, and how much there is
  {
    static const char *const ABOUT[] = {
      "The Library's pictures and GIFs, and clips of half a minute or less, one after another, at one pace.",
      "Starts low and climbs the levels slowly: 8 minutes a level, slides from 10 s down to 3 s, fading and drifting.",
      "Climbs the levels steadily: 5 minutes a level, slides from 8 s down to 1.5 s.",
      "Up the levels quickly: 2 minutes a level, slides from 4 s down to under a second.",
    };
    int y = text_wrap(cx + 16, ky + (levels ? 128 : 92), FAINT, 14, ABOUT[so.preset], cw - 32, 20);
    char cnt[64];
    snprintf(cnt, sizeof cnt, "%d in the Library now", nshown);
    if (y < cy + ch - 70) text(cx + 16, y + 4, DIM, 14, cnt);
  }
  // Start (or carry on), and end the session
  const char *go = ss.on && levels ? "▶  Carry on" : "▶  Start";
  int bw = ss.on ? (cw - 24 - 10) / 2 : cw - 24, by = cy + ch - 56;
  rrect(cx + 12, by, bw, 44, 22, ACC);
  text_center(cx + 12 + bw / 2, by + 29, RGBA(20, 12, 16, 255), 18, go);
  if (plan_sel == nf - 1) focus_ring(cx + 12, by, bw, 44);
  int start = hit(cx + 12, by, bw, 44);
  if (ss.on) {
    rrect(cx + 22 + bw, by, bw, 44, 22, SURF2);
    text_center(cx + 22 + bw + bw / 2, by + 29, TEXT, 16, "End session  □");
    if (hit(cx + 22 + bw, by, bw, 44)) { sfx(SFX_CLOSE); session_end("Session ended"); }
  }

  // --- the tiles, two across ---
  const int gx = cx + cw + 16, gw = W - 24 - gx, tw = (gw - 12) / 2;
  int rows = (n + 1) / 2, th = (ch - (rows - 1) * 12) / rows;
  if (th > 104) th = 104;
  int d = (in.pressed & SCE_CTRL_RIGHT) ? 1 : (in.pressed & SCE_CTRL_LEFT) ? -1 : 0;
  int tapped = -1, tap_d = 0;
  for (int k = 0; k < n; k++) {
    int x = gx + (k % 2) * (tw + 12), y = cy + (k / 2) * (th + 12);
    int on = plan_sel == k + 1;
    rrect(x, y, tw, th, 18, on ? SURF2 : SURF);
    circle(x + 30, y + 30, 16, with_alpha(ACC, on ? 255 : 60));
    icon(t[k].icon, x + 30, y + 30, on ? RGBA(20, 12, 16, 255) : ACC);
    text(x + 56, y + 36, DIM, 14, t[k].name);
    text_fit(x + 18, y + th - 20, TEXT, th > 90 ? 24 : 20, t[k].value, tw - 70);
    // ‹ › to change it
    text(x + tw - 50, y + th - 20, on ? ACC : FAINT, 22, "‹");
    text(x + tw - 26, y + th - 20, on ? ACC : FAINT, 22, "›");
    if (on) focus_ring(x, y, tw, th);
    if (hit(x, y, tw, th)) { tapped = k; tap_d = in.tap_x < x + tw / 2 ? -1 : 1; }
  }
  if (tapped >= 0) { plan_sel = tapped + 1; d = tap_d; }

  // --- the buttons ---
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) plan_sel = (plan_sel + 1) % nf;
  if (in.pressed & SCE_CTRL_UP) plan_sel = (plan_sel + nf - 1) % nf;
  if (d && plan_sel == 0) {
    if (ss.on) say("End the session to change its kind");
    else { sfx(SFX_TOGGLE); preset((so.preset + d + 4) % 4); show_save(); }
  } else if (d && plan_sel >= 1 && plan_sel <= n) {
    sfx(SFX_TOGGLE);
    switch (t[plan_sel - 1].id) {
      case R_FROM: if (ss.on) { say("These stay until the session ends"); break; }
                   if (in.now & SCE_CTRL_RTRIGGER) so.hi = (so.hi + d + 9) % 10 + 1; else so.lo = (so.lo + d + 9) % 10 + 1;
                   if (so.hi < so.lo) so.hi = so.lo;
                   break;
      case R_FIRST: if (!ss.on) so.first = (so.first + d + 9) % 10 + 1; break;
      case R_MIN: so.minutes += d * (so.minutes >= 10 ? 5 : so.minutes >= 2 ? 1 : 0.5f); if (so.minutes < 0.5f) so.minutes = 60; if (so.minutes > 60) so.minutes = 0.5f; break;
      case R_PACE:
        if (in.now & SCE_CTRL_RTRIGGER) { so.s1 += d * 0.5f; if (so.s1 < 0.5f) so.s1 = 10; if (so.s1 > 10) so.s1 = 0.5f; }
        else { so.s0 += d; if (so.s0 < 1) so.s0 = 30; if (so.s0 > 30) so.s0 = 1; }
        break;
      case R_MIX: so.below = !so.below; ss.qn = 0; break;
      case R_INTERVAL: cfg.interval = cfg.interval + d * (cfg.interval >= 10 ? 5 : 1); if (cfg.interval > 60) cfg.interval = 2; if (cfg.interval < 2) cfg.interval = 60; cfg_save(); break;
      case R_SHUFFLE: cfg.shuffle = !cfg.shuffle; cfg_save(); break;
      case R_MOTION: so.motion = (so.motion + d + 4) % 4; break;
      case R_FADE: so.fade = !so.fade; break;
      case R_SLEEP: so.sleep = (so.sleep + d + 6) % 6; break;
    }
    show_save();
  }
  // a hint for the two-ended ones
  int fid = plan_sel >= 1 && plan_sel <= n ? t[plan_sel - 1].id : -1;
  if (fid == R_FROM) text(gx, H - BAR - 2, FAINT, 13, "←→ the first level  ·  hold R: the last one");
  else if (fid == R_PACE) text(gx, H - BAR - 2, FAINT, 13, "←→ the first slides  ·  hold R: the last ones");
  if ((in.pressed & SCE_CTRL_SQUARE) && ss.on) { sfx(SFX_CLOSE); session_end("Session ended"); }
  if ((in.pressed & SCE_CTRL_CROSS) || (in.pressed & SCE_CTRL_START) || start) {
    if (!nshown) { sfx(SFX_ERROR); say("No pictures, GIFs or short clips to show"); }
    else if (levels) {
      if (!ss.on) session_start();
      if (ss.on) {
        int k = session_pick(-1);
        if (k >= 0) { sfx(SFX_SLIDESHOW); open_viewer(k, 1); }
        else { sfx(SFX_ERROR); say("Nothing at this level in the Library"); }
      }
    } else {
      sfx(SFX_SLIDESHOW);
      open_viewer(cfg.shuffle ? (int)(rnd() % nshown) : 0, 1);
    }
  }
  if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_TAB); screen = S_SETTINGS; set_sel = 0; }
  draw_bar();
}
