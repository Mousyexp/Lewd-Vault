// Find duplicates (Settings, Library): every item's thumbnail made into a 64-bit fingerprint (a
// difference hash: 9 x 8 grey points, each row's neighbours compared), then the ones whose
// fingerprints all but match grouped, the same kind only. A group at a time: the sharpest one kept
// (its pixels), the others to the vault's trash with ✕, → to leave the group as it is. Part of main.c.

static uint64_t *dp_hash;        // (per item: its fingerprint; 0 none)
static int *dp_group, *dp_order; // (per item: its group's first member, or -1; the groups' items in order)
static volatile int dp_state;    // 0 not begun, 1 looking, 2 done
static volatile int dp_done, dp_total;
static int dp_gen;               // (the vault looked at: vgen)
static int dp_ngroups, *dp_gstart, *dp_glen, dp_at; // (the groups: where each begins in dp_order, how many)

static uint64_t dhash_of(const uint8_t *b, size_t n) {
  LvPixels px;
  if (lv_decode(b, n, 48, 48, &px) != 0 || !px.rgba || px.w < 2 || px.h < 2) return 0;
  float g[8][9];
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 9; x++) {
      // (each point the mean of its patch of the small picture)
      int x0 = x * px.w / 9, x1 = (x + 1) * px.w / 9, y0 = y * px.h / 8, y1 = (y + 1) * px.h / 8;
      if (x1 <= x0) x1 = x0 + 1;
      if (y1 <= y0) y1 = y0 + 1;
      float s = 0;
      int c = 0;
      for (int yy = y0; yy < y1 && yy < px.h; yy++)
        for (int xx = x0; xx < x1 && xx < px.w; xx++) { const uint8_t *p = px.rgba + ((size_t)yy * px.w + xx) * 4; s += p[0] * 0.3f + p[1] * 0.59f + p[2] * 0.11f; c++; }
      g[y][x] = c ? s / c : 0;
    }
  free(px.rgba);
  uint64_t h = 0;
  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) h = (h << 1) | (g[y][x] < g[y][x + 1]);
  return h ? h : 1; // (0 means none)
}
static void *dp_main(void *arg) {
  (void)arg;
  pin_to_core(1);
  int n = dp_total, my = vgen;
  for (int i = 0; i < n && my == vgen && worker_run; i++) {
    size_t bn = 0;
    pthread_mutex_lock(&vmx);
    uint8_t *b = loaded_vault && i < vault.n && vgen == my ? lv_thumb_bytes(&vault, &vault.items[i], &bn) : NULL;
    pthread_mutex_unlock(&vmx);
    dp_hash[i] = b ? dhash_of(b, bn) : 0;
    free(b);
    dp_done = i + 1;
  }
  dp_state = 2;
  return NULL;
}
static void dp_free(void) {
  free(dp_hash); free(dp_group); free(dp_order); free(dp_gstart); free(dp_glen);
  dp_hash = NULL; dp_group = dp_order = dp_gstart = dp_glen = NULL;
  dp_ngroups = 0; dp_at = 0; dp_state = 0;
}
static void dp_start(void) {
  if (dp_state == 1) return;
  dp_free();
  dp_total = vault.n;
  dp_hash = calloc(vault.n + 1, sizeof *dp_hash);
  if (!dp_hash) return;
  dp_done = 0; dp_state = 1; dp_gen = vgen;
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 256 * 1024);
  if (pthread_create(&t, &at, dp_main, NULL) == 0) pthread_detach(t);
  else dp_state = 0;
}
// the sharper of two: more pixels, else added first
static int dp_better(int a, int b) {
  long long pa = (long long)vault.items[a].width * vault.items[a].height, pb = (long long)vault.items[b].width * vault.items[b].height;
  if (pa != pb) return pa > pb;
  return vault.hot[a].added < vault.hot[b].added;
}
// The fingerprints into groups: within 5 points of each other, the same kind (once the looking's done)
static void dp_group_up(void) {
  int n = dp_total;
  dp_group = malloc((n + 1) * sizeof *dp_group);
  dp_order = malloc((n + 1) * sizeof *dp_order);
  dp_gstart = malloc((n / 2 + 2) * sizeof *dp_gstart);
  dp_glen = malloc((n / 2 + 2) * sizeof *dp_glen);
  if (!dp_group || !dp_order || !dp_gstart || !dp_glen) { dp_free(); return; }
  for (int i = 0; i < n; i++) dp_group[i] = -1;
  int no = 0;
  for (int i = 0; i < n; i++) {
    if (!dp_hash[i] || dp_group[i] >= 0) continue;
    int start = no, len = 0;
    for (int j = i + 1; j < n; j++) {
      if (!dp_hash[j] || dp_group[j] >= 0 || vault.hot[j].kind != vault.hot[i].kind) continue;
      if (__builtin_popcountll(dp_hash[i] ^ dp_hash[j]) > 5) continue;
      if (!len) { dp_order[no++] = i; dp_group[i] = i; len = 1; }
      dp_order[no++] = j; dp_group[j] = i; len++;
    }
    if (len < 2) continue;
    // (the one kept first)
    int best = start;
    for (int k = start + 1; k < start + len; k++) if (dp_better(dp_order[k], dp_order[best])) best = k;
    int t = dp_order[start]; dp_order[start] = dp_order[best]; dp_order[best] = t;
    dp_gstart[dp_ngroups] = start; dp_glen[dp_ngroups] = len; dp_ngroups++;
  }
  dp_at = 0;
}
static void draw_dupes(void) {
  vita2d_draw_rectangle(0, 0, W, H, BG);
  text(30, 44, TEXT, 26, "Find duplicates");
  if (dp_state == 0 || dp_gen != vgen) dp_start();
  if (dp_state == 1) {
    char m[96];
    snprintf(m, sizeof m, "Looking at each one: %d of %d", dp_done, dp_total);
    text(30, 80, DIM, 16, m);
    float f = dp_total ? (float)dp_done / dp_total : 0;
    rrect(30, 100, W - 60, 8, 4, SURF2);
    rrect(30, 100, (W - 60) * f, 8, 4, ACC);
    spinner(W / 2, H / 2, 20);
  } else if (dp_state == 2) {
    if (!dp_group) dp_group_up();
    if (!dp_ngroups || dp_at >= dp_ngroups) {
      text(30, 80, DIM, 16, dp_ngroups ? "That's every group." : "No duplicates found.");
    } else {
      int g = dp_at, st = dp_gstart[g], len = dp_glen[g];
      char m[96];
      snprintf(m, sizeof m, "Group %d of %d  ·  %d alike", g + 1, dp_ngroups, len);
      text(30, 80, DIM, 16, m);
      int want[8], nw = 0;
      int shown_n = len < 5 ? len : 5, cw = (W - 60 - (shown_n - 1) * 12) / shown_n, chh = 236;
      if (cw > 220) cw = 220;
      for (int k = 0; k < shown_n; k++) {
        int i = dp_order[st + k];
        float x = 30 + k * (cw + 12), y = 104;
        vita2d_texture *t = thumb_of(i);
        if (t) draw_cover(t, x, y, cw, chh, RGBA(255, 255, 255, 255));
        else { skeleton(x, y, cw, chh, 12); if (nw < 8) want[nw++] = i; }
        if (k == 0) { glass(x + 8, y + 8, text_w(13, "Kept") + 22, 26, 0); text(x + 19, y + 26, TEXT, 13, "Kept"); }
        else { rrect(x + 8, y + 8, text_w(13, "To the trash") + 22, 26, 9, RGBA(0, 0, 0, 170)); text(x + 19, y + 26, ACC2, 13, "To the trash"); }
        char d[64];
        snprintf(d, sizeof d, "%d × %d", vault.items[i].width, vault.items[i].height);
        text(x, y + chh + 20, DIM, 13, d);
        text_fit(x, y + chh + 38, FAINT, 12, vault.items[i].name && vault.items[i].name[0] ? vault.items[i].name : vault.items[i].file, cw);
      }
      if (nw) ask_thumbs(want, nw);
      if (len > shown_n) { char more[32]; snprintf(more, sizeof more, "and %d more", len - shown_n); text(W - 140, 80, FAINT, 13, more); }
      // ✕ keep the first, the rest to the trash; → leave this group
      int bx = 30, by = H - BAR - 54;
      int w1 = text_w(16, "Keep one, trash the rest") + 40;
      rrect(bx, by, w1, 44, 15, ACC);
      text(bx + 20, by + 28, RGBA(20, 12, 16, 255), 16, "Keep one, trash the rest");
      int w2 = text_w(16, "Leave them") + 40;
      rrect(bx + w1 + 12, by, w2, 44, 15, SURF2);
      text(bx + w1 + 32, by + 28, TEXT, 16, "Leave them");
      text(bx + w1 + w2 + 36, by + 28, FAINT, 13, "✕ trash  ·  → leave  ·  ○ back");
      if (hit(bx, by, w1, 44) || (in.pressed & SCE_CTRL_CROSS)) {
        saver_flush();
        pthread_mutex_lock(&vmx);
        // (from the last item down: the places of those before stay as they are)
        int del[64], nd = 0;
        for (int k = 1; k < len && nd < 64; k++) del[nd++] = dp_order[st + k];
        for (int a = 0; a < nd; a++) for (int b2 = a + 1; b2 < nd; b2++) if (del[b2] > del[a]) { int t = del[a]; del[a] = del[b2]; del[b2] = t; }
        for (int a = 0; a < nd; a++) {
          lv_vault_delete(&vault, del[a], 0);
          // (the fingerprints follow the items down, as the vault's own list does: nothing looked at again)
          memmove(dp_hash + del[a], dp_hash + del[a] + 1, (size_t)(dp_total - del[a] - 1) * sizeof *dp_hash);
          dp_total--;
        }
        int r = lv_vault_save_index(&vault);
        pthread_mutex_unlock(&vmx);
        sfx(r == 0 ? SFX_DELETE : SFX_ERROR);
        say(r == 0 ? "The rest went to the vault's trash" : "The index couldn't be written");
        library_shrank();
        // (grouped again from the fingerprints, on from the group after this one)
        int next = dp_at;
        free(dp_group); free(dp_order); free(dp_gstart); free(dp_glen);
        dp_group = dp_order = dp_gstart = dp_glen = NULL; dp_ngroups = 0;
        dp_group_up();
        dp_at = next;
        return;
      }
      if (hit(bx + w1 + 12, by, w2, 44) || (in.pressed & SCE_CTRL_RIGHT)) { sfx(SFX_TICK); dp_at++; }
    }
  }
  if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_BACK); screen = S_SETTINGS; }
  draw_bar();
}
