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
  size_t cap = 16 + (size_t)nresume * 120;
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
static void show_save(void) {
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
static int is_locked(int i) { return vault.items[i].nsfw > unlocked_level(); }
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

static void draw_show_plan(void) {
  text(30, 44, TEXT, 26, "Slideshow");
  int levels = so.preset != PRE_SIMPLE;
  if (levels) make_stages();
  char status[160];
  if (ss.on && ss.nst) {
    int k = stage_now();
    double left = (k + 1) * level_ms() - ss.ms;
    if (k < ss.nst - 1) snprintf(status, sizeof status, "Session: level %d reached · the next in %d:%02d", ss.st_hi[k], (int)(left / 60000), (int)(left / 1000) % 60);
    else snprintf(status, sizeof status, "Session: the last level, %d", ss.st_hi[k]);
  } else if (levels) {
    snprintf(status, sizeof status, "%d %s · %d min · %.1f s a slide, down to %.1f s", ss.nst, ss.nst == 1 ? "stage" : "stages",
             (int)(ss.nst * so.minutes + 0.5f), so.s0, so.s1);
  } else {
    snprintf(status, sizeof status, "Everything in the Library, %d s each%s", cfg.interval, cfg.shuffle ? ", shuffled" : "");
  }
  text(30 + text_w(26, "Slideshow") + 16, 44, ACC, 15, status);

  // the levels, as the PC's stepper: passed ones full, the one now filling, the rest dim
  if (levels) {
    int k = ss.on ? stage_now() : -1;
    float p = ss.on ? (float)((ss.ms - k * level_ms()) / level_ms()) : 0;
    if (p > 1) p = 1;
    for (int l = 1; l <= 10; l++) {
      int x = 30 + (l - 1) * 90, y = 62;
      int in_plan = 0;
      for (int s = 0; s < ss.nst; s++) if (l >= ss.st_lo[s] && l <= ss.st_hi[s]) in_plan = 1;
      int passed = ss.on && l < ss.st_lo[k], now = ss.on && l >= ss.st_lo[k] && l <= ss.st_hi[k];
      rrect(x, y, 84, 26, 13, in_plan ? SURF2 : with_alpha(SURF2, 90));
      if (passed) rrect(x, y, 84, 26, 13, with_alpha(ACC, 150));
      if (now) rrect(x, y, 84 * p < 26 ? 26 : 84 * p, 26, 13, ACC);
      char b[8];
      snprintf(b, sizeof b, "%d", l);
      text_center(x + 42, y + 19, in_plan ? TEXT : FAINT, 15, b);
    }
  }

  char v[12][48];
  snprintf(v[0], 48, "%s", PRESETS[so.preset]);
  int n = 0;
  struct { const char *name, *value; int id; } rows[12];
  enum { R_PRESET, R_FROM, R_FIRST, R_TO, R_MIN, R_PACE, R_MIX, R_INTERVAL, R_SHUFFLE, R_MOTION, R_FADE, R_SLEEP };
  rows[n++] = (typeof(rows[0])){"Kind", v[0], R_PRESET};
  if (levels) {
    snprintf(v[1], 48, "%d to %d", so.lo, so.hi);
    rows[n++] = (typeof(rows[0])){"Levels", v[1], R_FROM};
    snprintf(v[2], 48, "up to %d", so.first);
    rows[n++] = (typeof(rows[0])){"The first stage takes levels", v[2], R_FIRST};
    snprintf(v[3], 48, "%g min", so.minutes);
    rows[n++] = (typeof(rows[0])){"Each level for", v[3], R_MIN};
    snprintf(v[4], 48, "%g s → %g s", so.s0, so.s1);
    rows[n++] = (typeof(rows[0])){"Each slide for", v[4], R_PACE};
    rows[n++] = (typeof(rows[0])){"At each level, show", so.below ? "It and everything below" : "Only that level", R_MIX};
  } else {
    snprintf(v[5], 48, "%d s", cfg.interval);
    rows[n++] = (typeof(rows[0])){"Each picture for", v[5], R_INTERVAL};
    rows[n++] = (typeof(rows[0])){"Shuffle", cfg.shuffle ? "On" : "Off", R_SHUFFLE};
  }
  rows[n++] = (typeof(rows[0])){"Motion", MOTIONS[so.motion], R_MOTION};
  rows[n++] = (typeof(rows[0])){"Fade from one to the next", so.fade ? "On" : "Off", R_FADE};
  if (so.sleep) snprintf(v[6], 48, "after %d min", SLEEPS[so.sleep]); else snprintf(v[6], 48, "Off");
  rows[n++] = (typeof(rows[0])){"Sleep timer", v[6], R_SLEEP};
  if (plan_sel >= n + 1) plan_sel = n;
  int top = levels ? 98 : 66, rh = levels ? 36 : 44;
  for (int k = 0; k < n; k++) {
    int y = top + k * rh;
    rrect(30, y, W - 60, rh - 4, 10, k == plan_sel ? SURF2 : SURF);
    if (k == plan_sel) rrect(30, y, 5, rh - 4, 3, ACC);
    text(50, y + rh / 2 + 3, TEXT, 16, rows[k].name);
    text(W - 50 - text_w(16, rows[k].value) - 22, y + rh / 2 + 3, ACC, 16, rows[k].value);
    text(W - 62, y + rh / 2 + 3, FAINT, 16, "‹›");
    if (hit(30, y, W - 60, rh - 4)) { plan_sel = k; in.pressed |= SCE_CTRL_RIGHT; }
  }
  // the buttons: start (or carry on), and end the session
  int by = top + n * rh + 6;
  const char *go = ss.on && levels ? "Carry on" : "Start";
  rrect(30, by, 200, 40, 20, plan_sel == n ? ACC : with_alpha(ACC, 120));
  text_center(130, by + 27, RGBA(20, 12, 16, 255), 17, go);
  int start = hit(30, by, 200, 40);
  if (ss.on) {
    rrect(244, by, 200, 40, 20, SURF2);
    text_center(344, by + 27, TEXT, 16, "End the session");
    if (hit(244, by, 200, 40)) { sfx(SFX_CLOSE); session_end("Session ended"); }
    text(462, by + 26, FAINT, 13, "□ ends it  ·  levels above it stay locked till then");
  }

  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) plan_sel = (plan_sel + 1) % (n + 1);
  if (in.pressed & SCE_CTRL_UP) plan_sel = (plan_sel + n) % (n + 1);
  int d = (in.pressed & SCE_CTRL_RIGHT) ? 1 : (in.pressed & SCE_CTRL_LEFT) ? -1 : 0;
  if ((in.pressed & SCE_CTRL_CROSS) && plan_sel < n) d = 1;
  if (d && plan_sel < n) {
    sfx(SFX_TOGGLE);
    switch (rows[plan_sel].id) {
      case R_PRESET: if (!ss.on) preset((so.preset + d + 4) % 4); else say("End the session to change its kind"); break;
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
  if (plan_sel == R_FROM && levels) text(30, H - BAR - 12, FAINT, 13, "←→ the first level  ·  hold R for the last one");
  else if (plan_sel == 4 && levels) text(30, H - BAR - 12, FAINT, 13, "←→ the first slides  ·  hold R for the last ones");
  if ((in.pressed & SCE_CTRL_SQUARE) && ss.on) { sfx(SFX_CLOSE); session_end("Session ended"); }
  if (((in.pressed & SCE_CTRL_CROSS) && plan_sel == n) || (in.pressed & SCE_CTRL_START) || start) {
    if (!nshown) { sfx(SFX_ERROR); say("Nothing in the Library to show"); }
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
