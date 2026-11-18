// Clean up, as the phone's: one picture at a time, swipe it away (or ←) to let it go, keep it (→),
// undo (△); nothing is deleted until the end, when the ones let go go into the vault's _trash at once.
// And the health check: what's wrong in the vault and what can be put right. Compiled inside main.c.

static int *cl_order, cl_n, cl_k;   // the items to look at (never opened first, then the least seen)
static unsigned char *cl_gone;      // per item: let go
static int cl_ngone, cl_kept;
static int cl_hist[64], cl_nhist;   // (for undo)
static float cl_swipe, cl_swipe_y;
static int cl_confirm;
// (the card on its way off the screen after a decision: which way, how far along, which item)
static float cl_fly_x, cl_fly_y, cl_fly_t;
static int cl_fly_item = -1;

static float *cl_score;
static int cmp_clean(const void *a, const void *b) {
  float x = cl_score[*(const int *)a], y = cl_score[*(const int *)b];
  return x < y ? -1 : x > y;
}
static void clean_open(void) {
  free(cl_order); free(cl_gone); free(cl_score);
  cl_order = malloc((nshown + 1) * sizeof *cl_order);
  cl_gone = calloc(vault.n + 1, 1);
  cl_score = malloc((vault.n + 1) * sizeof *cl_score);
  cl_n = cl_k = cl_ngone = cl_kept = cl_nhist = cl_confirm = 0;
  cl_swipe = 0;
  if (!cl_order || !cl_gone || !cl_score) { say("Not enough memory"); return; }
  // (from the Library as it's filtered now: the ones looked at least, and rated lowest, first)
  for (int k = 0; k < nshown; k++) {
    int i = shown[k];
    if (is_locked(i)) continue;
    const LvItem *it = &vault.items[i];
    cl_score[i] = lv_views_of(&vault, it) * 2.0f + lv_likes_of(&vault, it) * 6.0f + it->quality * 3.0f + (rnd() % 100) / 100.0f;
    cl_order[cl_n++] = i;
  }
  qsort(cl_order, cl_n, sizeof *cl_order, cmp_clean);
  screen = S_CLEAN;
  if (cl_n) ask_big(cl_order[0]);
}
// gone: 1 let go, 0 keep, 2 keep and heart it (it sorts to Most loved)
static void clean_next(int gone) {
  int i = cl_order[cl_k];
  cl_fly_item = i; cl_fly_t = 0;
  cl_fly_x = gone == 1 ? -1 : gone == 0 ? 1 : 0; cl_fly_y = gone == 2 ? -1 : 0;
  if (gone == 1) { cl_gone[i] = 1; cl_ngone++; sfx(SFX_DELETE); }
  else if (gone == 2) { char now[40]; now_iso(now, sizeof now); lv_liked(&vault, &vault.items[i], now); cl_kept++; sfx(SFX_DONE); }
  else { cl_kept++; sfx(SFX_TICK); }
  cl_swipe_y = 0;
  if (cl_nhist < 64) cl_hist[cl_nhist++] = cl_k; else { memmove(cl_hist, cl_hist + 1, 63 * sizeof *cl_hist); cl_hist[63] = cl_k; }
  cl_k++;
  cl_swipe = 0;
  if (cl_k < cl_n) ask_big(cl_order[cl_k]);
}
// the ones let go, into _trash (from the end, so the numbers of the rest don't move under it)
static void clean_finish(void) {
  int n = 0;
  saver_flush(); // (a queued write mustn't land after this one)
  pthread_mutex_lock(&vmx);
  for (int i = vault.n - 1; i >= 0; i--) if (cl_gone[i]) { if (lv_vault_delete(&vault, i, 0) == 0) n++; }
  int r = n ? lv_vault_save_index(&vault) : 0;
  pthread_mutex_unlock(&vmx);
  char m[96];
  snprintf(m, sizeof m, r == 0 ? "%d moved to the vault's _trash" : "The index couldn't be written", n);
  say(m);
  sfx(r == 0 ? SFX_DONE : SFX_ERROR);
  if (n) library_shrank();
  screen = S_LIBRARY;
}
static void draw_clean(void) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 255));
  if (cl_confirm) {
    char m[120];
    snprintf(m, sizeof m, "Let %d go? They go into the vault's _trash (the PC can restore them).", cl_ngone);
    text_center(W / 2, H / 2 - 30, TEXT, 20, m);
    rrect(W / 2 - 230, H / 2, 220, 48, 24, RGBA(220, 70, 80, 255));
    text_center(W / 2 - 120, H / 2 + 31, TEXT, 18, "✕ Let them go");
    rrect(W / 2 + 10, H / 2, 220, 48, 24, SURF2);
    text_center(W / 2 + 120, H / 2 + 31, TEXT, 18, "○ Keep them all");
    if ((in.pressed & SCE_CTRL_CROSS) || hit(W / 2 - 230, H / 2, 220, 48)) { clean_finish(); return; }
    if ((in.pressed & SCE_CTRL_CIRCLE) || hit(W / 2 + 10, H / 2, 220, 48)) { cl_confirm = 0; screen = S_LIBRARY; say("Nothing deleted"); }
    return;
  }
  if (cl_k >= cl_n) {
    text_center(W / 2, H / 2 - 20, TEXT, 22, cl_n ? "That's all of them" : "Nothing to look at here");
    char m[96];
    snprintf(m, sizeof m, "%d kept, %d to let go  ·  ✕ done  ·  △ undo", cl_kept, cl_ngone);
    text_center(W / 2, H / 2 + 14, DIM, 16, m);
    if (in.pressed & SCE_CTRL_CROSS) { if (cl_ngone) cl_confirm = 1; else screen = S_LIBRARY; }
  } else {
    int i = cl_order[cl_k];
    vita2d_texture *t = big_item == i ? big_tex : thumb_of(i);
    if (!t) { int w[1] = {i}; ask_thumbs(w, 1); }
    // the next one, waiting under it (its thumbnail asked for now, so it's there when it comes up)
    if (cl_k + 1 < cl_n) {
      int j = cl_order[cl_k + 1];
      vita2d_texture *tn = thumb_of(j);
      if (!tn) { int w[1] = {j}; ask_thumbs(w, 1); }
      else { float tw = vita2d_texture_get_width(tn), th = vita2d_texture_get_height(tn), s = fminf((W - 220) / tw, (H - 160) / th); vita2d_draw_texture_tint_scale(tn, W / 2 - tw * s / 2, H / 2 - 10 - th * s / 2, s, s, RGBA(255, 255, 255, 70)); }
    }
    // the card follows the finger; let go of it far enough, or flicked, and it's decided (the
    // distance is read before it springs back)
    int decided = -1;
    if (in.touching) { cl_swipe += in.drag_dx; cl_swipe_y += in.drag_dy; }
    else {
      float vx = in.flicked ? in.fvx : 0, vy = in.flicked ? in.fvy : 0;
      if (cl_swipe < -140 || vx < -14) decided = 1;
      else if (cl_swipe > 140 || vx > 14) decided = 0;
      else if (cl_swipe_y < -120 || vy < -14) decided = 2;
      cl_swipe *= decay(0.7f); cl_swipe_y *= decay(0.7f);
    }
    float f = cl_swipe / 300.0f;
    if (t) {
      float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
      float s = fminf((W - 160) / tw, (H - 130) / th);
      vita2d_draw_texture_scale_rotate(t, W / 2 + cl_swipe, H / 2 - 20 + fminf(0, cl_swipe_y) + fabsf(cl_swipe) * 0.1f, s, s, f * 0.25f);
    } else spinner(W / 2 + cl_swipe, H / 2, 22);
    if (cl_swipe < -20) { int a = (int)fminf(220, -cl_swipe * 1.4f); text_center(W / 2 - 300, H / 2, RGBA(240, 80, 90, a), 30, "LET GO"); }
    if (cl_swipe > 20) { int a = (int)fminf(220, cl_swipe * 1.4f); text_center(W / 2 + 300, H / 2, RGBA(90, 220, 140, a), 30, "KEEP"); }
    if (cl_swipe_y < -20 && fabsf(cl_swipe) < 60) { int a = (int)fminf(220, -cl_swipe_y * 1.6f); heart(W / 2, 70, 16, with_alpha(ACC, a)); }
    // three big round buttons along the foot, for a tap instead of a swipe
    {
      static const int bx[3] = {W / 2 - 150, W / 2, W / 2 + 150};
      static const char *bl[3] = {"Let go", "Heart", "Keep"};
      unsigned bc[3] = {RGBA(220, 70, 80, 255), ACC, RGBA(70, 190, 120, 255)};
      for (int b = 0; b < 3; b++) {
        circle(bx[b], H - 52, 30, with_alpha(SURF2, 230));
        circle(bx[b], H - 52, 27, with_alpha(bc[b], 60));
        if (b == 0) icon("xmark", bx[b], H - 52, bc[b]);
        else if (b == 1) heart(bx[b], H - 52, 9, bc[b]);
        else icon("chev_r", bx[b], H - 52, bc[b]);
        text_center(bx[b], H - 8, FAINT, 12, bl[b]);
        if (hit(bx[b] - 34, H - 86, 68, 68)) decided = b == 0 ? 1 : b == 1 ? 2 : 0;
      }
    }
    const LvItem *it = &vault.items[i];
    char m[200];
    snprintf(m, sizeof m, "%d of %d  ·  seen %lld  ·  %d★  ·  %s", cl_k + 1, cl_n, lv_views_of(&vault, it), it->quality, it->category);
    text_center(W / 2, 30, DIM, 15, m);
    text(16, H - 12, FAINT, 12, "\u2190 let go  \u2192 keep  \u2191 heart  \u25b3 undo  \u25cb finish");
    if (decided >= 0) { clean_next(decided); return; }
    if (in.pressed & SCE_CTRL_LEFT) { clean_next(1); return; }
    if (in.pressed & SCE_CTRL_RIGHT) { clean_next(0); return; }
    if (in.pressed & SCE_CTRL_UP) { clean_next(2); return; }
  }
  // the one just decided, flying off the way it went
  if (cl_fly_item >= 0 && cl_fly_t < 1) {
    vita2d_texture *ft = thumb_of(cl_fly_item);
    cl_fly_t += 0.12f;
    if (ft) {
      float tw = vita2d_texture_get_width(ft), th = vita2d_texture_get_height(ft), s = fminf((W - 160) / tw, (H - 130) / th);
      float e = cl_fly_t * cl_fly_t;
      vita2d_draw_texture_tint_scale_rotate(ft, W / 2 + cl_fly_x * e * 900, H / 2 - 20 + cl_fly_y * e * 600, s, s, cl_fly_x * e * 0.5f, RGBA(255, 255, 255, (int)(255 * (1 - cl_fly_t))));
    }
  }
  if ((in.pressed & SCE_CTRL_TRIANGLE) && cl_nhist) {
    cl_k = cl_hist[--cl_nhist];
    int i = cl_order[cl_k];
    if (cl_gone[i]) { cl_gone[i] = 0; cl_ngone--; } else cl_kept--;
    sfx(SFX_BACK);
    ask_big(i);
  }
  if (in.pressed & SCE_CTRL_CIRCLE) { if (cl_ngone) cl_confirm = 1; else { sfx(SFX_BACK); screen = S_LIBRARY; } }
}

// ---------- the health check ----------
static struct { int done, missing, stale_views, dup_ids, orphans, no_size; } hc;
static int cmp_strp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static const char *base_of(const char *p) { const char *s = p ? strrchr(p, '/') : NULL; return s ? s + 1 : p; }
static void health_scan(void) {
  memset(&hc, 0, sizeof hc);
  // (the ids and the files' names, sorted once: a duplicate is next to its twin, and a file in Media/
  //  is looked up by halving, not compared with every item)
  const char **ids = malloc((vault.n + 1) * sizeof *ids), **files = malloc((vault.n + 1) * sizeof *files);
  int nfiles = 0;
  for (int i = 0; i < vault.n; i++) {
    const LvItem *it = &vault.items[i];
    if (!it->path) hc.missing++;
    if (!it->width && vault.hot[i].kind == LV_IMAGE) hc.no_size++;
    if (ids) ids[i] = it->id;
    if (files && it->path) files[nfiles++] = base_of(it->path);
  }
  if (ids) {
    qsort(ids, vault.n, sizeof *ids, cmp_strp);
    for (int i = 1; i < vault.n; i++) if (!strcmp(ids[i], ids[i - 1])) hc.dup_ids++;
  }
  if (files) qsort(files, nfiles, sizeof *files, cmp_strp);
  unsigned char *used = calloc(vault.nviews + 1, 1);
  for (int i = 0; used && i < vault.n; i++) if (vault.items[i].view >= 0 && vault.items[i].view < vault.nviews) used[vault.items[i].view] = 1;
  for (int k = 0; used && k < vault.nviews; k++) if (!used[k]) hc.stale_views++;
  free(used);
  // files in Media/ that no row has
  char dir[700];
  snprintf(dir, sizeof dir, "%s/Media", vault.root);
  DIR *d = opendir(dir);
  struct dirent *e;
  while (d && (e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    const char *nm = e->d_name;
    int used = files && bsearch(&nm, files, nfiles, sizeof *files, cmp_strp) != NULL;
    if (!files) for (int i = 0; i < vault.n && !used; i++) { const char *b = base_of(vault.items[i].path); if (b && !strcmp(b, nm)) used = 1; }
    if (!used) hc.orphans++;
  }
  if (d) closedir(d);
  free(ids); free(files);
  hc.done = 1;
}
static int hc_sel;
static void draw_health(void) {
  text(30, 44, TEXT, 26, "Health check");
  if (!hc.done) {
    // (one frame of the spinner first: the look round can take a moment on a big vault)
    static unsigned shown_at;
    if (!shown_at || frame_no - shown_at > 2) shown_at = frame_no;
    spinner(W / 2, H / 2, 22);
    text_center(W / 2, H / 2 + 50, DIM, 16, "Looking through the vault\u2026");
    if (frame_no - shown_at >= 1) { health_scan(); shown_at = 0; }
    return;
  }
  struct { const char *what; int n; const char *fix; } rows[] = {
    {"Items whose file isn't on the card", hc.missing, "✕ removes their rows (their thumbnails go with them)"},
    {"Counts for items that are gone (views.csv)", hc.stale_views, "✕ drops them"},
    {"Items listed twice", hc.dup_ids, "The PC's health check fixes these"},
    {"Files in Media no item points to", hc.orphans, "Left as they are: the PC's health check can add them back"},
    {"Pictures without their size", hc.no_size, "Harmless: shown at the size they turn out to be"},
  };
  int all_ok = 1;
  for (int k = 0; k < 5; k++) {
    int y = 70 + k * 72;
    if (rows[k].n) all_ok = 0;
    rrect(30, y, W - 60, 64, 14, k == hc_sel ? SURF2 : SURF);
    if (k == hc_sel) rrect(30, y, 5, 64, 3, ACC);
    char b[16];
    snprintf(b, sizeof b, "%d", rows[k].n);
    text(52, y + 28, TEXT, 17, rows[k].what);
    text(52, y + 52, FAINT, 14, rows[k].n ? rows[k].fix : "All good");
    text(W - 60 - text_w(22, b), y + 40, rows[k].n ? ACC2 : RGBA(90, 220, 140, 255), 22, b);
    if (hit(30, y, W - 60, 64)) { hc_sel = k; in.pressed |= SCE_CTRL_CROSS; }
  }
  if (all_ok) text(30, H - BAR - 14, RGBA(90, 220, 140, 255), 16, "Everything's in order.");
  if (in.pressed & SCE_CTRL_DOWN) hc_sel = (hc_sel + 1) % 5;
  if (in.pressed & SCE_CTRL_UP) hc_sel = (hc_sel + 4) % 5;
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_CROSS) {
    if (hc_sel == 0 && hc.missing) {
      saver_flush(); // (a queued write mustn't land after this one)
      pthread_mutex_lock(&vmx);
      for (int i = vault.n - 1; i >= 0; i--) if (!vault.items[i].path) lv_vault_delete(&vault, i, 0);
      int r = lv_vault_save_index(&vault);
      pthread_mutex_unlock(&vmx);
      sfx(r == 0 ? SFX_DONE : SFX_ERROR);
      library_shrank();
      hc.done = 0;
    } else if (hc_sel == 1 && hc.stale_views) {
      // (views.csv written again from the rows still in use)
      saver_flush(); // (a queued write mustn't land after this one)
      pthread_mutex_lock(&vmx);
      int w = 0;
      int *to = malloc((vault.nviews + 1) * sizeof *to);
      for (int k = 0; to && k < vault.nviews; k++) to[k] = -1;
      for (int i = 0; to && i < vault.n; i++) { int v = vault.items[i].view; if (v >= 0 && v < vault.nviews) to[v] = 1; }
      for (int k = 0; to && k < vault.nviews; k++) if (to[k] == 1) { vault.views[w] = vault.views[k]; to[k] = w++; } // (the strings move with their rows)
      for (int i = 0; to && i < vault.n; i++) { int v = vault.items[i].view; if (v >= 0 && v < vault.nviews) { vault.items[i].view = to[v]; vault.hot[i].view = to[v]; } }
      if (to) vault.nviews = w;
      free(to);
      vault.views_dirty = 1;
      int r = lv_vault_save_views(&vault);
      pthread_mutex_unlock(&vmx);
      sfx(r == 0 ? SFX_DONE : SFX_ERROR);
      hc.done = 0;
    } else sfx(SFX_ERROR);
  }
  if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_BACK); hc.done = 0; screen = S_SETTINGS; }
  draw_bar();
}
