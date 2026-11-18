// Favourites: long videos and comics kept as links, not files (ux0:data/LWDV00001/favorites.tsv, a
// line each: kind, site, title, page, preview, extra, length). A favourite plays as it downloads, or
// reads page by page; "Make available offline" puts it in the vault like any save. The Favourites
// screen has two tabs, Videos and Comics. Compiled inside main.c, after manga_ui.c.

enum { FAV_VIDEO, FAV_COMIC };
typedef struct {
  int kind;
  char site[48], title[200], page[600], preview[600], extra[200];
  int length; // (a video's seconds; a comic's pages)
} Fav;
#define FAV_MAX 300
static Fav *favs;
static int nfavs, favs_loaded;
static unsigned favs_ver; // (one more each time they change: what's worked out from them is kept till then)

static void fav_clean(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }
static void favs_load(void) {
  favs_loaded = 1;
  favs_ver++;
  if (!favs) favs = calloc(FAV_MAX, sizeof *favs);
  if (!favs) return;
  nfavs = 0;
  FILE *f = fopen(DATA "/favorites.tsv", "r");
  static char ln[2400];
  while (f && nfavs < FAV_MAX && fgets(ln, sizeof ln, f)) {
    ln[strcspn(ln, "\r\n")] = 0;
    char *p[7] = {0};
    int n = 0;
    for (char *t = ln; n < 7; ) { p[n++] = t; char *tab = strchr(t, '\t'); if (!tab) break; *tab = 0; t = tab + 1; }
    if (n < 7) continue;
    Fav *v = &favs[nfavs];
    memset(v, 0, sizeof *v);
    v->kind = atoi(p[0]) == FAV_COMIC ? FAV_COMIC : FAV_VIDEO;
    snprintf(v->site, sizeof v->site, "%s", p[1]);
    snprintf(v->title, sizeof v->title, "%s", p[2]);
    snprintf(v->page, sizeof v->page, "%s", p[3]);
    snprintf(v->preview, sizeof v->preview, "%s", p[4]);
    snprintf(v->extra, sizeof v->extra, "%s", p[5]);
    v->length = atoi(p[6]);
    // (checked once, here: one AI-made by its title, kept from before that was checked, is left out;
    //  new ones come from posts already checked)
    if (v->page[0] && !lv_ai_made(v->title)) nfavs++;
  }
  if (f) fclose(f);
}
static void favs_save(void) {
  favs_ver++;
  FILE *f = fopen(DATA "/favorites.tsv.part", "w");
  if (!f) return;
  for (int k = 0; k < nfavs; k++) {
    Fav *v = &favs[k];
    fav_clean(v->title); fav_clean(v->site); fav_clean(v->page); fav_clean(v->preview); fav_clean(v->extra);
    fprintf(f, "%d\t%s\t%s\t%s\t%s\t%s\t%d\n", v->kind, v->site, v->title, v->page, v->preview, v->extra, v->length);
  }
  fclose(f);
  sceIoRemove(DATA "/favorites.tsv");
  sceIoRename(DATA "/favorites.tsv.part", DATA "/favorites.tsv");
}
static int fav_find(int kind, const char *page) {
  if (!favs_loaded) favs_load();
  // (the full view asks for the same post's page several times a frame: the last answer kept until
  //  the favourites change or another page is asked about)
  static char m_page[600]; static int m_kind = -1, m_ans; static unsigned m_ver = ~0u;
  if (kind == m_kind && m_ver == favs_ver && !strcmp(page, m_page)) return m_ans; // (one compare, not one per favourite)
  snprintf(m_page, sizeof m_page, "%s", page); m_kind = kind; m_ver = favs_ver;
  for (int k = 0; k < nfavs; k++) if (favs[k].kind == kind && !strcmp(favs[k].page, page)) return m_ans = k;
  return m_ans = -1;
}
static void fav_remove(int k) {
  if (k < 0 || k >= nfavs) return;
  memmove(&favs[k], &favs[k + 1], (nfavs - k - 1) * sizeof *favs);
  nfavs--; favs_ver++;
  favs_save();
}
// (newest first)
static Fav *fav_add(void) {
  if (!favs_loaded) favs_load();
  if (!favs || nfavs >= FAV_MAX) { sfx(SFX_ERROR); say("Three hundred favourites at most"); return NULL; }
  memmove(&favs[1], &favs[0], nfavs * sizeof *favs);
  nfavs++; favs_ver++;
  memset(&favs[0], 0, sizeof *favs);
  return &favs[0];
}
// A video of the Browser's: in or out of the favourites. 1 now in.
static int fav_toggle_video(const LvPost *p, int site) {
  int k = fav_find(FAV_VIDEO, p->page);
  if (k >= 0) { fav_remove(k); sfx(SFX_CLOSE); say("Taken out of Favourites"); return 0; }
  Fav *v = fav_add();
  if (!v) return 0;
  v->kind = FAV_VIDEO;
  snprintf(v->site, sizeof v->site, "%s", LV_SITES[site].name);
  // (a title: the site's words for it, else its tags)
  snprintf(v->title, sizeof v->title, "%.190s", p->tags);
  snprintf(v->page, sizeof v->page, "%s", p->page);
  snprintf(v->preview, sizeof v->preview, "%s", p->preview);
  // (the file itself, where it isn't found on the page each time: a booru's)
  snprintf(v->extra, sizeof v->extra, "%s", strcmp(p->file, p->page) && strlen(p->file) < sizeof v->extra ? p->file : "");
  v->length = p->duration;
  favs_save();
  sfx(SFX_DONE);
  say("In Favourites: Library › Videos › ♥");
  return 1;
}
static int fav_toggle_comic(const LvBook *b, int src) {
  int k = fav_find(FAV_COMIC, b->id);
  if (k >= 0) { fav_remove(k); sfx(SFX_CLOSE); say("Taken out of Favourites"); return 0; }
  Fav *v = fav_add();
  if (!v) return 0;
  v->kind = FAV_COMIC;
  snprintf(v->site, sizeof v->site, "%s", LV_MSRC[src >= 0 && src < MS_N ? src : 0].name);
  snprintf(v->title, sizeof v->title, "%s", b->title);
  snprintf(v->page, sizeof v->page, "%s", b->id);
  snprintf(v->preview, sizeof v->preview, "%s", b->cover);
  snprintf(v->extra, sizeof v->extra, "%s", b->token);
  v->length = b->pages;
  favs_save();
  sfx(SFX_DONE);
  say("In Favourites: Library › Comics › ♥");
  return 1;
}

// ---------- the screen ----------
static int fav_tab, fav_sel, fav_back = S_LIBRARY;
static float fav_scroll;
// previews: a small thread fetching each card's, decoded to the card's size
#define FAV_CW 216
#define FAV_CH 122
#define FAV_BW 140
#define FAV_BH 200
typedef struct { char url[600]; int w, h, slot, gen; } FavJob;
static FavJob fav_jobs[24];
static int nfav_jobs, fav_thread_on;
static volatile int fav_gen = 1;
static struct { int slot, gen; LvPixels px; } fav_done[8];
static int nfav_done;
static pthread_mutex_t fvmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fvcv = PTHREAD_COND_INITIALIZER;
static vita2d_texture *fav_tex[FAV_MAX];
static signed char fav_ts[FAV_MAX]; // 0, 1 asked, 2 here, 3 failed
static void *fav_main(void *arg) {
  (void)arg;
  pin_to_core(2);
  LvHttp *h = lv_http_new();
  while (worker_run) {
    pthread_mutex_lock(&fvmx);
    while (worker_run && !nfav_jobs) wait_work(&fvcv, &fvmx, h);
    if (!worker_run) { pthread_mutex_unlock(&fvmx); break; }
    FavJob j = fav_jobs[--nfav_jobs];
    pthread_mutex_unlock(&fvmx);
    if (j.gen != fav_gen) continue;
    uint8_t *b = NULL;
    size_t n = 0;
    LvPixels px = {0};
    if (h && lv_http_get(h, j.url, &b, &n, 4u << 20, NULL, NULL, NULL) == 200 && b) {
      if (decode_exact(b, n, j.w, j.h, &px) != 0) px.rgba = NULL;
    }
    free(b);
    pthread_mutex_lock(&fvmx);
    if (nfav_done < 8 && j.gen == fav_gen) { fav_done[nfav_done].slot = j.slot; fav_done[nfav_done].gen = j.gen; fav_done[nfav_done].px = px; nfav_done++; }
    else free(px.rgba);
    pthread_mutex_unlock(&fvmx);
  }
  lv_http_free(h);
  return NULL;
}
static void fav_drop_textures(void) {
  pthread_mutex_lock(&fvmx);
  fav_gen++;
  nfav_jobs = 0;
  for (int k = 0; k < nfav_done; k++) free(fav_done[k].px.rgba);
  nfav_done = 0;
  pthread_mutex_unlock(&fvmx);
  for (int k = 0; k < FAV_MAX; k++) { if (fav_tex[k]) release_tex(fav_tex[k]); fav_tex[k] = NULL; fav_ts[k] = 0; }
}
static void fav_ask(int k, int w, int h) {
  if (k < 0 || k >= nfavs || fav_ts[k] || !favs[k].preview[0]) return;
  pthread_mutex_lock(&fvmx);
  if (nfav_jobs < 24) {
    FavJob *j = &fav_jobs[nfav_jobs++];
    snprintf(j->url, sizeof j->url, "%s", favs[k].preview);
    j->w = w; j->h = h; j->slot = k; j->gen = fav_gen;
    fav_ts[k] = 1;
    pthread_cond_signal(&fvcv);
  }
  pthread_mutex_unlock(&fvmx);
}
// (Favourites and the video browser left: the previews waiting dropped)
static void fav_drop_wants(void) {
  pthread_mutex_lock(&fvmx);
  for (int k = 0; k < nfav_jobs; k++) { int sl = fav_jobs[k].slot; if (sl >= 0 && sl < FAV_MAX && fav_ts[sl] == 1) fav_ts[sl] = 0; }
  nfav_jobs = 0;
  pthread_mutex_unlock(&fvmx);
}
static void fav_take(void) {
  if (!nfav_done) return; // (nothing come: no lock)
  pthread_mutex_lock(&fvmx);
  int n = nfav_done < 3 ? nfav_done : 3; // (three a frame: the scroll never waits)
  typeof(fav_done[0]) got[8];
  memcpy(got, fav_done, n * sizeof *got);
  memmove(fav_done, fav_done + n, (nfav_done - n) * sizeof *fav_done);
  nfav_done -= n;
  pthread_mutex_unlock(&fvmx);
  for (int k = 0; k < n; k++) {
    int s = got[k].slot;
    if (got[k].gen == fav_gen && s >= 0 && s < FAV_MAX && got[k].px.rgba) { if (fav_tex[s]) release_tex(fav_tex[s]); fav_tex[s] = pool_put(&pool_favs, &got[k].px); fav_ts[s] = fav_tex[s] ? 2 : 3; }
    else if (got[k].gen == fav_gen && s >= 0 && s < FAV_MAX) fav_ts[s] = 3;
    free(got[k].px.rgba);
  }
}
static float bar_busy(void) {
  if (msave_on && msave_total > 0) return fminf(1, msave_done / (float)msave_total);
  if (conv_on) return 2;
  if (saving) return big_progress > 0 ? fminf(1, big_progress) : 2;
  return -1;
}
static void fav_thread_start(void) {
  if (!fav_thread_on) {
    lv_net_warm(); // (on a thread of its own; the rest on the worker that fetches)
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512 * 1024); // (it decodes previews)
    fav_thread_on = pthread_create(&t, &at, fav_main, NULL) == 0;
    if (fav_thread_on) pthread_detach(t);
  }
}
static int fav_list(int tab, int *out);
static void fav_play(int at, int offline);
// ---- the video browser's "Up next" row: the favourite videos (vbrowse.c) ----
// (the list made again only when the favourites change, not each frame)
static int upnext_list[FAV_MAX], nupnext;
static unsigned upnext_ver = ~0u;
static int upnext_n(void) {
  if (!favs_loaded) favs_load();
  if (upnext_ver != favs_ver) { upnext_ver = favs_ver; nupnext = fav_list(0, upnext_list); }
  return nupnext;
}
static int upnext_at(int j) { int m = upnext_n(); return j >= 0 && j < m ? upnext_list[j] : -1; }
static vita2d_texture *upnext_tex(int j, int w, int h) {
  int k = upnext_at(j);
  if (k < 0) return NULL;
  fav_thread_start();
  if (!fav_tex[k]) fav_ask(k, w, h);
  return fav_tex[k];
}
static int upnext_state(int j) { int k = upnext_at(j); return k < 0 ? 3 : fav_ts[k]; }
static const char *upnext_title(int j) { int k = upnext_at(j); return k < 0 ? "" : favs[k].title; }
static int upnext_len(int j) { int k = upnext_at(j); return k < 0 ? 0 : favs[k].length; }
static void upnext_play(int j) { int k = upnext_at(j); if (k < 0) return; fav_play(k, 0); if (screen == S_WEBVIEW) wv_back = S_VIDEOS; }
static void upnext_take(void) { fav_take(); }
static void favs_open(int tab) {
  if (!favs_loaded) favs_load();
  fav_drop_textures();
  fav_tab = tab; fav_sel = 0; fav_scroll = 0;
  fav_back = screen == S_FAVS ? fav_back : screen;
  if (!fav_thread_on) {
    lv_net_warm(); // (on a thread of its own; the rest on the worker that fetches)
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512 * 1024); // (it decodes previews)
    fav_thread_on = pthread_create(&t, &at, fav_main, NULL) == 0;
    if (fav_thread_on) pthread_detach(t);
  }
  screen = S_FAVS;
}
// (the favourites of the tab, in order: their places in favs)
static int fav_list(int tab, int *out) {
  int m = 0;
  for (int k = 0; k < nfavs; k++) if (favs[k].kind == (tab ? FAV_COMIC : FAV_VIDEO)) out[m++] = k;
  return m;
}
static int site_named(const char *name) {
  for (int s = 0; s < LV_NSITES; s++) if (!strcmp(LV_SITES[s].name, name)) return s;
  return -1;
}
// A video favourite: every video favourite becomes the Browser's list (swipe for the next), and the
// full view opens on this one, played as it downloads.
static void fav_play(int at, int offline) {
  int list[FAV_MAX], m = fav_list(0, list);
  if (!m) return;
  web_start();
  pthread_mutex_lock(&wmx);
  wgen++;
  for (int i = 0; i < nwdone; i++) { free(wdone[i].px.rgba); free(wdone[i].a); free(wdone[i].b); }
  nwdone = 0;
  nwwant = 0;
  if (page_ready.ready) { lv_posts_free(page_ready.posts, page_ready.count); page_ready.ready = 0; page_ready.posts = NULL; }
  lv_posts_free(posts, nposts);
  posts = calloc(m + 1, sizeof *posts);
  signed char *ns = realloc(wstate, m + 1);
  short *nso = realloc(wslot_of, (m + 1) * sizeof *nso);
  if (ns) wstate = ns;
  if (nso) wslot_of = nso;
  nposts = 0;
  int open_at = 0;
  for (int i = 0; posts && ns && nso && i < m; i++) {
    const Fav *v = &favs[list[i]];
    LvPost *p = &posts[nposts];
    int site = site_named(v->site);
    p->id = 1000000 + list[i];
    p->preview = strdup(v->preview); p->sample = strdup(v->preview);
    p->file = strdup(v->extra[0] ? v->extra : v->page);
    p->page = strdup(v->page); p->source = strdup(""); p->tags = strdup(v->title);
    snprintf(p->ext, sizeof p->ext, "mp4");
    p->width = 1280; p->height = 720; p->rating = LV_R_EXPLICIT; p->duration = v->length;
    p->site1 = site >= 0 ? site + 1 : 0;
    if (!p->preview || !p->sample || !p->file || !p->page || !p->source || !p->tags) { free(p->preview); free(p->sample); free(p->file); free(p->page); free(p->source); free(p->tags); memset(p, 0, sizeof *p); continue; }
    if (list[i] == at) open_at = nposts;
    wstate[nposts] = 0; wslot_of[nposts] = -1;
    nposts++;
  }
  grid_build(&wgrid, nposts, post_aspect);
  wend = 1; werr[0] = 0; wloading = 0; list_want = 0;
  big_cancel = 1;
  pthread_mutex_unlock(&wmx);
  web_clear_textures();
  if (!nposts) return;
  wsel = open_at;
  wdetails = 0; wdetails_y = 0; wchrome_until = frame_no + 180;
  wv_back = S_FAVS;
  screen = S_WEBVIEW;
  fav_offline_next = offline ? open_at + 1 : 0;
  wv_show(open_at);
  if (offline) say("Downloading it to make it available offline… (it plays meanwhile)");
}
// A comic favourite: its book opens (the list holds it alone), as from the Comics tab.
static void fav_read(int at, int offline) {
  const Fav *v = &favs[at];
  int src = MS_MANGADEX;
  for (int k = 0; k < MS_N; k++) if (!strcmp(v->site, LV_MSRC[k].name)) src = k; // (by its name, as kept)
  lv_net_warm(); // (on a thread of its own; the rest on the worker that fetches)
  mg_start_thread();
  mg.src = src;
  mg_search_cancel();
  LvBook *b = calloc(1, sizeof *b);
  signed char *c = calloc(1, 1);
  if (!b || !c) { free(b); free(c); return; }
  snprintf(b->id, sizeof b->id, "%s", v->page);
  snprintf(b->token, sizeof b->token, "%s", v->extra);
  snprintf(b->title, sizeof b->title, "%s", v->title);
  snprintf(b->cover, sizeof b->cover, "%s", v->preview);
  b->pages = v->length;
  mg.books = b; mg.cstate = c; mg.nbooks = 1; mg.more = 0;
  mg_book_back = S_FAVS;
  mg_open_book(0);
  if (offline) mg_offline_pending = 1;
}

// Two-way Favourites (drive.c): Drive's list folded in once it's here, with what changed here
// meanwhile (from any screen: the main loop calls it)
static int favs_sync_loud; // (asked for with the button: says how it went, even when nothing changed)
static void favs_drive_take(void) {
  if (drive_favs_state == 3) { if (favs_sync_loud) { sfx(SFX_ERROR); say(drive_favs_msg); } drive_favs_state = 0; favs_sync_loud = 0; return; }
  if (drive_favs_state != 2) return;
  drive_favs_state = 0;
  if (!favs_loaded) favs_load();
  if (lv_favs_merge(DRIVE_FAV_LOCAL, DRIVE_FAV_MERGED, DRIVE_FAV_START, DRIVE_FAV_LOCAL, NULL, NULL) == 0) {
    // (what's in Drive now: next time's base)
    sceIoRemove(DRIVE_FAV_BASE);
    sceIoRename(DRIVE_FAV_MERGED, DRIVE_FAV_BASE);
    favs_load();
    fav_drop_textures();
    char m[120];
    int ad = drive_favs_added, rm = drive_favs_removed;
    if (ad || rm) { snprintf(m, sizeof m, "Favourites from Drive: %d new, %d taken out", ad, rm); say(m); }
    else if (favs_sync_loud) say("Favourites: the same as in Drive");
  }
  favs_sync_loud = 0;
}
static void draw_favs(void) {
  fav_take();
  if (!favs_loaded) favs_load();
  { static unsigned last_in; if (frame_no - last_in > 2) { if (drive_signed_in() && lv_net_online()) drive_favs_sync(); } last_in = frame_no; } // (opened: Drive's asked)
  // (the background: the frame is cleared to it already)
  // (the tab's list: made again only when the favourites or the tab change)
  static int list[FAV_MAX], m, l_tab = -1;
  static unsigned l_ver = ~0u;
  if (l_ver != favs_ver || l_tab != fav_tab) { m = fav_list(fav_tab, list); l_ver = favs_ver; l_tab = fav_tab; }
  int comic = fav_tab == 1;
  int cw = comic ? FAV_BW : FAV_CW, ch = comic ? FAV_BH : FAV_CH, gap = 12;
  int cols = (W - 24 + gap) / (cw + gap), x0 = (W - (cols * cw + (cols - 1) * gap)) / 2;
  int rowh = ch + 54, top = 64;
  int rows = (m + cols - 1) / cols;
  float maxs = fmaxf(0, rows * rowh + 20 - (H - top - BAR));
  if (fav_sel >= m) fav_sel = m ? m - 1 : 0;
  // moving
  if (in.touching && in.ty > top) fav_scroll -= in.drag_dy;
  int r = fav_sel / cols;
  if (in.pressed & SCE_CTRL_RIGHT) { if (fav_sel + 1 < m) fav_sel++; sfx(SFX_TICK); }
  if (in.pressed & SCE_CTRL_LEFT) { if (fav_sel > 0) fav_sel--; sfx(SFX_TICK); }
  if (in.pressed & SCE_CTRL_DOWN) { if (fav_sel + cols < m) fav_sel += cols; sfx(SFX_TICK); }
  if (in.pressed & SCE_CTRL_UP) { if (fav_sel - cols >= 0) fav_sel -= cols; sfx(SFX_TICK); }
  if (r != fav_sel / cols) {
    float y = (fav_sel / cols) * rowh;
    if (y < fav_scroll) fav_scroll = y;
    if (y + rowh > fav_scroll + H - top - BAR) fav_scroll = y + rowh - (H - top - BAR);
  }
  if (fav_scroll < 0) fav_scroll = 0;
  if (fav_scroll > maxs) fav_scroll = maxs;
  // the cards
  for (int i = 0; i < m; i++) {
    int k = list[i];
    const Fav *v = &favs[k];
    float x = x0 + (i % cols) * (cw + gap), y = top + 8 + (i / cols) * rowh - fav_scroll;
    if (y > H || y + rowh < top) continue;
    int on = i == fav_sel && !in.touching;
    if (fav_tex[k]) draw_cover(fav_tex[k], x, y, cw, ch, RGBA(255, 255, 255, 255));
    else if (fav_ts[k] == 3) { rrect(x, y, cw, ch, 10, SURF); text_center(x + cw / 2, y + ch / 2, FAINT, 13, "No preview"); }
    else { skeleton(x, y, cw, ch, 10); spinner(x + cw / 2, y + ch / 2, 8); fav_ask(k, cw, ch); }
    if (on) focus_ring(x, y, cw, ch);
    char len[24] = "";
    if (!comic && v->length > 0) snprintf(len, sizeof len, v->length >= 3600 ? "%d:%02d:%02d" : "%d:%02d", v->length >= 3600 ? v->length / 3600 : v->length / 60, v->length >= 3600 ? v->length / 60 % 60 : v->length % 60, v->length % 60);
    if (comic && v->length > 0) snprintf(len, sizeof len, "%d p", v->length);
    if (len[0]) { int tw = text_w(12, len); rrect(x + cw - tw - 16, y + ch - 24, tw + 12, 18, 9, RGBA(0, 0, 0, 170)); text(x + cw - tw - 10, y + ch - 10, TEXT, 12, len); }
    heart(x + 16, y + 16, 6, ACC);
    text_fit((int)x, (int)y + ch + 20, TEXT, 14, v->title[0] ? v->title : v->page, cw);
    text_fit((int)x, (int)y + ch + 38, FAINT, 12, v->site, cw);
    if (hit((int)x, (int)y, cw, ch + 40) && in.tap_y > top && in.tap_y < H - BAR) { fav_sel = i; in.pressed |= SCE_CTRL_CROSS; }
  }
  if (!m) {
    text_center(W / 2, H / 2 - 20, DIM, 20, comic ? "No favourite comics yet" : "No favourite videos yet");
    text_center(W / 2, H / 2 + 12, FAINT, 15, comic ? "♥ Favorite on a comic keeps its link here, without downloading it" : "♥ Favorite on a long video keeps its link here, without downloading it");
  }
  // the top: back, the title, the tabs
  vita2d_draw_rectangle(0, 0, W, top, with_alpha(BG, 250));
  rrect(10, 10, 48, 40, 20, SURF2);
  icon("back", 34, 30, TEXT);
  heart(84, 30, 9, ACC);
  text(100, 38, TEXT, 22, "Favourites");
  static const char *tabs[2] = {"Videos", "Comics"};
  static const char *ticons[2] = {"film", "book"};
  int segw = 300, tw2 = segw / 2, segx = W - segw - 14;
  rrect(segx, 10, segw, 40, 20, SURF2);
  glass(segx + 3 + fav_tab * tw2, 13, tw2 - 6, 34, 0);
  for (int t = 0; t < 2; t++) {
    unsigned c = t == fav_tab ? TEXT : DIM;
    int lw = text_w(16, tabs[t]) + 28, lx = segx + t * tw2 + (tw2 - lw) / 2;
    icon(ticons[t], lx + 10, 30, c);
    text(lx + 26, 36, c, 16, tabs[t]);
    if (hit(segx + t * tw2, 6, tw2, 48) && fav_tab != t) { fav_tab = t; fav_sel = 0; fav_scroll = 0; sfx(SFX_TAB); }
  }
  if (in.pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) { fav_tab ^= 1; fav_sel = 0; fav_scroll = 0; sfx(SFX_TAB); }
  // with Drive (signed in): the button left of the tabs, or SELECT
  if (drive_signed_in()) {
    int sx = segx - 54, busy = drive_favs_state == 1;
    rrect(sx, 10, 44, 40, 20, SURF2);
    if (busy) spinner(sx + 22, 30, 9);
    else icon("refresh", sx + 22, 30, TEXT);
    if ((hit(sx, 6, 44, 48) || (in.pressed & SCE_CTRL_SELECT)) && !busy) {
      if (drive_favs_sync() == 0) { favs_sync_loud = 1; sfx(SFX_OPEN); say("Favourites: in step with Drive…"); }
      else { sfx(SFX_ERROR); say("Drive is busy: in a moment"); }
    }
  }
  // the chosen one's actions, along the foot: open, available offline, remove
  if (m && fav_sel < m) {
    int k = list[fav_sel];
    int by = H - BAR - 56;
    const char *b1 = comic ? "Read" : "Play", *b2 = "Make available offline", *b3 = "Remove";
    int w1 = text_w(16, b1) + 48, w2 = text_w(16, b2) + 48, w3 = text_w(16, b3) + 48;
    int bx = (W - (w1 + w2 + w3 + 24)) / 2;
    rrect(bx - 8, by - 6, w1 + w2 + w3 + 40, 52, 26, with_alpha(SURF, 235));
    rrect(bx, by, w1, 40, 20, ACC); text(bx + 24, by + 26, RGBA(20, 12, 16, 255), 16, b1);
    int a1 = hit(bx, by - 4, w1, 48);
    rrect(bx + w1 + 12, by, w2, 40, 20, SURF2); text(bx + w1 + 36, by + 26, TEXT, 16, b2);
    int a2 = hit(bx + w1 + 12, by - 4, w2, 48);
    rrect(bx + w1 + w2 + 24, by, w3, 40, 20, SURF2); text(bx + w1 + w2 + 48, by + 26, ACC2, 16, b3);
    int a3 = hit(bx + w1 + w2 + 24, by - 4, w3, 48);
    text(16, H - BAR - 64, FAINT, 12, drive_signed_in() ? "✕ open  △ available offline  □ remove  SELECT sync with Drive  ○ back" : "✕ open  △ available offline  □ remove  ○ back");
    if (a2 || (in.pressed & SCE_CTRL_TRIANGLE)) { sfx(SFX_OPEN); if (comic) fav_read(k, 1); else fav_play(k, 1); return; }
    if (a3 || (in.pressed & SCE_CTRL_SQUARE)) { fav_remove(k); fav_drop_textures(); sfx(SFX_DELETE); say("Taken out of Favourites"); return; }
    if (a1 || (in.pressed & SCE_CTRL_CROSS)) { sfx(SFX_SELECT); if (comic) fav_read(k, 0); else fav_play(k, 0); return; }
  }
  if (hit(10, 6, 48, 48) || (in.pressed & SCE_CTRL_CIRCLE)) { sfx(SFX_BACK); fav_drop_textures(); screen = fav_back; return; }
  draw_bar();
}
