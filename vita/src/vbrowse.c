// The video browser, opened from the Videos tab of the site switcher: a big banner with the first of
// what's trending, then rows (Trending, New, Top this month, Top of all time, Something random) of
// landscape cards, as the TV apps have them. A video plays in the Browser's own full view (the row
// handed to it: swipe for the next, □ saves it into the vault). Compiled inside main.c, after web.c.
//
// Each card's preview is decoded straight to the card's own pixels (190 x 107, the picture covered and
// cropped), and the banner's to the banner's: nothing larger is ever made or drawn.

static pthread_mutex_t vbmx = PTHREAD_MUTEX_INITIALIZER; // (the video browser's thread and its list of sources)
static int vb_src = -1;
typedef struct { const char *name, *site, *tags, *icon, *about; int blocked; } VSource;
static const VSource VSRC_BUILTIN[] = {
  // (whole episodes, 10 minutes and longer: kept as a favourite or made available offline)
  {"HentaiWorld", "HentaiWorld", "", "hentaiworld", "Hentai episodes, new each week", 0},
  {"HentaiCloud", "HentaiCloud", "", "hentaicloud", "Hentai episodes, English subs", 0},
  {"HentaiCity", "HentaiCity", "", "hentaicity", "Hentai episodes, 8 min+", 0},
  {"Hentai Episodes", "XVideos Hentai", "", "xvideos", "Whole episodes, 10 min+", 0},
  {"Uncensored Episodes", "XVideos Hentai", "uncensored", "xvideos", "Uncensored episodes, 10 min+", 0},
  {"R34V Longest", "Rule34Video", "longest", "rule34video", "The longest hentai videos", 0},
  {"Rule34Video", "Rule34Video", "hentai", "rule34video", "Hentai animations", 0},
  {"Genshin Impact", "Rule34Video", "genshin-impact", "r34anime", "Genshin animations (Rule34Video)", 0},
  {"Blue Archive", "Rule34Video", "blue-archive", "r34anime", "Blue Archive animations (Rule34Video)", 0},
  {"Sakugabooru", "Sakugabooru", "", "sakugabooru", "Anime animation clips", 0},
  {"Rule34", "Rule34", "video -furry -3d -real", "rule34", "Hentai videos (API key)", 0},
  {"Xbooru", "Xbooru", "video", "xbooru", "Hentai clips", 0},
  {"Gelbooru", "Gelbooru", "video", "gelbooru", "Anime and hentai clips (key)", 0},
  {"Danbooru", "Danbooru", "filetype:mp4", "danbooru", "Anime clips", 0},
  // (both only answer after a Cloudflare browser check, which an app can't pass)
  {"hanime.tv", NULL, NULL, "hanime", "Hentai (browser check)", 1},
  {"MissAV", NULL, NULL, "missav", "JAV (browser check)", 1},
};
#define NVSRC_BUILTIN ((int)(sizeof VSRC_BUILTIN / sizeof *VSRC_BUILTIN))
// The sources offered: the built-in ones that answer, the ones added (the site finder, web.c: saved
// in ux0:data/LWDV00001/video_sources.txt, "<site name>\t<tags>" a line), then the locked ones.
#define VSRC_MAX 32 // (the built-in ones, 16 now, and room for as many added: icon slots 32-63)
static VSource VSRC[VSRC_MAX];
static int NVSRC, vsrc_built, nvs_added;
static char vs_name[VSRC_MAX][48], vs_tags[VSRC_MAX][64];
static void vsrc_build(void) {
  // (the video browser's thread reads the list, under its lock: rebuilt under it too)
  pthread_mutex_lock(&vbmx);
  vsrc_built = 1;
  // (the switcher's icons are kept by place: the places may have moved)
  for (int k = 32; k < 32 + VSRC_MAX; k++) { if (site_tex[k]) release_tex(site_tex[k]); site_tex[k] = NULL; site_tex_tried[k] = 0; }
  NVSRC = 0;
  for (int k = 0; k < NVSRC_BUILTIN; k++) if (!VSRC_BUILTIN[k].blocked) VSRC[NVSRC++] = VSRC_BUILTIN[k];
  for (int k = 0; k < nvs_added && NVSRC < VSRC_MAX - 2; k++) {
    const char *ic = NULL;
    int gone = 0;
    for (int s = 0; s < LV_NSITES; s++) if (!strcmp(LV_SITES[s].name, vs_name[k])) { ic = LV_SITES[s].icon; gone = LV_SITES[s].hidden && LV_SITES[s].kind != BK_KVS; }
    if (gone) continue; // (a site no longer offered: AIBooru)
    VSRC[NVSRC++] = (VSource){vs_name[k], vs_name[k], vs_tags[k], ic, "Added: videos", 0};
  }
  for (int k = 0; k < NVSRC_BUILTIN; k++) if (VSRC_BUILTIN[k].blocked && NVSRC < VSRC_MAX) VSRC[NVSRC++] = VSRC_BUILTIN[k];
  if (vb_src >= NVSRC) vb_src = -1; // (a source removed under the thread: it stops asking)
  pthread_mutex_unlock(&vbmx);
}
static void vsrc_load(void) {
  FILE *f = fopen(DATA "/video_sources.txt", "r");
  char ln[200];
  nvs_added = 0;
  while (f && fgets(ln, sizeof ln, f) && nvs_added < VSRC_MAX - NVSRC_BUILTIN) {
    ln[strcspn(ln, "\r\n")] = 0;
    char *t = strchr(ln, '\t');
    if (!t || t == ln) continue;
    *t = 0;
    snprintf(vs_name[nvs_added], sizeof vs_name[0], "%s", ln);
    snprintf(vs_tags[nvs_added], sizeof vs_tags[0], "%s", t + 1);
    nvs_added++;
  }
  if (f) fclose(f);
  vsrc_build();
}
static void vsrc_save(void) {
  FILE *f = fopen(DATA "/video_sources.txt.part", "w");
  if (!f) return;
  for (int k = 0; k < nvs_added; k++) fprintf(f, "%s\t%s\n", vs_name[k], vs_tags[k]);
  fclose(f);
  sceIoRemove(DATA "/video_sources.txt");
  sceIoRename(DATA "/video_sources.txt.part", DATA "/video_sources.txt");
}
// 1: a source for that site is offered already
static int vsrc_has_site(const char *site) {
  if (!vsrc_built) vsrc_load();
  for (int k = 0; k < NVSRC; k++) if (VSRC[k].site && !strcmp(VSRC[k].site, site)) return 1;
  return 0;
}
static int vsrc_add(const char *site, const char *tags) {
  if (!vsrc_built) vsrc_load();
  if (nvs_added >= VSRC_MAX - NVSRC_BUILTIN || vsrc_has_site(site)) return -1;
  pthread_mutex_lock(&vbmx);
  snprintf(vs_name[nvs_added], sizeof vs_name[0], "%s", site);
  snprintf(vs_tags[nvs_added], sizeof vs_tags[0], "%s", tags);
  nvs_added++;
  pthread_mutex_unlock(&vbmx);
  vsrc_save();
  vsrc_build();
  return 0;
}
// (□ on an added one in the switcher: gone)
static int vsrc_is_added(int k) { return k >= 0 && k < NVSRC && !strcmp(VSRC[k].about, "Added: videos"); }
static void vsrc_remove(int k) {
  pthread_mutex_lock(&vbmx);
  for (int j = 0; j < nvs_added; j++)
    if (!strcmp(vs_name[j], VSRC[k].name)) {
      for (int m = j; m < nvs_added - 1; m++) { memcpy(vs_name[m], vs_name[m + 1], sizeof vs_name[0]); memcpy(vs_tags[m], vs_tags[m + 1], sizeof vs_tags[0]); }
      nvs_added--;
      break;
    }
  pthread_mutex_unlock(&vbmx);
  vsrc_save();
  vsrc_build();
}
static int nvsources(void) { if (!vsrc_built) vsrc_load(); return NVSRC; }
static const char *vsource_name(int k) { return k >= 0 && k < NVSRC && VSRC[k].name ? VSRC[k].name : ""; }
static const char *vsource_icon(int k) { return k >= 0 && k < NVSRC ? VSRC[k].icon : NULL; }
static const char *vsource_about(int k) { return k >= 0 && k < NVSRC && VSRC[k].about ? VSRC[k].about : ""; }
static const char *vsource_tags(int k) { return k >= 0 && k < NVSRC && VSRC[k].tags ? VSRC[k].tags : ""; }
static int vsource_blocked(int k) { return VSRC[k].blocked; }
static int vsource_site(int k) {
  if (k < 0 || k >= NVSRC) return -1;
  for (int s = 0; VSRC[k].site && s < LV_NSITES; s++) if (!strcmp(LV_SITES[s].name, VSRC[k].site)) return s;
  return -1;
}

#define VB_ROWS 5
#define VB_PER 24
#define VB_CW 190
#define VB_CH 107
#define HERO_H 270
static const struct { const char *title; int sort, span; } VROWS[VB_ROWS] = {
  {"Trending now", LV_SORT_TOP, LV_SPAN_WEEK}, {"New", LV_SORT_LATEST, 0}, {"Top this month", LV_SORT_TOP, LV_SPAN_MONTH},
  {"Top of all time", LV_SORT_TOP, LV_SPAN_ALL}, {"Something random", LV_SORT_RANDOM, 0},
};
typedef struct {
  LvPost *p;
  int n, state; // 0 to ask, 1 asked, 2 here, 3 failed
  char err[96];
  vita2d_texture *tex[VB_PER];
  signed char ts[VB_PER]; // per card: 0 nothing, 1 asked, 2 here, 3 failed
} VRow;
static VRow vrows[VB_ROWS];
static vita2d_texture *vb_hero;
static int vb_hero_state;
static int vb_row, vb_col[VB_ROWS + 1];
static float vb_y, vb_x[VB_ROWS + 1]; // (the view: how far down; each row: how far along; [VB_ROWS]: Up next)
// (where each source was left: its row and each row's card, put back when it's opened again)
static struct { int set, row, col[VB_ROWS + 1]; } vb_mem[VSRC_MAX];
static int upnext_n(void), upnext_state(int j), upnext_len(int j); // (favs.c: the favourite videos, "Up next")
static vita2d_texture *upnext_tex(int j, int w, int h);
static const char *upnext_title(int j);
static void upnext_play(int j), upnext_take(void);
static pthread_cond_t vbcv = PTHREAD_COND_INITIALIZER;
static volatile int vb_gen;
static int vb_thread_on;
// (the thread's work and results)
typedef struct { int row, card, gen; LvPixels px; } VbDone; // card -1: the banner
static VbDone vb_done[16];
static int nvb_done;
static struct { int row, card; } vb_want[24];
static int nvb_want;
static struct { int row, gen; LvPost *p; int n; char err[96]; int ready; } vb_list;

static void *vb_main(void *arg) {
  pin_to_core(2); // (off the drawing thread's core)
  (void)arg;
  LvHttp *h = lv_http_new();
  while (worker_run) {
    pthread_mutex_lock(&vbmx);
    int job = -1, card = -2, gen = vb_gen, src = vb_src;
    while (worker_run) {
      gen = vb_gen;
      src = vb_src;
      // a row's list first (in order), then the banner, then the cards wanted
      job = -1;
      for (int r = 0; r < VB_ROWS && src >= 0; r++) if (vrows[r].state == 0 && !vb_list.ready) { job = r; vrows[r].state = 1; break; }
      if (job >= 0) break;
      if (vb_hero_state == 1) { vb_hero_state = 4; card = -1; job = 0; break; }
      if (nvb_want) { job = vb_want[nvb_want - 1].row; card = vb_want[nvb_want - 1].card; nvb_want--; break; }
      pthread_cond_wait(&vbcv, &vbmx);
    }
    // the post's address, copied while held
    char url[1024] = "";
    if (card >= -1 && job >= 0 && vrows[job].p && (card < 0 ? 0 : card) < vrows[job].n) snprintf(url, sizeof url, "%s", vrows[job].p[card < 0 ? 0 : card].preview);
    int site = src >= 0 ? vsource_site(src) : -1;
    char key[200] = "", tags[200] = "";
    if (site >= 0) { snprintf(key, sizeof key, "%s", wkeys[site]); snprintf(tags, sizeof tags, "%s", VSRC[src].tags); }
    int ratings = cfg.ratings;
    pthread_mutex_unlock(&vbmx);
    if (!worker_run) break;
    if (card == -2) {
      // a row: its search
      if (site < 0) continue;
      char u[2048];
      LvSite sc = LV_SITES[site]; // (a copy: the list of sites can change on the drawing thread)
      lv_booru_url_ex(&sc, tags, ratings, VROWS[job].sort, VROWS[job].span, key, 1, u, sizeof u);
      uint8_t *b = NULL;
      size_t n = 0;
      const char *err = NULL;
      int st = lv_http_get(h, u, &b, &n, 8u << 20, NULL, NULL, &err);
      int count = 0;
      LvPost *got = st == 200 ? lv_booru_parse(&sc, (const char *)b, n, ratings, &count) : NULL;
      if (got && lv_site_checks_items(&sc)) count = check_items(h, &sc, got, count, &vb_gen, gen);
      // (videos only: some sites' video tags also have pictures)
      int kept = 0;
      for (int k = 0; got && k < count; k++) {
        if (lv_post_is_video(&got[k]) && kept < VB_PER) got[kept++] = got[k];
        else { LvPost *q = &got[k]; free(q->preview); free(q->sample); free(q->file); free(q->page); free(q->source); free(q->tags); }
      }
      pthread_mutex_lock(&vbmx);
      while (vb_list.ready && worker_run) { pthread_mutex_unlock(&vbmx); sceKernelDelayThread(10000); pthread_mutex_lock(&vbmx); }
      vb_list.row = job; vb_list.gen = gen; vb_list.p = got; vb_list.n = kept; vb_list.ready = 1;
      snprintf(vb_list.err, sizeof vb_list.err, "%s", st == 200 ? (got ? "" : "The site's answer couldn't be read") : err && err[0] ? err : "No answer from the site");
      if (b && strstr((const char *)b, "Just a moment")) snprintf(vb_list.err, sizeof vb_list.err, "The site is asking for a browser check");
      pthread_mutex_unlock(&vbmx);
      free(b);
      continue;
    }
    if (!url[0]) continue;
    // a card's preview (or the banner's), at exactly its size
    uint8_t *b = NULL;
    size_t n = 0;
    VbDone d = {job, card, gen, {0}};
    if (lv_http_get(h, url, &b, &n, 4u << 20, NULL, NULL, NULL) == 200) {
      if (decode_exact(b, n, card < 0 ? W : VB_CW, card < 0 ? HERO_H : VB_CH, &d.px) != 0) d.px.rgba = NULL;
    }
    free(b);
    pthread_mutex_lock(&vbmx);
    if (nvb_done < 16 && gen == vb_gen) vb_done[nvb_done++] = d;
    else free(d.px.rgba);
    pthread_mutex_unlock(&vbmx);
  }
  lv_http_free(h);
  return NULL;
}
static void vb_clear(void) {
  for (int r = 0; r < VB_ROWS; r++) {
    for (int k = 0; k < VB_PER; k++) { if (vrows[r].tex[k]) free_later(vrows[r].tex[k]); vrows[r].tex[k] = NULL; vrows[r].ts[k] = 0; }
    lv_posts_free(vrows[r].p, vrows[r].n);
    vrows[r].p = NULL; vrows[r].n = 0; vrows[r].state = 0; vrows[r].err[0] = 0;
    vb_col[r] = 0; vb_x[r] = 0;
  }
  if (vb_hero) free_later(vb_hero);
  vb_hero = NULL;
  vb_hero_state = 0;
  vb_row = 0;
  vb_y = 0;
}
static void vb_open(int k) {
  lv_net_init();
  wlists_load();
  pthread_mutex_lock(&vbmx);
  vb_gen++;
  for (int i = 0; i < nvb_done; i++) free(vb_done[i].px.rgba);
  nvb_done = 0;
  nvb_want = 0;
  if (vb_list.ready) { lv_posts_free(vb_list.p, vb_list.n); vb_list.ready = 0; }
  if (vb_src >= 0 && vb_src < VSRC_MAX) { vb_mem[vb_src].set = 1; vb_mem[vb_src].row = vb_row; memcpy(vb_mem[vb_src].col, vb_col, sizeof vb_col); }
  vb_clear();
  vb_src = k;
  vb_cur = k;
  if (k >= 0 && k < VSRC_MAX && vb_mem[k].set) { vb_row = vb_mem[k].row; memcpy(vb_col, vb_mem[k].col, sizeof vb_col); }
  pthread_cond_broadcast(&vbcv);
  pthread_mutex_unlock(&vbmx);
  if (!vb_thread_on) {
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512 * 1024); // (it decodes previews)
    vb_thread_on = pthread_create(&t, &at, vb_main, NULL) == 0;
  }
  screen = S_VIDEOS;
}
// (on the drawing thread: lists and pictures in)
static void vb_take(void) {
  pthread_mutex_lock(&vbmx);
  if (vb_list.ready) {
    VRow *r = &vrows[vb_list.row];
    if (vb_list.gen == vb_gen) {
      r->p = vb_list.p; r->n = vb_list.n; r->state = vb_list.p ? 2 : 3;
      snprintf(r->err, sizeof r->err, "%s", vb_list.err);
      if (vb_list.row == 0 && r->n && !vb_hero_state) { vb_hero_state = 1; pthread_cond_broadcast(&vbcv); }
    } else lv_posts_free(vb_list.p, vb_list.n);
    vb_list.ready = 0;
    pthread_cond_broadcast(&vbcv);
  }
  // (four previews a frame at most: a row arriving at once is spread over a few frames, never one long one)
  int n = nvb_done < 4 ? nvb_done : 4;
  VbDone got[16];
  memcpy(got, vb_done, n * sizeof *got);
  memmove(vb_done, vb_done + n, (nvb_done - n) * sizeof *vb_done);
  nvb_done -= n;
  pthread_mutex_unlock(&vbmx);
  for (int k = 0; k < n; k++) {
    VbDone *d = &got[k];
    if (d->gen == vb_gen && d->px.rgba) {
      vita2d_texture *t = upload(&d->px);
      if (d->card < 0) { if (vb_hero) free_later(vb_hero); vb_hero = t; }
      else if (d->card < VB_PER) { if (vrows[d->row].tex[d->card]) free_later(vrows[d->row].tex[d->card]); vrows[d->row].tex[d->card] = t; vrows[d->row].ts[d->card] = t ? 2 : 3; }
    } else if (d->gen == vb_gen && d->card >= 0 && d->card < VB_PER) vrows[d->row].ts[d->card] = 3;
    free(d->px.rgba);
  }
}
// The row a video was chosen from becomes the Browser's list, and the full view opens on it.
static void vb_play(int r, int k) {
  int site = vsource_site(vb_src);
  if (site < 0 || k >= vrows[r].n) return;
  pthread_mutex_lock(&wmx);
  wgen++;
  for (int i = 0; i < nwdone; i++) { free(wdone[i].px.rgba); free(wdone[i].a); free(wdone[i].b); }
  nwdone = 0;
  nwwant = 0;
  if (page_ready.ready) { lv_posts_free(page_ready.posts, page_ready.count); page_ready.ready = 0; page_ready.posts = NULL; }
  lv_posts_free(posts, nposts);
  int n = vrows[r].n;
  posts = calloc(n + 1, sizeof *posts);
  signed char *ns = realloc(wstate, n + 1);
  short *nso = realloc(wslot_of, (n + 1) * sizeof *nso);
  if (ns) wstate = ns;
  if (nso) wslot_of = nso;
  nposts = 0;
  if (posts && ns && nso) {
    for (int i = 0; i < n; i++) { post_copy(&vrows[r].p[i], &posts[i]); wstate[i] = 0; wslot_of[i] = -1; }
    nposts = n;
  }
  grid_build(&wgrid, nposts, post_aspect);
  // (more of the same come as the grid scrolls, like any search)
  cfg.site = site;
  snprintf(cfg.wquery, sizeof cfg.wquery, "%s", VSRC[vb_src].tags);
  cfg.wsort = VROWS[r].sort;
  cfg.wspan = VROWS[r].span;
  job_q.site = site; job_q.ratings = cfg.ratings; job_q.sort = cfg.wsort; job_q.span = cfg.wspan;
  snprintf(job_q.q, sizeof job_q.q, "%s", cfg.wquery);
  snprintf(job_q.key, sizeof job_q.key, "%s", wkeys[site]);
  snprintf(job_q.hidden, sizeof job_q.hidden, "%s", whidden);
  wpage = 1; wend = 0; werr[0] = 0; wloading = 0; list_want = 0;
  big_cancel = 1;
  pthread_mutex_unlock(&wmx);
  web_start();
  web_clear_textures();
  wscroll = wscroll_v = 0;
  if (!nposts) return;
  wsel = k;
  wdetails = 0; wdetails_y = 0; wchrome_until = frame_no + 180;
  wv_back = S_VIDEOS;
  screen = S_WEBVIEW;
  wv_show(k);
}

static void vb_ask(int r, int k) {
  if (r < 0 || r >= VB_ROWS || k < 0 || k >= vrows[r].n || k >= VB_PER || vrows[r].ts[k]) return;
  pthread_mutex_lock(&vbmx);
  if (nvb_want < 24) { vb_want[nvb_want].row = r; vb_want[nvb_want].card = k; nvb_want++; vrows[r].ts[k] = 1; }
  pthread_cond_broadcast(&vbcv);
  pthread_mutex_unlock(&vbmx);
}

static void draw_videos(void) {
  vb_take();
  upnext_take();
  int nup = upnext_n();
  if (vb_row > VB_ROWS || (vb_row == VB_ROWS && !nup)) vb_row = 0;
  vita2d_draw_rectangle(0, 0, W, H, RGBA(8, 6, 10, 255));
  const VSource *src = &VSRC[vb_src < 0 ? 0 : vb_src];
  int site = vsource_site(vb_src);
  // where the view is: the chosen row in sight (the banner scrolls away as rows go down)
  float target = vb_row == 0 ? 0 : HERO_H - 120 + (vb_row - 1) * (VB_CH + 52);
  vb_y += (target - vb_y) * 0.22f;
  float y0 = -vb_y;
  // ---- the banner: the first trending video, big, fading into the page ----
  VRow *r0 = &vrows[0];
  if (vb_hero) {
    // (slowly moving: a little zoomed in, drifting side to side and back, like a trailer)
    float tw = vita2d_texture_get_width(vb_hero), th = vita2d_texture_get_height(vb_hero);
    float u = 0.5f - 0.5f * cosf(frame_no * 6.2832f / 1500), z = 1.1f;
    float pw = tw / z, ph = th / z;
    vita2d_draw_texture_part_scale(vb_hero, 0, y0, (tw - pw) * u, (th - ph) * 0.4f, pw, ph, W / pw, HERO_H / ph);
  } else rrect(0, y0, W, HERO_H, 0, RGBA(30, 20, 30, 255));
  grad(0, y0 + HERO_H - 120, W, 120, RGBA(8, 6, 10, 255), 15, 255, 0);
  grad(0, y0, 360, HERO_H, RGBA(8, 6, 10, 255), 190, 0, 1);
  text(30, y0 + 70, ACC, 15, src->name);
  text(30, y0 + 104, TEXT, 30, "Trending now");
  if (r0->n) {
    char t[160];
    snprintf(t, sizeof t, "%.150s", r0->p[0].tags);
    text_fit(30, y0 + 134, DIM, 15, t, 420);
    int bx = 30, by = (int)y0 + 156;
    rrect(bx, by, 132, 44, 22, RGBA(255, 255, 255, 240));
    text(bx + 26, by + 30, RGBA(10, 8, 12, 255), 18, "▶  Play");
    if (hit(bx, by, 132, 44)) { sfx(SFX_SELECT); vb_play(0, 0); return; }
    rrect(bx + 144, by, 184, 44, 22, with_alpha(TEXT, 50));
    text(bx + 168, by + 30, TEXT, 17, "In the Browser");
    if (hit(bx + 144, by, 184, 44)) { sfx(SFX_TAB); vb_play(0, 0); if (screen == S_WEBVIEW) { wv_drop(); screen = S_WEB; } return; }
  } else if (r0->state == 3) text_fit(30, y0 + 134, ACC2, 16, r0->err, 600);
  else if (site >= 0 && LV_SITES[site].needs_key && !wkeys[site][0]) text_fit(30, y0 + 134, ACC2, 16, "Needs your account's API key: the Browser's Filter → API key", 600);
  else spinner(60, y0 + 140, 16);
  // ---- the rows ----
  int ry = (int)y0 + HERO_H - 30;
  for (int r = 0; r < VB_ROWS; r++, ry += VB_CH + 52) {
    VRow *row = &vrows[r];
    if (ry > H || ry + VB_CH + 40 < 0) continue;
    text(30, ry + 20, r == vb_row ? TEXT : DIM, 18, VROWS[r].title);
    if (row->state == 3) { text(30, ry + 60, FAINT, 14, row->err[0] ? row->err : "Nothing here"); continue; }
    if (row->state < 2) { for (int k = 0; k < 5; k++) skeleton(30 + k * (VB_CW + 12), ry + 32, VB_CW, VB_CH, 10); spinner(30 + VB_CW / 2, ry + 32 + VB_CH / 2, 12); continue; }
    if (!row->n) { text(30, ry + 60, FAINT, 14, "No videos here"); continue; }
    if (vb_col[r] >= row->n) vb_col[r] = row->n - 1; // (a remembered place past a shorter list)
    // (the chosen card kept in sight)
    float want = vb_col[r] * (VB_CW + 12) - (W - 60 - VB_CW) / 2.0f;
    if (want < 0) want = 0;
    if (r == vb_row || vb_x[r] > want + W) vb_x[r] += (want - vb_x[r]) * 0.25f;
    if (in.touching && in.down_y > ry && in.down_y < ry + VB_CH + 40) { vb_x[r] -= in.drag_dx; vb_row = r; }
    for (int k = 0; k < row->n; k++) {
      float x = 30 + k * (VB_CW + 12) - vb_x[r];
      if (x > W || x + VB_CW < 0) continue;
      vb_ask(r, k);
      int on = r == vb_row && k == vb_col[r] && !in.touching;
      float g = on ? 6 : 0, cy = ry + 32;
      vita2d_texture *t = k < VB_PER ? row->tex[k] : NULL;
      if (t) {
        float sx = (VB_CW + 2 * g) / (float)vita2d_texture_get_width(t), sy = (VB_CH + 2 * g) / (float)vita2d_texture_get_height(t);
        vita2d_draw_texture_scale(t, x - g, cy - g, sx, sy);
      } else if (k < VB_PER && row->ts[k] == 1) { skeleton(x - g, cy - g, VB_CW + 2 * g, VB_CH + 2 * g, 10); spinner(x + VB_CW / 2, cy + VB_CH / 2, 10); }
      else rrect(x - g, cy - g, VB_CW + 2 * g, VB_CH + 2 * g, 10, SURF);
      if (on) focus_ring(x - g, cy - g, VB_CW + 2 * g, VB_CH + 2 * g);
      // the play badge, the kind
      circle(x + 22, cy + VB_CH - 20, 13, RGBA(0, 0, 0, 150));
      text_center(x + 23, cy + VB_CH - 15, TEXT, 13, "▶");
      char e[8];
      snprintf(e, sizeof e, "%s", row->p[k].ext);
      for (char *c = e; *c; c++) *c = (char)toupper((unsigned char)*c);
      rrect(x + VB_CW - text_w(11, e) - 18, cy + 8, text_w(11, e) + 12, 18, 9, RGBA(0, 0, 0, 150));
      text(x + VB_CW - text_w(11, e) - 12, cy + 21, TEXT, 11, e);
      if (is_saved(&row->p[k])) { circle(x + VB_CW - 16, cy + VB_CH - 16, 10, ACC); text_center(x + VB_CW - 16, cy + VB_CH - 11, RGBA(20, 12, 16, 255), 12, "✓"); }
      if (hit((int)x, (int)cy, VB_CW, VB_CH) && in.tap_y < H - BAR) { vb_row = r; vb_col[r] = k; sfx(SFX_SELECT); vb_play(r, k); return; }
    }
  }
  // ---- Up next: the favourite videos, after the rows ----
  if (nup) {
    const int r = VB_ROWS;
    if (ry < H && ry + VB_CH + 40 > 0) {
      text(30, ry + 20, r == vb_row ? TEXT : DIM, 18, "Up next \u00b7 your Favourites");
      float want = vb_col[r] * (VB_CW + 12) - (W - 60 - VB_CW) / 2.0f;
      if (want < 0) want = 0;
      if (r == vb_row) vb_x[r] += (want - vb_x[r]) * 0.25f;
      if (in.touching && in.down_y > ry && in.down_y < ry + VB_CH + 40) { vb_x[r] -= in.drag_dx; vb_row = r; }
      for (int k = 0; k < nup; k++) {
        float x = 30 + k * (VB_CW + 12) - vb_x[r], cy = ry + 32;
        if (x > W || x + VB_CW < 0) continue;
        int on = r == vb_row && k == vb_col[r] && !in.touching;
        vita2d_texture *t = upnext_tex(k, VB_CW, VB_CH);
        if (t) draw_cover(t, x, cy, VB_CW, VB_CH, RGBA(255, 255, 255, 255));
        else if (upnext_state(k) == 3) rrect(x, cy, VB_CW, VB_CH, 10, SURF);
        else { skeleton(x, cy, VB_CW, VB_CH, 10); spinner(x + VB_CW / 2, cy + VB_CH / 2, 10); }
        grad(x, cy + VB_CH - 50, VB_CW, 50, RGBA(0, 0, 0, 255), 0, 200, 0);
        text_fit((int)x + 10, (int)cy + VB_CH - 12, TEXT, 13, upnext_title(k), VB_CW - 20);
        int len = upnext_len(k);
        if (len > 0) { char l[16]; snprintf(l, sizeof l, "%d:%02d", len / 60, len % 60); int lw = text_w(11, l) + 12; rrect(x + VB_CW - lw - 6, cy + 8, lw, 18, 9, RGBA(0, 0, 0, 160)); text(x + VB_CW - lw, cy + 21, TEXT, 11, l); }
        heart(x + 16, cy + 18, 6, ACC);
        if (on) focus_ring(x, cy, VB_CW, VB_CH);
        if (hit((int)x, (int)cy, VB_CW, VB_CH) && in.tap_y < H - BAR) { vb_row = r; vb_col[r] = k; sfx(SFX_SELECT); upnext_play(k); return; }
      }
    }
  }
  // ---- the top: back, the source (its tab of the site switcher) ----
  grad(0, 0, W, 70, RGBA(8, 6, 10, 255), 210, 0, 0);
  rrect(16, 10, 44, 40, 20, with_alpha(TEXT, 40));
  icon("back", 38, 30, TEXT);
  if (hit(16, 10, 44, 40)) { sfx(SFX_BACK); screen = S_WEB; return; }
  vita2d_texture *si = site_icon_named(32 + vb_src, src->icon);
  char sl[64];
  snprintf(sl, sizeof sl, "%s  ▾", src->name);
  int sw = text_w(17, sl) + 64;
  rrect(72, 10, sw, 40, 20, with_alpha(TEXT, 40));
  if (si) vita2d_draw_texture_scale(si, 80, 14, 32 / 112.0f, 32 / 112.0f);
  text(120, 37, TEXT, 17, sl);
  if (hit(72, 10, sw, 40)) { sfx(SFX_OPEN); screen = S_WEB; sheet = SH_SITES; sites_tab = 1; sheet_sel = vb_src; sites_scroll = 0; return; }
  text(W - 20 - text_w(13, "✕ play  ·  ○ back  ·  START sources"), 36, FAINT, 13, "✕ play  ·  ○ back  ·  START sources");
  // ---- the buttons ----
  int last_row = nup ? VB_ROWS : VB_ROWS - 1, cur_n = vb_row == VB_ROWS ? nup : vrows[vb_row].n;
  if (in.pressed & SCE_CTRL_DOWN) { if (vb_row < last_row) { vb_row++; sfx(SFX_TICK); } }
  if (in.pressed & SCE_CTRL_UP) { if (vb_row > 0) { vb_row--; sfx(SFX_TICK); } }
  cur_n = vb_row == VB_ROWS ? nup : vrows[vb_row].n;
  VRow *cur = vb_row < VB_ROWS ? &vrows[vb_row] : NULL;
  if (in.pressed & SCE_CTRL_RIGHT) { if (vb_col[vb_row] < cur_n - 1) { vb_col[vb_row]++; sfx(SFX_TICK); } }
  if (in.pressed & SCE_CTRL_LEFT) { if (vb_col[vb_row] > 0) { vb_col[vb_row]--; sfx(SFX_TICK); } }
  vb_x[vb_row] += in.lx * 12;
  if ((in.pressed & SCE_CTRL_CROSS) && cur_n) { sfx(SFX_SELECT); if (cur) vb_play(vb_row, vb_col[vb_row]); else upnext_play(vb_col[vb_row]); return; }
  if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_BACK); screen = S_WEB; return; }
  if (in.pressed & SCE_CTRL_START) { sfx(SFX_OPEN); screen = S_WEB; sheet = SH_SITES; sites_tab = 1; sheet_sel = vb_src; sites_scroll = 0; return; }
  // the next ones just out of sight, ready
  if (cur) for (int k = vb_col[vb_row]; k < vb_col[vb_row] + 6 && k < cur->n; k++) vb_ask(vb_row, k);
}
