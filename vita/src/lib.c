// The Library's own tools, as the phone has them: filters (type, NSFW up to, stars, smart albums,
// collections, card size), saved filters, the tags panel, a fast-scroll handle with the month, quick
// actions on a card (hold ✕ or press and hold), selecting many, and editing in the full view's
// details (stars, the level dial, category, tags, delete). Edits are written to index.csv whole (after
// the day's backup), a moment after the last one. Compiled inside main.c (#include "lib.c").

static const char *KIND4[] = {"Everything", "Pictures", "GIFs", "Videos"};
static const char *ALBUMS[] = {"All", "Never opened", "Forgotten", "Long videos", "Most loved", "1 star", "Continue watching"};
#define NALBUMS 7
static int resume_of(const LvItem *it); // (show.c)
static const char *SIZES[] = {"Small", "Medium", "Large"};
static const int SIZE_H[] = {120, 168, 220};
static struct { int nsfw_max, stars_min, album, size; char coll[64]; } lf = {0, 0, 0, 1, ""};
static LvSearch lsearch;
static char forgotten_key[16]; // ("forgotten": last seen before this, yyyymmdd...)
static unsigned edits_at;       // (an edit waiting to be written: when the last one was)

static void edited(void) { edits_at = frame_no ? frame_no : 1; }
static void save_edits_now(void) {
  if (!vault.index_dirty) return;
  if (index_save_async() != 0) { sfx(SFX_ERROR); say("The changes couldn't be written to the card"); }
  edits_at = 0;
}

// ---------- which items show ----------
static int lib_pass(int i) {
  const LvHot *h = &vault.hot[i];
  if (cfg.kind == 1 && h->kind != LV_IMAGE) return 0;
  if (cfg.kind == 2 && h->kind != LV_GIF) return 0;
  if (cfg.kind == 3 && h->kind != LV_VIDEO) return 0;
  const LvItem *it = &vault.items[i];
  if (lf.nsfw_max && it->nsfw > lf.nsfw_max) return 0;
  if (lf.stars_min && it->quality < lf.stars_min) return 0;
  if (lf.coll[0] && strcmp(it->collection, lf.coll)) return 0;
  switch (lf.album) {
    case 1: if (lv_views_of(&vault, it) > 0) return 0; break;
    case 2: {
      if (lv_views_of(&vault, it) == 0 || it->view < 0) return 0;
      const char *last = vault.views[it->view].last;
      if (!last[0] || strncmp(last, forgotten_key, 10) >= 0) return 0;
      break;
    }
    case 3: if (h->kind != LV_VIDEO || it->duration < 300) return 0; break;
    case 4: if (lv_likes_of(&vault, it) < 2) return 0; break;
    case 5: if (it->quality != 1) return 0; break;
    case 6: if (resume_of(it) <= 0) return 0; break;
  }
  if (lsearch.n && !lv_search_match(&lsearch, it)) return 0;
  return 1;
}
static void lib_prepare(void) {
  lv_search_compile(query, &lsearch);
  time_t t = time(NULL) - 60 * 86400;
  struct tm g;
  gmtime_r(&t, &g);
  strftime(forgotten_key, sizeof forgotten_key, "%Y-%m-%d", &g);
  ROW_H = SIZE_H[lf.size < 0 || lf.size > 2 ? 1 : lf.size];
}

// "For you": what you come back to and like, the stars, a little of what's new, and some chance
static float *foryou;
static int cmp_foryou(const void *a, const void *b) {
  float x = foryou[*(const int *)a], y = foryou[*(const int *)b];
  return x < y ? 1 : x > y ? -1 : 0;
}
static void sort_foryou(void) {
  free(foryou);
  foryou = malloc((vault.n + 1) * sizeof *foryou);
  if (!foryou) return;
  for (int k = 0; k < nshown; k++) {
    int i = shown[k];
    const LvItem *it = &vault.items[i];
    float score = lv_likes_of(&vault, it) * 3.0f + logf(1 + lv_views_of(&vault, it)) * 2.0f + it->quality * 1.5f;
    score += (float)(vault.hot[i].added / 1000000 % 100000) * 0.00002f; // (a little for what's newer)
    score += (rnd() % 1000) / 250.0f;                                  // (and some chance)
    foryou[i] = score;
  }
  qsort(shown, nshown, sizeof *shown, cmp_foryou);
}

// ---------- saved filters (ux0:data/LWDV00001/filters.txt) ----------
typedef struct { char name[40], query[128], coll[64]; int kind, nsfw, stars, album; } Saved;
static Saved saved[16];
static int nsaved_f, saved_loaded;
static void saved_load(void) {
  saved_loaded = 1;
  FILE *f = fopen(DATA "/filters.txt", "r");
  if (!f) return;
  char line[400];
  while (nsaved_f < 16 && fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    Saved s;
    memset(&s, 0, sizeof s);
    char *p[8] = {0};
    int n = 0;
    for (char *t = line; n < 8; ) { p[n++] = t; char *tab = strchr(t, '\t'); if (!tab) break; *tab = 0; t = tab + 1; }
    if (n < 7) continue;
    snprintf(s.name, sizeof s.name, "%.39s", p[0]);
    snprintf(s.query, sizeof s.query, "%s", p[1]);
    s.kind = atoi(p[2]); s.nsfw = atoi(p[3]); s.stars = atoi(p[4]); s.album = atoi(p[5]); if (s.album < 0 || s.album >= NALBUMS) s.album = 0;
    snprintf(s.coll, sizeof s.coll, "%s", p[6]);
    saved[nsaved_f++] = s;
  }
  fclose(f);
}
static void saved_write(void) {
  FILE *f = fopen(DATA "/filters.txt.part", "w");
  if (!f) return;
  for (int k = 0; k < nsaved_f; k++)
    fprintf(f, "%s\t%s\t%d\t%d\t%d\t%d\t%s\n", saved[k].name, saved[k].query, saved[k].kind, saved[k].nsfw, saved[k].stars, saved[k].album, saved[k].coll);
  fclose(f);
  sceIoRemove(DATA "/filters.txt");
  sceIoRename(DATA "/filters.txt.part", DATA "/filters.txt");
}
static void saved_add_current(const char *name) {
  if (nsaved_f >= 16) { sfx(SFX_ERROR); say("Sixteen saved filters at most"); return; }
  Saved s;
  memset(&s, 0, sizeof s);
  snprintf(s.name, sizeof s.name, "%.39s", name);
  snprintf(s.query, sizeof s.query, "%s", query);
  snprintf(s.coll, sizeof s.coll, "%s", lf.coll);
  s.kind = cfg.kind; s.nsfw = lf.nsfw_max; s.stars = lf.stars_min; s.album = lf.album;
  for (char *c = s.name; *c; c++) if (*c == '\t') *c = ' ';
  saved[nsaved_f++] = s;
  saved_write();
  sfx(SFX_SAVED);
  say("Filters saved");
}
static void saved_apply(int k) {
  Saved *s = &saved[k];
  snprintf(query, sizeof query, "%s", s->query);
  snprintf(lf.coll, sizeof lf.coll, "%s", s->coll);
  cfg.kind = s->kind; lf.nsfw_max = s->nsfw; lf.stars_min = s->stars; lf.album = s->album;
}

// ---------- the sheets ----------
enum { LS_NONE, LS_FILTERS, LS_TAGS, LS_QUICK, LS_BATCH };
static int lsheet, lsheet_sel, quick_item = -1, delete_armed;
static unsigned delete_armed_at;
static unsigned char *picked; // per item: chosen (selecting many)
static int npicked, selecting;

static void lib_after_change(void) { refilter(); if (sel >= nshown) sel = nshown ? nshown - 1 : 0; }

static void sheet_frame(int x, int y, int w, int h) {
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 150));
  rrect(x, y, w, h, 16, SURF);
}
static int sheet_row(int k, int x, int y, int w, int rh, const char *name, const char *value) {
  if (k == lsheet_sel) rrect(x + 6, y, w - 12, rh - 4, 10, SURF2);
  text(x + 20, y + rh / 2 + 6, TEXT, 17, name);
  if (value && value[0]) text(x + w - 22 - text_w(16, value), y + rh / 2 + 6, ACC, 16, value);
  if (hit(x, y, w, rh)) { lsheet_sel = k; in.pressed |= SCE_CTRL_CROSS; return 1; }
  return 0;
}
static int sheet_nav(int n) {
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) lsheet_sel = (lsheet_sel + 1) % n;
  if (in.pressed & SCE_CTRL_UP) lsheet_sel = (lsheet_sel + n - 1) % n;
  return (in.pressed & SCE_CTRL_RIGHT) ? 1 : (in.pressed & SCE_CTRL_LEFT) ? -1 : (in.pressed & SCE_CTRL_CROSS) ? 1 : 0;
}
static void sheet_close(void) { sfx(SFX_CLOSE); lsheet = LS_NONE; save_edits_now(); }

// the vault's collections, for cycling through
static int coll_next(int d) {
  // (a small list made each time: collections are few)
  const char *names[64];
  int n = 0;
  for (int i = 0; i < vault.n && n < 64; i++) {
    const char *c = vault.items[i].collection;
    if (!c[0]) continue;
    int seen = 0;
    for (int k = 0; k < n; k++) if (!strcmp(names[k], c)) { seen = 1; break; }
    if (!seen) names[n++] = c;
  }
  int cur = -1;
  for (int k = 0; k < n; k++) if (!strcmp(names[k], lf.coll)) cur = k;
  int nx = cur + d;
  if (nx < -1) nx = n - 1;
  if (nx >= n) nx = -1;
  if (nx < 0) lf.coll[0] = 0; else snprintf(lf.coll, sizeof lf.coll, "%s", names[nx]);
  return n;
}

static void draw_filters_sheet(void) {
  if (!saved_loaded) saved_load();
  enum { F_SEARCH, F_KIND, F_NSFW, F_STARS, F_ALBUM, F_COLL, F_SIZE, F_TAGS, F_SAVED, F_SAVE, F_RESET, F_N };
  int w = 460, x = W - w - 12, y = 12, rh = 46;
  sheet_frame(x, y, w, F_N * rh + 16);
  char v[160];
  for (int k = 0; k < F_N; k++) {
    int ry = y + 8 + k * rh;
    switch (k) {
      case F_SEARCH: sheet_row(k, x, ry, w, rh, "Search", query[0] ? query : "tags, -not, ~any, artist:*"); break;
      case F_KIND: sheet_row(k, x, ry, w, rh, "Show", KIND4[cfg.kind & 3]); break;
      case F_NSFW: if (lf.nsfw_max) snprintf(v, sizeof v, "up to %d", lf.nsfw_max); else snprintf(v, sizeof v, "Any"); sheet_row(k, x, ry, w, rh, "NSFW level", v); break;
      case F_STARS: snprintf(v, sizeof v, "%s", lf.stars_min == 0 ? "Any" : lf.stars_min == 1 ? "★ and up" : lf.stars_min == 2 ? "★★ and up" : "★★★"); sheet_row(k, x, ry, w, rh, "Stars", v); break;
      case F_ALBUM: sheet_row(k, x, ry, w, rh, "Album", ALBUMS[lf.album]); break;
      case F_COLL: sheet_row(k, x, ry, w, rh, "Collection", lf.coll[0] ? lf.coll : "All"); break;
      case F_SIZE: sheet_row(k, x, ry, w, rh, "Card size", SIZES[lf.size]); break;
      case F_TAGS: sheet_row(k, x, ry, w, rh, "Tags of these", "›"); break;
      case F_SAVED: snprintf(v, sizeof v, "%s", nsaved_f ? "‹ choose ›" : "none yet"); sheet_row(k, x, ry, w, rh, "Saved filters", v); break;
      case F_SAVE: sheet_row(k, x, ry, w, rh, "Save these filters…", ""); break;
      case F_RESET: sheet_row(k, x, ry, w, rh, "Clear all filters", ""); break;
    }
  }
  int d = sheet_nav(F_N);
  static int saved_k = -1;
  if (d) {
    int changed = 1;
    switch (lsheet_sel) {
      case F_SEARCH: changed = 0; if ((in.pressed & SCE_CTRL_CROSS) && !ime_open) ime_start(2, "Search: tags, -not, ~any, artist:*", query, 0); break;
      case F_KIND: cfg.kind = (cfg.kind + d + 4) % 4; break;
      case F_NSFW: lf.nsfw_max = (lf.nsfw_max + d + 11) % 11; break;
      case F_STARS: lf.stars_min = (lf.stars_min + d + 4) % 4; break;
      case F_ALBUM: lf.album = (lf.album + d + NALBUMS) % NALBUMS; break;
      case F_COLL: if (!coll_next(d)) { changed = 0; sfx(SFX_ERROR); say("No collections in this vault"); } break;
      case F_SIZE: lf.size = (lf.size + d + 3) % 3; break;
      case F_TAGS: changed = 0; if (in.pressed & SCE_CTRL_CROSS) { sfx(SFX_OPEN); lsheet = LS_TAGS; lsheet_sel = 0; } break;
      case F_SAVED:
        if (!nsaved_f) { changed = 0; break; }
        saved_k = (saved_k + d + nsaved_f + 1) % (nsaved_f + 1) - 0;
        if (saved_k >= nsaved_f) saved_k = 0;
        saved_apply(saved_k);
        { char m[80]; snprintf(m, sizeof m, "%s", saved[saved_k].name); say(m); }
        break;
      case F_SAVE: changed = 0; if ((in.pressed & SCE_CTRL_CROSS) && !ime_open) ime_start(5, "A name for these filters", "", 0); break;
      case F_RESET: cfg.kind = 0; lf.nsfw_max = lf.stars_min = lf.album = 0; lf.coll[0] = 0; query[0] = 0; cat = -1; break;
    }
    if (changed) { sfx(SFX_TOGGLE); lib_after_change(); scroll = 0; sel = 0; cfg_save(); }
  }
  // (□ removes the saved filter shown)
  if (lsheet_sel == F_SAVED && (in.pressed & SCE_CTRL_SQUARE) && saved_k >= 0 && saved_k < nsaved_f) {
    memmove(&saved[saved_k], &saved[saved_k + 1], (nsaved_f - saved_k - 1) * sizeof *saved);
    nsaved_f--;
    saved_k = -1;
    saved_write();
    sfx(SFX_DELETE);
    say("Saved filter removed");
    in.pressed &= ~SCE_CTRL_SQUARE;
  }
  if ((in.pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_SQUARE)) || (in.tap && in.tap_x < x)) sheet_close();
  in.pressed = 0; in.tap = 0;
}

// the tags of what's shown, most common first: ✕ adds one to the search, □ leaves it out
typedef struct { const char *t; int len, count; } TagCount;
static TagCount tcounts[48];
static int ntcounts;
static void count_tags(void) {
  enum { CAP = 4096 };
  TagCount *tab = calloc(CAP, sizeof *tab);
  ntcounts = 0;
  if (!tab) return;
  int lim = nshown < 6000 ? nshown : 6000;
  for (int k = 0; k < lim; k++) {
    const char *p = vault.items[shown[k]].tags;
    while (*p) {
      while (*p == ' ') p++;
      const char *e = p;
      while (*e && *e != ' ') e++;
      int len = (int)(e - p);
      if (len > 0) {
        unsigned long hsh = 5381;
        for (int i = 0; i < len; i++) hsh = hsh * 33 ^ (unsigned char)p[i];
        unsigned at = hsh & (CAP - 1);
        int probes = 0;
        while (tab[at].t && !(tab[at].len == len && !memcmp(tab[at].t, p, len)) && probes++ < CAP) at = (at + 1) & (CAP - 1);
        if (!tab[at].t) { tab[at].t = p; tab[at].len = len; }
        tab[at].count++;
      }
      p = e;
    }
  }
  for (int k = 0; k < CAP; k++) {
    if (!tab[k].t) continue;
    int at = ntcounts < 48 ? ntcounts++ : -1;
    if (at < 0) { int low = 0; for (int j = 1; j < 48; j++) if (tcounts[j].count < tcounts[low].count) low = j; if (tab[k].count <= tcounts[low].count) continue; at = low; }
    tcounts[at] = tab[k];
  }
  for (int a = 0; a < ntcounts; a++)
    for (int b = a + 1; b < ntcounts; b++)
      if (tcounts[b].count > tcounts[a].count) { TagCount t = tcounts[a]; tcounts[a] = tcounts[b]; tcounts[b] = t; }
  free(tab);
}
static void draw_tags_sheet(void) {
  static unsigned counted_at;
  if (counted_at + 1 < frame_no || !ntcounts) count_tags();
  counted_at = frame_no;
  sheet_frame(12, 12, W - 24, H - 24);
  text(32, 44, TEXT, 20, "Tags of what's shown");
  text(W - 32 - text_w(13, "✕ add   □ leave out   ○ back"), 44, FAINT, 13, "✕ add   □ leave out   ○ back");
  float x = 32, y = 62;
  for (int k = 0; k < ntcounts; k++) {
    char t[96];
    snprintf(t, sizeof t, "%.*s  %d", tcounts[k].len > 60 ? 60 : tcounts[k].len, tcounts[k].t, tcounts[k].count);
    int w = text_w(14, t) + 24;
    if (x + w > W - 32) { x = 32; y += 34; }
    if (y > H - 50) break;
    rrect(x, y, w, 28, 14, k == lsheet_sel ? ACC : SURF2);
    text(x + 12, y + 19, k == lsheet_sel ? RGBA(20, 12, 16, 255) : TEXT, 14, t);
    if (hit((int)x, (int)y, w, 28)) { lsheet_sel = k; in.pressed |= SCE_CTRL_CROSS; }
    x += w + 6;
  }
  if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_DOWN)) { sfx(SFX_TICK); lsheet_sel = (lsheet_sel + 1) % (ntcounts ? ntcounts : 1); }
  if (in.pressed & (SCE_CTRL_LEFT | SCE_CTRL_UP)) { sfx(SFX_TICK); lsheet_sel = (lsheet_sel + (ntcounts ? ntcounts : 1) - 1) % (ntcounts ? ntcounts : 1); }
  if ((in.pressed & (SCE_CTRL_CROSS | SCE_CTRL_SQUARE)) && lsheet_sel < ntcounts) {
    char add[100];
    snprintf(add, sizeof add, "%s%.*s", (in.pressed & SCE_CTRL_SQUARE) ? "-" : "", tcounts[lsheet_sel].len > 90 ? 90 : tcounts[lsheet_sel].len, tcounts[lsheet_sel].t);
    size_t ql = strlen(query);
    if (ql + strlen(add) + 2 < sizeof query) { if (ql) strcat(query, " "); strcat(query, add); }
    sfx(SFX_TOGGLE);
    lib_after_change();
    sel = 0; scroll = 0;
    lsheet = LS_NONE;
  } else if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_CLOSE); lsheet = LS_FILTERS; lsheet_sel = 7; }
  in.pressed = 0; in.tap = 0;
}

// ---------- editing one item (quick actions, the full view's details) ----------
static void set_int(int i, const char *col, int v) {
  char b[16];
  snprintf(b, sizeof b, "%d", v);
  if (lv_item_set(&vault, i, col, b) == 0) edited();
}
static void set_category(int i, int d) {
  if (vault.ncats < 1) return;
  int c = vault.hot[i].cat;
  for (int tries = 0; tries < vault.ncats; tries++) {
    c = (c + d + vault.ncats) % vault.ncats;
    if (vault.cat_counts[c] > 0 || c == vault.hot[i].cat) break;
  }
  if (c != vault.hot[i].cat && lv_item_set(&vault, i, "category", vault.cats[c]) == 0) edited();
}
// the delete: the first press arms it (3 s), the second does it
static int delete_confirmed(void) {
  if (delete_armed && frame_no - delete_armed_at < 180) { delete_armed = 0; return 1; }
  delete_armed = 1;
  delete_armed_at = frame_no;
  sfx(SFX_ERROR);
  say("Press again to move it to the vault's trash");
  return 0;
}
// After items left the vault: everything that was by position starts again (the thumbnails reload).
static void library_shrank(void) {
  pthread_mutex_lock(&mx);
  vgen++;
  nwant = 0;
  big_ask.item = -1;
  for (int i = 0; i <= vault.n; i++) { tstate[i] = 0; slot_of[i] = -1; }
  for (int k = 0; k < SLOTS; k++) { slots[k].tex = NULL; slots[k].item = -1; slots[k].used = 0; }
  pthread_mutex_unlock(&mx);
  if (picked) memset(picked, 0, vault.n + 1);
  npicked = 0;
  lib_after_change();
}
static int delete_item(int i) {
  saver_flush(); // (a queued write mustn't land after this one)
  pthread_mutex_lock(&vmx);
  int r = lv_vault_delete(&vault, i, 1);
  pthread_mutex_unlock(&vmx);
  if (r != 0) { sfx(SFX_ERROR); say("It couldn't be deleted"); return -1; }
  sfx(SFX_DELETE);
  library_shrank();
  return 0;
}

// the level dial: a half circle; touch along it, or ←/→
static void level_dial(int i, float cx, float cy, float r, int focus) {
  int lv = vault.items[i].nsfw;
  for (int k = 1; k <= 10; k++) {
    float a = 3.14159f * (1 - (k - 1) / 9.0f);
    float x = cx + cosf(a) * r, y = cy - sinf(a) * r;
    circle(x, y, k == lv ? 9 : 6, k <= lv ? ACC : with_alpha(TEXT, 50));
  }
  char b[8];
  snprintf(b, sizeof b, "%d", lv);
  text_center((int)cx, (int)cy + 2, focus ? ACC : TEXT, 26, b);
  text_center((int)cx, (int)cy + 20, FAINT, 12, "NSFW level");
  if (in.touching && hypotf(in.tx - cx, in.ty - cy) < r + 26 && in.ty < cy + 10) {
    float a = atan2f(cy - in.ty, in.tx - cx);
    int k = 1 + (int)lroundf((1 - a / 3.14159f) * 9);
    if (k < 1) k = 1;
    if (k > 10) k = 10;
    if (k != lv) { sfx(SFX_TICK); set_int(i, "nsfw", k); }
  }
}
static void stars(int i, float x, float y, int focus) {
  int q = vault.items[i].quality;
  for (int k = 1; k <= 3; k++) {
    text((int)x + (k - 1) * 30, (int)y, k <= q ? ACC : with_alpha(TEXT, focus ? 120 : 60), 26, "★");
    if (hit((int)x + (k - 1) * 30 - 4, (int)y - 26, 30, 34)) { sfx(SFX_TOGGLE); set_int(i, "quality", k); }
  }
}

// quick actions on one card (hold ✕, or press and hold it)
static void draw_quick_sheet(void) {
  int i = quick_item;
  if (i < 0 || i >= vault.n) { lsheet = LS_NONE; return; }
  enum { Q_STARS, Q_LEVEL, Q_CAT, Q_SELECT, Q_DELETE, Q_N };
  int w = 420, h = 300, x = W / 2 - w / 2, y = H / 2 - h / 2;
  sheet_frame(x, y, w, h);
  text_fit(x + 20, y + 34, TEXT, 18, vault.items[i].name[0] ? vault.items[i].name : vault.items[i].file, w - 40);
  if (lsheet_sel == Q_STARS) rrect(x + 10, y + 46, 150, 40, 10, SURF2);
  stars(i, x + 24, y + 78, lsheet_sel == Q_STARS);
  if (lsheet_sel == Q_LEVEL) rrect(x + w - 190, y + 46, 180, 100, 10, SURF2);
  level_dial(i, x + w - 100, y + 126, 66, lsheet_sel == Q_LEVEL);
  char c[96];
  snprintf(c, sizeof c, "‹  %s  ›", vault.items[i].category);
  if (lsheet_sel == Q_CAT) rrect(x + 10, y + 154, w - 20, 36, 10, SURF2);
  text(x + 24, y + 178, TEXT, 16, "Category");
  text(x + w - 24 - text_w(16, c), y + 178, ACC, 16, c);
  if (hit(x, y + 154, w / 2, 36)) set_category(i, -1);
  if (hit(x + w / 2, y + 154, w / 2, 36)) set_category(i, 1);
  rrect(x + 20, y + 206, w / 2 - 30, 40, 20, lsheet_sel == Q_SELECT ? ACC : SURF2);
  text_center(x + 20 + (w / 2 - 30) / 2, y + 232, lsheet_sel == Q_SELECT ? RGBA(20, 12, 16, 255) : TEXT, 16, "Select");
  rrect(x + w / 2 + 10, y + 206, w / 2 - 30, 40, 20, lsheet_sel == Q_DELETE ? RGBA(220, 70, 80, 255) : SURF2);
  text_center(x + w / 2 + 10 + (w / 2 - 30) / 2, y + 232, TEXT, 16, delete_armed ? "Press again" : "Delete");
  text_center(W / 2, y + h - 18, FAINT, 12, "↑↓ choose  ←→ change  ○ done");
  int act = 0;
  if (hit(x + 20, y + 206, w / 2 - 30, 40)) { lsheet_sel = Q_SELECT; act = 1; }
  if (hit(x + w / 2 + 10, y + 206, w / 2 - 30, 40)) { lsheet_sel = Q_DELETE; act = 1; }
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) lsheet_sel = (lsheet_sel + 1) % Q_N;
  if (in.pressed & SCE_CTRL_UP) lsheet_sel = (lsheet_sel + Q_N - 1) % Q_N;
  int d = (in.pressed & SCE_CTRL_RIGHT) ? 1 : (in.pressed & SCE_CTRL_LEFT) ? -1 : 0;
  if (d && lsheet_sel == Q_STARS) { sfx(SFX_TOGGLE); int q = vault.items[i].quality + d; set_int(i, "quality", q < 1 ? 1 : q > 3 ? 3 : q); }
  if (d && lsheet_sel == Q_LEVEL) { sfx(SFX_TICK); int l = vault.items[i].nsfw + d; set_int(i, "nsfw", l < 1 ? 1 : l > 10 ? 10 : l); }
  if (d && lsheet_sel == Q_CAT) { sfx(SFX_TOGGLE); set_category(i, d); }
  if ((in.pressed & SCE_CTRL_CROSS) || act) {
    if (lsheet_sel == Q_SELECT) {
      if (!picked) picked = calloc(vault.n + 64, 1);
      if (picked) { selecting = 1; picked[i] = 1; npicked = 1; sfx(SFX_SELECT); }
      lsheet = LS_NONE;
      save_edits_now();
    } else if (lsheet_sel == Q_DELETE && delete_confirmed()) {
      lsheet = LS_NONE;
      save_edits_now();
      delete_item(i);
    }
  }
  if ((in.pressed & SCE_CTRL_CIRCLE) || (in.tap && !(in.tap_x >= x && in.tap_x < x + w && in.tap_y >= y && in.tap_y < y + h))) {
    sheet_close();
    if (vault.index_dirty) lib_after_change();
  }
  in.pressed = 0; in.tap = 0;
}

// what to do with the ones chosen
static void draw_batch_sheet(void) {
  enum { B_STARS, B_LEVEL, B_CAT, B_TAG, B_DELETE, B_N };
  static int stars_v = 2, level_v = 5, cat_v = 0;
  int w = 440, x = W / 2 - w / 2, y = 70, rh = 44;
  sheet_frame(x, y, w, B_N * rh + 60);
  char t[80];
  snprintf(t, sizeof t, "%d chosen", npicked);
  text(x + 20, y + 32, TEXT, 18, t);
  char v[96];
  for (int k = 0; k < B_N; k++) {
    int ry = y + 46 + k * rh;
    switch (k) {
      case B_STARS: snprintf(v, sizeof v, "‹ %s ›  ✕ set", stars_v == 1 ? "★" : stars_v == 2 ? "★★" : "★★★"); sheet_row(k, x, ry, w, rh, "Stars", v); break;
      case B_LEVEL: snprintf(v, sizeof v, "‹ %d ›  ✕ set", level_v); sheet_row(k, x, ry, w, rh, "NSFW level", v); break;
      case B_CAT: snprintf(v, sizeof v, "‹ %s ›  ✕ move", vault.ncats ? vault.cats[cat_v % vault.ncats] : "-"); sheet_row(k, x, ry, w, rh, "Category", v); break;
      case B_TAG: sheet_row(k, x, ry, w, rh, "Add a tag…", ""); break;
      case B_DELETE: sheet_row(k, x, ry, w, rh, delete_armed ? "Press again to delete them" : "Delete them", ""); break;
    }
  }
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) lsheet_sel = (lsheet_sel + 1) % B_N;
  if (in.pressed & SCE_CTRL_UP) lsheet_sel = (lsheet_sel + B_N - 1) % B_N;
  int d = (in.pressed & SCE_CTRL_RIGHT) ? 1 : (in.pressed & SCE_CTRL_LEFT) ? -1 : 0;
  if (d) {
    sfx(SFX_TICK);
    if (lsheet_sel == B_STARS) stars_v = (stars_v - 1 + d + 3) % 3 + 1;
    if (lsheet_sel == B_LEVEL) level_v = (level_v - 1 + d + 10) % 10 + 1;
    if (lsheet_sel == B_CAT && vault.ncats) cat_v = (cat_v + d + vault.ncats) % vault.ncats;
  }
  if (in.pressed & SCE_CTRL_CROSS) {
    if (lsheet_sel == B_TAG) { if (!ime_open) ime_start(6, "A tag for all of them", "", 0); }
    else if (lsheet_sel == B_DELETE) {
      if (delete_confirmed()) {
        saver_flush(); // (a queued write mustn't land after this one)
        pthread_mutex_lock(&vmx);
        for (int i = vault.n - 1; i >= 0; i--) if (picked && picked[i]) lv_vault_delete(&vault, i, 0);
        int r = lv_vault_save_index(&vault);
        pthread_mutex_unlock(&vmx);
        sfx(r == 0 ? SFX_DELETE : SFX_ERROR);
        say(r == 0 ? "Moved to the vault's trash" : "The index couldn't be written");
        selecting = 0;
        lsheet = LS_NONE;
        library_shrank();
      }
    } else {
      for (int i = 0; i < vault.n; i++) {
        if (!picked || !picked[i]) continue;
        if (lsheet_sel == B_STARS) set_int(i, "quality", stars_v);
        if (lsheet_sel == B_LEVEL) set_int(i, "nsfw", level_v);
        if (lsheet_sel == B_CAT && vault.ncats) { if (lv_item_set(&vault, i, "category", vault.cats[cat_v % vault.ncats]) == 0) edited(); }
      }
      save_edits_now();
      sfx(SFX_DONE);
      say("Done");
      lib_after_change();
    }
  }
  if (in.pressed & SCE_CTRL_CIRCLE) sheet_close();
  in.pressed = 0; in.tap = 0;
}
// the tag typed for all the chosen ones
static void batch_add_tag(const char *tag) {
  char clean[96];
  int n = 0;
  for (const char *p = tag; *p && n < 94; p++) clean[n++] = *p == ' ' ? '_' : *p;
  clean[n] = 0;
  if (!n) return;
  for (int i = 0; i < vault.n; i++) {
    if (!picked || !picked[i]) continue;
    const char *t = vault.items[i].tags;
    LvSearch one;
    lv_search_compile(clean, &one);
    if (lv_search_match(&one, &vault.items[i]) && strstr(t, clean)) continue; // (has it)
    char *nt = malloc(strlen(t) + n + 2);
    if (!nt) continue;
    sprintf(nt, "%s%s%s", t, t[0] ? " " : "", clean);
    if (lv_item_set(&vault, i, "tags", nt) == 0) edited();
    free(nt);
  }
  save_edits_now();
  sfx(SFX_DONE);
  say("Tag added");
}

// ---------- in the grid: holding, selecting, the fast-scroll handle ----------
static unsigned cross_down_at;
// Returns 1 when ✕ was a short press (open), handling a long hold itself (quick actions).
static int lib_cross(void) {
  if (in.pressed & SCE_CTRL_CROSS) { cross_down_at = frame_no; in.pressed &= ~SCE_CTRL_CROSS; }
  int down = (in.now & SCE_CTRL_CROSS) != 0;
  if (cross_down_at && down && frame_no - cross_down_at >= 27 && nshown) {
    cross_down_at = 0;
    if (!selecting) { sfx(SFX_OPEN); quick_item = shown[sel]; lsheet = LS_QUICK; lsheet_sel = 0; delete_armed = 0; }
    return 0;
  }
  if (cross_down_at && !down) { cross_down_at = 0; return 1; }
  return 0;
}
static int lib_long_press_card(int k) {
  // a finger held still on a card for half a second
  if (in.touching && !in.moved && frame_no - in.down_frame == 30 && k >= 0 && k < nshown) {
    if (selecting) { picked[shown[k]] ^= 1; npicked += picked[shown[k]] ? 1 : -1; sfx(SFX_TOGGLE); return 1; }
    sfx(SFX_OPEN);
    quick_item = shown[k];
    lsheet = LS_QUICK;
    lsheet_sel = 0;
    delete_armed = 0;
    in.moved = 1; // (no tap when it's let go)
    return 1;
  }
  return 0;
}
static void month_of(const char *added, char *out, size_t cap) {
  static const char *M[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int y = atoi(added), m = strlen(added) >= 7 ? atoi(added + 5) : 0;
  if (m < 1 || m > 12) { snprintf(out, cap, "%s", added); return; }
  snprintf(out, cap, "%s %d", M[m - 1], y);
}
// dragging along the right edge flings through everything; a bubble says which month
static int fast_scroll(float max_scroll, float view_h) {
  if (!(in.touching && in.down_x > W - 36 && in.down_y > TOP && in.down_y < H - BAR) || max_scroll <= 0) return 0;
  float f = (in.ty - TOP) / view_h;
  f = f < 0 ? 0 : f > 1 ? 1 : f;
  scroll = f * max_scroll;
  scroll_v = 0;
  int k = grid_first(&lgrid, scroll);
  if (k < nshown) {
    char m[32];
    month_of(vault.items[shown[k]].added, m, sizeof m);
    int w = text_w(16, m) + 28;
    rrect(W - 50 - w, in.ty - 16, w, 32, 16, ACC);
    text(W - 50 - w + 14, in.ty + 6, RGBA(20, 12, 16, 255), 16, m);
  }
  rrect(W - 10, TOP + f * (view_h - 40), 6, 40, 3, ACC);
  return 1;
}
