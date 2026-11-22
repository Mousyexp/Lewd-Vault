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
  // (whole episodes, 8-10 minutes and longer: kept as a favourite or made available offline.
  //  HentaiCity first, the default; HentaiWorld, the XVideos episode lists, Sakugabooru, Xbooru,
  //  hanime.tv and MissAV taken off the list, 2026-11)
  {"HentaiCity", "HentaiCity", "", "hentaicity", "Hentai episodes, 8 min+", 0},
  {"HentaiOcean", "HentaiOcean", "", "hentaiocean", "Hentai episodes, new each day", 0},
  {"HentaiCloud", "HentaiCloud", "", "hentaicloud", "Hentai episodes, English subs", 0},
  {"R34V Longest", "Rule34Video", "longest", "rule34video", "The longest hentai videos", 0},
  {"Rule34Video", "Rule34Video", "hentai", "rule34video", "Hentai animations", 0},
  {"Genshin Impact", "Rule34Video", "genshin-impact", "r34anime", "Genshin animations (Rule34Video)", 0},
  {"Blue Archive", "Rule34Video", "blue-archive", "r34anime", "Blue Archive animations (Rule34Video)", 0},
  {"Rule34", "Rule34", "video -furry -3d -real", "rule34", "Hentai videos (your API key)", 0},
  {"Gelbooru", "Gelbooru", "video", "gelbooru", "Anime and hentai clips (your API key)", 0},
  {"Danbooru", "Danbooru", "filetype:mp4", "danbooru", "Anime clips", 0},
};
#define NVSRC_BUILTIN ((int)(sizeof VSRC_BUILTIN / sizeof *VSRC_BUILTIN))
// The sources offered: the built-in ones that answer, the ones added (the site finder, web.c: saved
// in ux0:data/LWDV00001/video_sources.txt, "<site name>\t<tags>" a line), then the locked ones.
#define VSRC_MAX 32 // (the built-in ones, 16 now, and room for as many added: icon slots 32-63)
static VSource VSRC[VSRC_MAX];
static int NVSRC, vsrc_built, nvs_added;
static char vs_name[VSRC_MAX][48], vs_tags[VSRC_MAX][64], vs_title[VSRC_MAX][48]; // (vs_title: your own source's name; "" an added site's)
static void vsrc_build(void) {
  // (the video browser's thread reads the list, under its lock: rebuilt under it too)
  pthread_mutex_lock(&vbmx);
  vsrc_built = 2; // (2: rebuilt, vsource_site's answers forgotten)
  // (the switcher's icons are kept by place: the places may have moved)
  for (int k = 32; k < 32 + VSRC_MAX; k++) { if (site_tex[k]) release_tex(site_tex[k]); site_tex[k] = NULL; site_tex_tried[k] = 0; }
  NVSRC = 0;
  for (int k = 0; k < NVSRC_BUILTIN; k++) if (!VSRC_BUILTIN[k].blocked) VSRC[NVSRC++] = VSRC_BUILTIN[k];
  for (int k = 0; k < nvs_added && NVSRC < VSRC_MAX - 2; k++) {
    const char *ic = NULL;
    int gone = 0;
    for (int s = 0; s < LV_NSITES; s++) if (!strcmp(LV_SITES[s].name, vs_name[k])) { ic = LV_SITES[s].icon; gone = LV_SITES[s].hidden && LV_SITES[s].kind != BK_KVS; }
    if (gone) continue; // (a site no longer offered: AIBooru)
    VSRC[NVSRC++] = vs_title[k][0] ? (VSource){vs_title[k], vs_name[k], vs_tags[k], ic, "Yours: videos", 0}
                                   : (VSource){vs_name[k], vs_name[k], vs_tags[k], ic, "Added: videos", 0};
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
    char *t2 = strchr(t + 1, '\t'); // (a third part: your own source's name)
    if (t2) *t2 = 0;
    snprintf(vs_name[nvs_added], sizeof vs_name[0], "%s", ln);
    snprintf(vs_tags[nvs_added], sizeof vs_tags[0], "%s", t + 1);
    snprintf(vs_title[nvs_added], sizeof vs_title[0], "%s", t2 ? t2 + 1 : "");
    nvs_added++;
  }
  if (f) fclose(f);
  vsrc_build();
}
static void vsrc_save(void) {
  FILE *f = fopen(DATA "/video_sources.txt.part", "w");
  if (!f) return;
  for (int k = 0; k < nvs_added; k++) { if (vs_title[k][0]) fprintf(f, "%s\t%s\t%s\n", vs_name[k], vs_tags[k], vs_title[k]); else fprintf(f, "%s\t%s\n", vs_name[k], vs_tags[k]); }
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
  vs_title[nvs_added][0] = 0;
  nvs_added++;
  pthread_mutex_unlock(&vbmx);
  vsrc_save();
  vsrc_build();
  return 0;
}
// Your own: a site with videos and the tags to ask it for, under a name of yours (the same site can be
// several of these). -1 when there's no room, or one already has that name.
static int vsrc_add_own(const char *site, const char *tags, const char *title) {
  if (!vsrc_built) vsrc_load();
  if (nvs_added >= VSRC_MAX - NVSRC_BUILTIN || !title[0]) return -1;
  for (int k = 0; k < NVSRC; k++) if (VSRC[k].name && !strcmp(VSRC[k].name, title)) return -1;
  pthread_mutex_lock(&vbmx);
  snprintf(vs_name[nvs_added], sizeof vs_name[0], "%s", site);
  snprintf(vs_tags[nvs_added], sizeof vs_tags[0], "%s", tags);
  snprintf(vs_title[nvs_added], sizeof vs_title[0], "%s", title);
  for (char *c = vs_title[nvs_added]; *c; c++) if (*c == '\t') *c = ' ';
  for (char *c = vs_tags[nvs_added]; *c; c++) if (*c == '\t') *c = ' ';
  nvs_added++;
  pthread_mutex_unlock(&vbmx);
  vsrc_save();
  vsrc_build();
  return 0;
}
// (□ on an added one in the switcher: gone)
static int vsrc_is_added(int k) { return k >= 0 && k < NVSRC && (!strcmp(VSRC[k].about, "Added: videos") || !strcmp(VSRC[k].about, "Yours: videos")); }
static void vsrc_remove(int k) {
  pthread_mutex_lock(&vbmx);
  for (int j = 0; j < nvs_added; j++)
    if (!strcmp(vs_title[j][0] ? vs_title[j] : vs_name[j], VSRC[k].name)) {
      for (int m = j; m < nvs_added - 1; m++) { memcpy(vs_name[m], vs_name[m + 1], sizeof vs_name[0]); memcpy(vs_tags[m], vs_tags[m + 1], sizeof vs_tags[0]); memcpy(vs_title[m], vs_title[m + 1], sizeof vs_title[0]); }
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
  // (asked every frame: found by name once per source, until the list or the sites change)
  static int memo[VSRC_MAX], memo_n = -1, memo_sites = -1;
  if (memo_n != NVSRC || memo_sites != LV_NSITES || vsrc_built == 2) { for (int j = 0; j < VSRC_MAX; j++) memo[j] = -2; memo_n = NVSRC; memo_sites = LV_NSITES; if (vsrc_built == 2) vsrc_built = 1; }
  if (memo[k] != -2) return memo[k];
  int r = -1;
  for (int s = 0; VSRC[k].site && s < LV_NSITES; s++) if (!strcmp(LV_SITES[s].name, VSRC[k].site)) { r = s; break; }
  return memo[k] = r;
}

#define VB_ROWS 8 // (at most: the source's own, five orders or a New row and its genres)
#define VB_PER 24
#define VB_CW 236
#define VB_CH 133 // (bigger cards: fewer at a time, easier to see)
#define HERO_H 270
// A row: its title, the order asked for and, for a genre, the words searched for with the source's
typedef struct { const char *title; int sort, span; const char *tag; } VRowDef;
static const VRowDef VROWS_ORDERS[] = {
  {"Trending now", LV_SORT_TOP, LV_SPAN_WEEK, ""}, {"New", LV_SORT_LATEST, 0, ""}, {"Top this month", LV_SORT_TOP, LV_SPAN_MONTH, ""},
  {"Top of all time", LV_SORT_TOP, LV_SPAN_ALL, ""}, {"Something random", LV_SORT_RANDOM, 0, ""},
};
// (the long-form sources, whose orders mostly come back the same: their newest, then genres, each a
//  search on the site itself. A genre the site has nothing for, or that repeats a row, isn't shown.)
static const VRowDef VROWS_GENRES[] = {
  {"New", LV_SORT_LATEST, 0, ""}, {"MILF", LV_SORT_LATEST, 0, "milf"},
  {"Elf", LV_SORT_LATEST, 0, "elf"}, {"Maid", LV_SORT_LATEST, 0, "maid"}, {"Big breasts", LV_SORT_LATEST, 0, "big_breasts"},
  {"Harem", LV_SORT_LATEST, 0, "harem"}, {"Romance", LV_SORT_LATEST, 0, "romance"},
};
static VRowDef VROWS[VB_ROWS];
static int vb_nrows = 5;
static int vsource_site(int k);
// (the rows for a source: genres for the long-form ones (episode sites, XVideos, Rule34Video's
//  hentai), the five orders for the rest)
static void vrows_for(int src) {
  int site = src >= 0 ? vsource_site(src) : -1, kind = site >= 0 ? LV_SITES[site].kind : -1;
  const char *tags = src >= 0 ? VSRC[src].tags : "";
  int genres = kind == BK_HWORLD || kind == BK_HCLOUD || kind == BK_HCITY || kind == BK_XV ||
               (kind == BK_KVS && tags && !strcmp(tags, "hentai"));
  const VRowDef *from = genres ? VROWS_GENRES : VROWS_ORDERS;
  vb_nrows = genres ? (int)(sizeof VROWS_GENRES / sizeof *VROWS_GENRES) : (int)(sizeof VROWS_ORDERS / sizeof *VROWS_ORDERS);
  if (vb_nrows > VB_ROWS) vb_nrows = VB_ROWS;
  for (int r = 0; r < VB_ROWS; r++) VROWS[r] = r < vb_nrows ? from[r] : (VRowDef){"", 0, 0, ""};
}
// (the words a row asks for: the source's and the row's genre; Rule34Video's tag page takes one tag,
//  the genre's in place of the source's)
static void vrow_tags(int src, int r, char *out, size_t cap) {
  const char *st = src >= 0 ? VSRC[src].tags : "", *gt = VROWS[r].tag ? VROWS[r].tag : "";
  int site = src >= 0 ? vsource_site(src) : -1;
  if (gt[0] && site >= 0 && LV_SITES[site].kind == BK_KVS) snprintf(out, cap, "%s", gt);
  else snprintf(out, cap, "%s%s%s", st ? st : "", st && st[0] && gt[0] ? " " : "", gt);
}
typedef struct {
  LvPost *p;
  int n, state; // 0 to ask, 1 asked, 2 here, 3 failed
  char err[96];
  vita2d_texture *tex[VB_PER];
  signed char ts[VB_PER]; // per card: 0 nothing, 1 asked, 2 here, 3 failed
  int dup; // (the same videos as a row above: a source that orders them all alike; not shown)
} VRow;
#define VB_SHOW 14 // (cards in a row; the next place is its All tile: the whole of it in the Browser)
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
static struct { int row, gen; LvPost *p; int n; char err[96]; int ready, append; } vb_list; // (append: more of a row already shown)
// (the worker hands a row's posts to the drawing thread: the whole list, or a part to add to it)
static void vb_publish(int row, int gen, LvPost *p, int n, const char *err, int append) {
  pthread_mutex_lock(&vbmx);
  while (vb_list.ready && worker_run) pthread_cond_wait(&vbcv, &vbmx); // (woken when it's taken, not looking every 10 ms)
  vb_list.row = row; vb_list.gen = gen; vb_list.p = p; vb_list.n = n; vb_list.ready = 1; vb_list.append = append;
  snprintf(vb_list.err, sizeof vb_list.err, "%s", err ? err : "");
  pthread_mutex_unlock(&vbmx);
}

// (a row's search answered in the last minute, kept: the rows of a source that answers every order
//  with the same page (HentaiOcean's feed, 440 KB) fetch it once, not once a row)
static struct { char url[1024]; uint8_t *b; size_t n; uint64_t at; } vbc[2];
static pthread_mutex_t vbc_mx = PTHREAD_MUTEX_INITIALIZER;
static int vb_cache_get(const char *u, uint8_t **b, size_t *n) {
  int got = 0;
  uint64_t now = sceKernelGetProcessTimeWide();
  pthread_mutex_lock(&vbc_mx);
  for (int k = 0; k < 2 && !got; k++)
    if (vbc[k].b && now - vbc[k].at < 60000000ull && strlen(u) < sizeof vbc[k].url && !strcmp(vbc[k].url, u)) {
      uint8_t *c = malloc(vbc[k].n + 1);
      if (c) { memcpy(c, vbc[k].b, vbc[k].n + 1); *b = c; *n = vbc[k].n; got = 1; }
    }
  pthread_mutex_unlock(&vbc_mx);
  return got;
}
static void vb_cache_put(const char *u, const uint8_t *b, size_t n) {
  if (strlen(u) >= sizeof vbc[0].url || n > (2u << 20)) return;
  uint8_t *c = malloc(n + 1);
  if (!c) return;
  memcpy(c, b, n); c[n] = 0;
  pthread_mutex_lock(&vbc_mx);
  int k = vbc[0].at <= vbc[1].at ? 0 : 1; // (the older one goes)
  free(vbc[k].b);
  snprintf(vbc[k].url, sizeof vbc[k].url, "%s", u); vbc[k].b = c; vbc[k].n = n; vbc[k].at = sceKernelGetProcessTimeWide();
  pthread_mutex_unlock(&vbc_mx);
}
static void *vb_main(void *arg) {
  pin_to_core(2); // (off the drawing thread's core)
  int helper = arg != NULL; // (the second thread: rows two at a time, except an episode site's, whose page checks would be four connections at once)
  LvHttp *h = lv_http_new();
  LvHttp *h2 = lv_http_new(); // (a second connection: episode pages checked two at a time)
  while (worker_run) {
    pthread_mutex_lock(&vbmx);
    int job = -1, card = -2, gen = vb_gen, src = vb_src;
    while (worker_run) {
      gen = vb_gen;
      src = vb_src;
      // the first row's list, then the banner, then the cards on screen, then the other rows' lists
      // (the row being looked at first): pictures show before every list is in (on an episode site
      // each list is a page check per episode)
      job = -1;
      if (src >= 0 && vrows[0].state == 0 && !vb_list.ready) { job = 0; vrows[0].state = 1; break; }
      if (vb_hero_state == 1) { vb_hero_state = 4; card = -1; job = 0; break; }
      if (nvb_want) { job = vb_want[nvb_want - 1].row; card = vb_want[nvb_want - 1].card; nvb_want--; break; }
      // (the other rows' lists only while the video browser is on screen: not fetched and checked
      //  in the background while something else is looked at, or a video from it streams)
      int episodes = 0;
      { int st2 = src >= 0 ? vsource_site(src) : -1; episodes = st2 >= 0 && lv_site_checks_items(&LV_SITES[st2]); }
      for (int d = 0; d < vb_nrows && src >= 0 && !vb_list.ready && screen == S_VIDEOS && !(helper && episodes); d++) {
        int r = (vb_row < vb_nrows ? vb_row : 0) + d;
        if (r >= vb_nrows) r -= vb_nrows;
        if (vrows[r].state == 0) { job = r; vrows[r].state = 1; break; }
      }
      if (job >= 0) break;
      wait_work(&vbcv, &vbmx, h);
    }
    // the post's address, copied while held
    char url[1024] = "";
    if (card >= -1 && job >= 0 && vrows[job].p && (card < 0 ? 0 : card) < vrows[job].n) snprintf(url, sizeof url, "%s", vrows[job].p[card < 0 ? 0 : card].preview);
    int site = src >= 0 ? vsource_site(src) : -1;
    char key[200] = "", tags[200] = "";
    if (site >= 0) { snprintf(key, sizeof key, "%s", wkeys[site]); vrow_tags(src, job >= 0 ? job : 0, tags, sizeof tags); }
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
      int st = vb_cache_get(u, &b, &n) ? 200 : -1;
      if (st != 200) { st = lv_http_get(h, u, &b, &n, 8u << 20, NULL, NULL, &err); if (st == 200 && b) vb_cache_put(u, b, n); }
      int count = 0;
      LvPost *got = st == 200 ? lv_booru_parse(&sc, (const char *)b, n, ratings, &count) : NULL;
      LOG("vb row %d: %s -> %d, %u bytes, %d posts\n", job, u, st, (unsigned)n, count);
      if (got && lv_site_checks_items(&sc)) {
        // (an episode site: each one's page is read first, a few at a time, and those that pass are
        //  shown as they come: the row's first cards after a few pages, not after all of them)
        int shown_n = 0;
        for (int pos = 0, step = 2; pos < count; pos += step, step = 4) {
          int m = count - pos < step ? count - pos : step; // (two first: something to see soonest)
          // (off screen: paused here until it's back, or another source is opened)
          pthread_mutex_lock(&vbmx);
          while (worker_run && gen == vb_gen && screen != S_VIDEOS && shown_n > 0) pthread_cond_wait(&vbcv, &vbmx);
          pthread_mutex_unlock(&vbmx);
          if (gen != vb_gen || shown_n >= VB_PER) { for (int k = pos; k < count; k++) { LvPost *q = &got[k]; free(q->preview); free(q->sample); free(q->file); free(q->page); free(q->source); free(q->tags); } break; }
          LvPost *part = malloc(m * sizeof *part);
          if (!part) continue;
          memcpy(part, got + pos, m * sizeof *part);
          int ok = check_items2(h, h2, &sc, part, m, &vb_gen, gen), kept = 0;
          LOG("vb row %d: checked %d of %d\n", job, ok, m);
          for (int k = 0; k < ok; k++) {
            if (lv_post_is_video(&part[k]) && shown_n + kept < VB_PER) part[kept++] = part[k];
            else { LvPost *q = &part[k]; free(q->preview); free(q->sample); free(q->file); free(q->page); free(q->source); free(q->tags); }
          }
          if (kept) { vb_publish(job, gen, part, kept, "", shown_n > 0); shown_n += kept; }
          else free(part);
        }
        free(got); // (the posts went on in parts, or were freed)
        if (!shown_n) vb_publish(job, gen, NULL, 0, st == 200 ? "Nothing here could be shown" : err && err[0] ? err : "No answer from the site", 0);
        free(b);
        continue;
      }
      // (videos only: some sites' video tags also have pictures)
      int kept = 0;
      for (int k = 0; got && k < count; k++) {
        if (lv_post_is_video(&got[k]) && kept < VB_PER) got[kept++] = got[k];
        else { LvPost *q = &got[k]; free(q->preview); free(q->sample); free(q->file); free(q->page); free(q->source); free(q->tags); }
      }
      const char *why = st == 200 ? (got ? "" : "The site's answer couldn't be read") : err && err[0] ? err : "No answer from the site";
      if (b && strstr((const char *)b, "Just a moment")) why = "The site is asking for a browser check";
      vb_publish(job, gen, got, kept, why, 0);
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
  if (h2) lv_http_free(h2);
  return NULL;
}
static void vb_clear(void) {
  for (int r = 0; r < VB_ROWS; r++) {
    for (int k = 0; k < VB_PER; k++) { if (vrows[r].tex[k]) release_tex(vrows[r].tex[k]); vrows[r].tex[k] = NULL; vrows[r].ts[k] = 0; }
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
static int vb_key_src = -1; // (a source that needs your API key, asked for: opened once it's typed)
static void vb_open(int k) {
  { int site = vsource_site(k);
    if (site >= 0 && LV_SITES[site].needs_key && !wkeys[site][0] && !ime_open) { // (its key first: your user ID, then the key)
      cfg.site = site; vb_key_src = k; sfx(SFX_OPEN);
      ime_start(10, "User ID (a number) for your API key", "", 0);
      return;
    } }
  lv_net_warm(); // (on a thread of its own; the rest on the worker that fetches)
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
  vrows_for(k);
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
    if (vb_thread_on) pthread_create(&t, &at, vb_main, (void *)1); // (a second: two rows at a time)
  }
  screen = S_VIDEOS;
}
// (on the drawing thread: lists and pictures in)
// (a row whose first videos are mostly those of a row above it: the source gives every order the same
//  list, so the row says nothing new and isn't shown. The first row is always shown.)
static int vb_same_post(const LvPost *a, const LvPost *b) {
  const char *x = a->page && a->page[0] ? a->page : a->file, *y = b->page && b->page[0] ? b->page : b->file;
  return x && y && !strcmp(x, y);
}
static void vb_dup_check(int r) {
  VRow *row = &vrows[r];
  row->dup = 0;
  if (r == 0 || row->n < 2) return;
  int m = row->n < 8 ? row->n : 8;
  for (int o = 0; o < r; o++) {
    VRow *up = &vrows[o];
    if (up->dup || up->state != 2 || !up->n) continue;
    int same = 0;
    for (int i = 0; i < m; i++) for (int j = 0; j < up->n && j < 24; j++) if (vb_same_post(&row->p[i], &up->p[j])) { same++; break; }
    if (same * 10 >= m * 6) { row->dup = 1; return; } // (six in ten or more: the same)
  }
}
// (a row's repeats taken out: the same video twice, or the same picture (a series' episodes a site
//  shows with one cover): one card for it, the row as short as it really is)
static void vb_dedupe(VRow *row) {
  int kept = 0;
  for (int i = 0; i < row->n; i++) {
    int rep = 0;
    for (int j = 0; j < kept && !rep; j++)
      rep = vb_same_post(&row->p[i], &row->p[j]) || (row->p[i].preview && row->p[j].preview && !strcmp(row->p[i].preview, row->p[j].preview));
    if (rep) { LvPost *q = &row->p[i]; free(q->preview); free(q->sample); free(q->file); free(q->page); free(q->source); free(q->tags); continue; }
    if (kept != i) row->p[kept] = row->p[i];
    kept++;
  }
  row->n = kept;
}
static int vb_row_shown(int r) { return r == VB_ROWS || (r < vb_nrows && !vrows[r].dup && !(vrows[r].state == 2 && !vrows[r].n && r > 0) && !(vrows[r].state == 3 && r > 0 && VROWS[r].tag && VROWS[r].tag[0])); }
static void vb_take(void) {
  if (!vb_list.ready && !nvb_done) return; // (nothing come: no lock)
  pthread_mutex_lock(&vbmx);
  if (vb_list.ready) {
    VRow *r = &vrows[vb_list.row];
    if (vb_list.gen == vb_gen && vb_list.append && r->p && vb_list.p) {
      // (more of a row: added at its end)
      LvPost *np = realloc(r->p, (r->n + vb_list.n) * sizeof *np);
      if (np) { memcpy(np + r->n, vb_list.p, vb_list.n * sizeof *np); r->p = np; r->n += vb_list.n; free(vb_list.p); vb_dedupe(r); }
      else lv_posts_free(vb_list.p, vb_list.n);
      vb_dup_check(vb_list.row);
    } else if (vb_list.gen == vb_gen) {
      if (r->p) lv_posts_free(r->p, r->n);
      r->p = vb_list.p; r->n = vb_list.n; r->state = vb_list.p ? 2 : 3;
      if (r->p) vb_dedupe(r);
      snprintf(r->err, sizeof r->err, "%s", vb_list.err);
      if (vb_list.row == 0 && r->n && !vb_hero_state) { vb_hero_state = 1; pthread_cond_broadcast(&vbcv); }
      vb_dup_check(vb_list.row);
      for (int q = vb_list.row + 1; q < VB_ROWS; q++) if (vrows[q].state == 2) vb_dup_check(q); // (those below, again)
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
      vita2d_texture *t = d->card < 0 ? upload(&d->px) : pool_put(&pool_cards, &d->px); // (the cards in a pool; the banner its own)
      if (d->card < 0) { if (vb_hero) release_tex(vb_hero); vb_hero = t; }
      else if (d->card < VB_PER) { if (vrows[d->row].tex[d->card]) release_tex(vrows[d->row].tex[d->card]); vrows[d->row].tex[d->card] = t; vrows[d->row].ts[d->card] = t ? 2 : 3; }
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
  vrow_tags(vb_src, r, cfg.wquery, sizeof cfg.wquery);
  wq_hidden = 1; // (the row's own search: not shown in the Browser's search bar)
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

// The whole of a row (its category) as the Browser's grid, more coming as it scrolls
static void vb_all(int r) {
  if (r < 0 || r >= VB_ROWS || !vrows[r].n) return;
  vb_play(r, 0);
  if (screen == S_WEBVIEW) { wv_drop(); screen = S_WEB; wsel = 0; wscroll = 0; }
  char m[80];
  snprintf(m, sizeof m, "All of \u201c%s\u201d", VROWS[r].title);
  say(m);
}
// (the video browser left: the previews waiting dropped, asked again when it's back)
static void vb_drop_wants(void) {
  pthread_mutex_lock(&vbmx);
  for (int k = 0; k < nvb_want; k++) { int r = vb_want[k].row, c = vb_want[k].card; if (r >= 0 && r < VB_ROWS && c >= 0 && c < VB_PER && vrows[r].ts[c] == 1) vrows[r].ts[c] = 0; }
  nvb_want = 0;
  pthread_mutex_unlock(&vbmx);
}
static void vb_ask(int r, int k) {
  if (r < 0 || r >= VB_ROWS || k < 0 || k >= vrows[r].n || k >= VB_PER || vrows[r].ts[k]) return;
  pthread_mutex_lock(&vbmx);
  if (nvb_want < 24) { vb_want[nvb_want].row = r; vb_want[nvb_want].card = k; nvb_want++; vrows[r].ts[k] = 1; }
  pthread_cond_broadcast(&vbcv);
  pthread_mutex_unlock(&vbmx);
}

// (a video's name from its page's address: "/videos/some-title-1" as "Some title 1")
static void vb_title_of(const LvPost *p, char *out, size_t cap) {
  const char *pg = p && p->page ? p->page : "";
  size_t pl = strlen(pg); while (pl && pg[pl - 1] == '/') pl--;
  size_t ps = pl; while (ps && pg[ps - 1] != '/') ps--;
  // (an address ending in its language, ".../title/english", or a bare number: the part before)
  static const char *const LANG[] = {"english", "eng", "en", "raw", "sub", "dub", "subbed", "dubbed", "uncensored", NULL};
  for (int tries = 0; tries < 2 && ps > 1; tries++) {
    int skip = 1;
    for (size_t i = ps; i < pl; i++) if (!isdigit((unsigned char)pg[i])) skip = 0;
    for (int k = 0; LANG[k] && !skip; k++) if (pl - ps == strlen(LANG[k]) && !strncasecmp(pg + ps, LANG[k], pl - ps)) skip = 1;
    if (!skip) break;
    pl = ps - 1; ps = pl; while (ps && pg[ps - 1] != '/') ps--;
  }
  size_t j = 0;
  for (size_t i = ps; i < pl && j < cap - 1 && pg[i] != '?' && pg[i] != '.'; i++) out[j++] = pg[i] == '-' || pg[i] == '_' ? ' ' : pg[i];
  out[j] = 0;
  // (an address like /posts/123 or /video/123 names no title: then its first few tags, spelled out)
  static const char *const GENERIC[] = {"posts", "post", "video", "videos", "watch", "view", "show", "play", "index", "index php", "page", "embed", "v", "p", NULL};
  for (int k = 0; GENERIC[k]; k++) if (!strcasecmp(out, GENERIC[k])) { out[0] = 0; break; }
  if (!out[0] && p && p->tags) {
    char tg[400]; snprintf(tg, sizeof tg, "%s", p->tags);
    int n = 0; j = 0;
    for (char *t = strtok(tg, " "); t && n < 3; t = strtok(NULL, " ")) {
      if (strlen(t) < 3 || strchr(t, ':') || strstr(t, "video") || strstr(t, "sound") || strstr(t, "animated") || strstr(t, "res")) continue;
      for (const char *c = j ? ", " : ""; *c && j < cap - 1; c++) out[j++] = *c;
      for (const char *c = t; *c && j < cap - 1; c++) out[j++] = *c == '_' ? ' ' : *c;
      n++;
    }
    out[j] = 0;
  }
  if (out[0]) out[0] = (char)toupper((unsigned char)out[0]);
}
static int vb_all_row = -1; static float vb_all_scroll; static unsigned vb_all_at; // (All: the row's window, over the page)
static int vb_lock; static int vb_free; static float vb_vy; static int vb_free_x[VB_ROWS + 1]; // (a row moved by touch: stays where it was left until the D-pad moves in it) // (a touch: locked to across or down; scrolled freely by it)
static int vb_hero_k; static unsigned vb_hero_at; static float vb_hero_x; // (the carousel: which, since when, dragged)
static void draw_vb_all(void);
static void draw_videos(void) {
  if (!VROWS[0].title) vrows_for(vb_src); // (the rows not made yet: this source's)
  Input vb_keep = in; // (All's window open: the page under it gets no input)
  if (vb_all_row >= 0) { memset(&in, 0, sizeof in); in.now = vb_keep.now; }
  // (a touch: across a row, or up and down the page, whichever it moves along first)
  if (!in.touching) vb_lock = 0;
  else if (!vb_lock && (fabsf((float)(in.tx - in.down_x)) > 12 || fabsf((float)(in.ty - in.down_y)) > 12)) vb_lock = fabsf((float)(in.tx - in.down_x)) > fabsf((float)(in.ty - in.down_y)) ? 1 : 2;
  vb_take();
  upnext_take();
  { // (back on screen after being away: the worker carries on with what it paused)
    static unsigned seen;
    if (frame_no - seen > 2) { pthread_mutex_lock(&vbmx); pthread_cond_broadcast(&vbcv); pthread_mutex_unlock(&vbmx); }
    seen = frame_no;
  }
  int nup = upnext_n();
  if (vb_row > VB_ROWS || (vb_row == VB_ROWS && !nup)) vb_row = 0;
  vita2d_draw_rectangle(0, 0, W, H, RGBA(8, 6, 10, 255));
  const VSource *src = &VSRC[vb_src < 0 ? 0 : vb_src];
  int site = vsource_site(vb_src);
  // where the view is: the chosen row in sight (the banner scrolls away as rows go down)
  int vis = 0; // (the chosen row's place among those shown)
  for (int r = 0; r < vb_row && r < VB_ROWS; r++) vis += vb_row_shown(r);
  float target = vb_row == 0 ? 0 : HERO_H - 4 + vis * (VB_CH + 52) - 196 - 6; // (the chosen row just under the banner)
  int nshown_rows = 0;
  for (int r = 0; r < VB_ROWS; r++) nshown_rows += vb_row_shown(r);
  float vmax = HERO_H - 4 + (nshown_rows + (nup ? 1 : 0)) * (VB_CH + 52) - (H - BAR) + 20;
  if (vmax < 0) vmax = 0;
  if (in.touching && vb_lock == 2) { vb_y -= in.drag_dy; vb_vy = 0; vb_free = 1; }
  else if (in.flicked && vb_lock == 2) vb_vy = -in.fvy * 1.2f;
  if (vb_free) {
    vb_y += vb_vy * ft; vb_vy *= decay(0.93f);
    if (vb_y < 0) { vb_y = 0; vb_vy = 0; } if (vb_y > vmax) { vb_y = vmax; vb_vy = 0; }
    // (the row nearest the middle of the view becomes the chosen one, for the buttons)
    if (!in.touching) { int best = 0, k = 0; float bd = 1e9f;
      for (int r = 0; r < VB_ROWS; r++) if (vb_row_shown(r)) { float ry = HERO_H - 4 + k * (VB_CH + 52) + VB_CH / 2 - vb_y; float d = fabsf(ry - H / 2); if (d < bd) { bd = d; best = r; } k++; }
      if (vb_y < 60) best = 0;
      vb_row = best; }
  } else { // (to the chosen row on a spring: it carries on a touch past and settles back)
    static float vb_sv;
    vb_sv = (vb_sv + (target - vb_y) * 0.045f * ft) * decay(0.8f);
    vb_y += vb_sv * ft;
    if (fabsf(target - vb_y) < 0.3f && fabsf(vb_sv) < 0.2f) { vb_y = target; vb_sv = 0; }
  }
  // (the banner stays at the top: as the rows go up it shrinks to HERO_MIN, then they pass under it)
  const float HERO_MIN = 196;
  float hh = HERO_H - fminf(fmaxf(vb_y, 0), HERO_H - HERO_MIN), shrink = HERO_H - hh;
  float y0 = 0;
  // ---- the top: the chosen video, its picture over the right half (its own shape, not stretched),
  //      fading into the page at its left; its name big, its tags as glass chips, Play ----
  VRow *r0 = &vrows[0];
  VRow *hr = vb_row < VB_ROWS && vrows[vb_row].n ? &vrows[vb_row] : r0;
  int hk = vb_row < VB_ROWS && vrows[vb_row].n ? vb_col[vb_row] : 0;
  if (hk >= hr->n) hk = hr->n ? hr->n - 1 : 0;
  int hrow = hr == r0 ? 0 : vb_row;
  (void)vb_hero_k; (void)vb_hero_at; (void)vb_hero_x;
  // ---- the rows ----
  int ry = (int)y0 + HERO_H - 4;
  for (int r = 0; r < VB_ROWS; r++) {
    VRow *row = &vrows[r];
    if (!vb_row_shown(r)) continue; // (the same as a row above, a genre with nothing, or one this source hasn't)
    ry += VB_CH + 52;
    int ry0 = ry - (VB_CH + 52); // (this row's top)
    #define ry ry0
    if (ry > H || ry + VB_CH + 40 < 0) continue;
    text(30, ry + 20, r == vb_row ? TEXT : DIM, 18, VROWS[r].title);
    if (row->state == 3) { text(30, ry + 60, FAINT, 14, row->err[0] ? row->err : "Nothing here"); continue; }
    if (row->state < 2) { for (int k = 0; k < 5; k++) skeleton(30 + k * (VB_CW + 12), ry + 32, VB_CW, VB_CH, 10); spinner(30 + VB_CW / 2, ry + 32 + VB_CH / 2, 12); if (r == vb_row && !in.touching) focus_ring(30, ry + 32, VB_CW, VB_CH); continue; } // (the ring on its first while it loads)
    if (!row->n) { text(30, ry + 60, FAINT, 14, "No videos here"); continue; }
    if (vb_col[r] > (row->n < VB_SHOW ? row->n : VB_SHOW)) vb_col[r] = row->n < VB_SHOW ? row->n : VB_SHOW; // (a remembered place past a shorter list; its last place is All)
    // (the chosen card kept in sight)
    float want = vb_col[r] * (VB_CW + 12) - (W - 60 - VB_CW) / 2.0f;
    if (want < 0) want = 0;
    int held = in.touching && vb_lock == 1 && in.down_y > fmaxf(ry, hh) && in.down_y < ry + VB_CH + 40;
    // (dragged across: it follows the finger; let go, it carries on and slows by itself, no snapping
    //  back; the card nearest the middle becomes the chosen one as it goes)
    static float vb_xv[VB_ROWS + 1];
    int mxc = row->n < VB_SHOW ? row->n : VB_SHOW;
    float xmax = mxc * (VB_CW + 12) - (W - 60 - VB_CW) / 2.0f; if (xmax < 0) xmax = 0;
    if (held) { vb_x[r] -= in.drag_dx; vb_xv[r] = -in.drag_dx / fmaxf(ft, 0.5f); vb_row = r; }
    else if (fabsf(vb_xv[r]) > 0.4f) {
      vb_x[r] += vb_xv[r] * ft; vb_xv[r] *= decay(0.93f);
      if (vb_x[r] < 0) { vb_x[r] = 0; vb_xv[r] = 0; } if (vb_x[r] > xmax) { vb_x[r] = xmax; vb_xv[r] = 0; }
    } else if (r == vb_row && in.pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_UP | SCE_CTRL_DOWN)) vb_xv[r] = 0;
    if (held || fabsf(vb_xv[r]) > 0.4f) { int c = (int)((vb_x[r] + (W - 60 - VB_CW) / 2.0f) / (VB_CW + 12) + 0.5f); vb_col[r] = c < 0 ? 0 : c > mxc ? mxc : c; vb_free_x[r] = 1; }
    else if (!vb_free_x[r] && (r == vb_row || vb_x[r] > want + W)) vb_x[r] += (want - vb_x[r]) * ease(0.25f);
    for (int k = 0; k < row->n && k < VB_SHOW; k++) {
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
      circle(x + 22, cy + VB_CH - 20, 14, RGBA(0, 0, 0, 150));
      vc_tri(x + 21, cy + VB_CH - 20, 7, TEXT); // (its triangle centred in the disc)
      char e[8];
      snprintf(e, sizeof e, "%s", row->p[k].ext);
      for (char *c = e; *c; c++) *c = (char)toupper((unsigned char)*c);
      rrect(x + VB_CW - text_w(11, e) - 18, cy + 8, text_w(11, e) + 12, 18, 9, RGBA(0, 0, 0, 150));
      text(x + VB_CW - text_w(11, e) - 12, cy + 21, TEXT, 11, e);
      if (is_saved(&row->p[k])) { circle(x + VB_CW - 16, cy + VB_CH - 16, 10, ACC); text_center(x + VB_CW - 16, cy + VB_CH - 11, RGBA(20, 12, 16, 255), 12, "✓"); }
      if (hit((int)x, (int)cy, VB_CW, VB_CH) && in.tap_y < H - BAR && in.tap_y > hh) { vb_row = r; vb_col[r] = k; sfx(SFX_SELECT); vb_play(r, k); return; }
    }
    // the All tile at the end: the whole row as a grid in the Browser
    {
      int k = row->n < VB_SHOW ? row->n : VB_SHOW;
      float x = 30 + k * (VB_CW + 12) - vb_x[r], cy = ry + 32;
      if (x < W && x + VB_CW > 0) {
        int on = r == vb_row && vb_col[r] == k && !in.touching;
        glass(x, cy, VB_CW, VB_CH, 0);
        circle(x + VB_CW / 2, cy + 38, 20, with_alpha(ACC, on ? 255 : 120));
        icon("grid", x + VB_CW / 2, cy + 38, on ? RGBA(20, 12, 16, 255) : TEXT);
        text_center((int)(x + VB_CW / 2), (int)cy + 80, TEXT, 16, "All");
        text_center((int)(x + VB_CW / 2), (int)cy + 98, FAINT, 11, VROWS[r].title);
        if (on) focus_ring(x, cy, VB_CW, VB_CH);
        if (hit((int)x, (int)cy, VB_CW, VB_CH) && in.tap_y < H - BAR && in.tap_y > hh) { vb_row = r; vb_col[r] = k; sfx(SFX_OPEN); vb_all_row = r; vb_all_scroll = 0; vb_all_at = frame_no; }
      }
    }
    #undef ry
  }
  // (ry: where Up next goes, below the last row shown)
  // ---- Up next: the favourite videos, after the rows ----
  if (nup) {
    const int r = VB_ROWS;
    if (ry < H && ry + VB_CH + 40 > 0) {
      text(30, ry + 20, r == vb_row ? TEXT : DIM, 18, "Up next \u00b7 your Favourites");
      float want = vb_col[r] * (VB_CW + 12) - (W - 60 - VB_CW) / 2.0f;
      if (want < 0) want = 0;
      if (in.touching && vb_lock == 1 && in.down_y > ry && in.down_y < ry + VB_CH + 40) { vb_x[r] -= in.drag_dx; vb_row = r; if (in.moved) { int c = (int)((vb_x[r] + (W - 60 - VB_CW) / 2.0f) / (VB_CW + 12) + 0.5f); vb_col[r] = c < 0 ? 0 : c >= nup ? (nup ? nup - 1 : 0) : c; } }
      else if (r == vb_row) vb_x[r] += (want - vb_x[r]) * ease(0.25f);
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
        if (hit((int)x, (int)cy, VB_CW, VB_CH) && in.tap_y < H - BAR && in.tap_y > hh) { vb_row = r; vb_col[r] = k; sfx(SFX_SELECT); upnext_play(k); return; }
      }
    }
  }
  {
    vita2d_draw_rectangle(0, 0, W, hh, RGBA(8, 6, 10, 255)); // (the rows pass under it)
    vita2d_texture *bt = hr == r0 && hk == 0 && vb_hero ? vb_hero : (hr->n && hk < VB_PER ? hr->tex[hk] : NULL);
    const float px0 = W * 0.42f, pw = W - px0;
    // (another one chosen: its picture fades in over the page, drifting in a little from the right)
    static int h_row = -1, h_k = -1; static unsigned h_at;
    if (h_row != hrow || h_k != hk) { h_row = hrow; h_k = hk; h_at = frame_no; }
    float he = fminf(1, (frame_no - h_at) / 18.0f); he = 1 - (1 - he) * (1 - he);
    if (bt) draw_cover(bt, px0 + (1 - he) * 24, y0, pw, hh, RGBA(255, 255, 255, (int)(255 * he)));
    else rrect(px0, y0, pw, hh, 0, RGBA(30, 20, 30, 255));
    grad(px0, y0, 220, hh, RGBA(8, 6, 10, 255), 255, 0, 1);       // (its left edge fading into the page)
    grad(0, y0 + hh - 60, W, 60, RGBA(8, 6, 10, 255), 0, 255, 0); // (and its foot)
    if (hr->n) { // (no source name over it: the source button says it already)
      char tt[160];
      vb_title_of(&hr->p[hk], tt, sizeof tt);
      if (!tt[0]) snprintf(tt, sizeof tt, "%s", VROWS[hrow].title);
      // (its name on up to two lines: an episode's number is at its end, and cut off it looked the same as the one before)
      { int maxw = (int)px0 - 30, fs = 27; char l1[160]; const char *rest = NULL;
        if (text_w(fs, tt) <= maxw) snprintf(l1, sizeof l1, "%s", tt);
        else { size_t cut = 0; for (size_t i = 0; tt[i]; i++) if (tt[i] == ' ') { char tr[160]; snprintf(tr, sizeof tr, "%.*s", (int)i, tt); if (text_w(fs, tr) > maxw) break; cut = i; }
               if (!cut) cut = strlen(tt); snprintf(l1, sizeof l1, "%.*s", (int)cut, tt); rest = tt[cut] ? tt + cut + 1 : NULL; }
        float ty = y0 + 110 - shrink * 0.55f - (rest ? 26 : 0);
        for (int o = 0; o < 3; o++) {
          text_fit(30 + (o == 1), ty + (o == 2), TEXT, fs, l1, maxw);
          if (rest) text_fit(30 + (o == 1), ty + 32 + (o == 2), TEXT, fs, rest, maxw);
        } }
      // its tags: glass chips, two lines at most
      { char tg[600]; snprintf(tg, sizeof tg, "%s", hr->p[hk].tags ? hr->p[hk].tags : "");
        int cx = 30, cy = (int)(y0 + 130 - shrink * 0.55f), lines = 0, maxl = shrink > 40 ? 1 : 2;
        for (char *t = strtok(tg, " "); t && lines < maxl; t = strtok(NULL, " ")) {
          for (char *c = t; *c; c++) if (*c == '_') *c = ' ';
          if (strcasestr(tt, t) || strlen(t) < 3) continue; // (a word of its name isn't a tag)
          int cw = text_w(12, t) + 20;
          if (cw > 170) continue;
          if (cx + cw > px0 + 40) { cx = 30; cy += 30; lines++; if (lines >= maxl) break; }
          rrect(cx, cy, cw, 24, 12, with_alpha(TEXT, 26));
          rrect(cx + 1, cy + 1, cw - 2, 10, 5, with_alpha(TEXT, 14));
          text(cx + 10, cy + 17, DIM, 12, t);
          cx += cw + 6;
        } }
      // Play: a pink glass button, a glow beating behind it like a heart; its triangle centred
      float pcx = 60, pcy = y0 + 222 - shrink, pr = 24;
      int pressed = in.touching && hypotf(in.tx - pcx, in.ty - pcy) < pr + 6;
      float br = 0.5f - 0.5f * cosf(frame_no * 6.2832f / 180.0f); // (a slow breath, three seconds)
      for (int g = 4; g >= 1; g--) circle(pcx, pcy, pr + g * 4, with_alpha(ACC, (int)((10 + 10 * br) / g)));
      circle(pcx, pcy + 2, pr * (pressed ? 0.9f : 1), RGBA(0, 0, 0, 90));
      circle(pcx, pcy, pr * (pressed ? 0.9f : 1), ACC);
      circle(pcx, pcy - pr * 0.3f, pr * 0.62f, RGBA(255, 255, 255, 50));
      vc_tri(pcx - 3, pcy, 12, RGBA(20, 12, 16, 255));
      text(pcx + 40, pcy + 7, TEXT, 19, "Play");
      if (hit((int)(pcx - pr - 4), (int)(pcy - pr - 4), (int)(pr * 2 + 80), (int)(pr * 2 + 8))) { sfx(SFX_SELECT); vb_play(hrow, hk); in = vb_keep; return; }
    } else if (r0->state == 3) text_fit(30, y0 + 134, ACC2, 16, r0->err, 600);
    else if (site >= 0 && LV_SITES[site].needs_key && !wkeys[site][0]) text_fit(30, y0 + 134, ACC2, 16, "Needs your account's API key: the Browser's Filter → API key", 600);
    else spinner(60, y0 + 140, 16);
  }
  // ---- the top: back, the source (its tab of the site switcher) ----
  grad(0, 0, W, 70, RGBA(8, 6, 10, 255), 210, 0, 0);
  // (no back button: this is the Browser while a video source is chosen; the source chip switches)
  vita2d_texture *si = site_icon_named(32 + vb_src, src->icon);
  char sl[64];
  snprintf(sl, sizeof sl, "%s  ▾", src->name);
  int sw = text_w(17, sl) + 64;
  rrect(16, 10, sw, 40, 20, with_alpha(TEXT, 40));
  if (si) vita2d_draw_texture_scale(si, 24, 14, 32 / 112.0f, 32 / 112.0f);
  text(64, 37, TEXT, 17, sl);
  if (hit(16, 10, sw, 40)) { sfx(SFX_OPEN); screen = S_WEB; sheet = SH_SITES; sites_tab = 1; sheet_sel = vb_src; sites_scroll = 0; return; }
  int kbw = kind_buttons(16 + sw + 10, 11, 1);
  if (screen != S_VIDEOS) { in = vb_keep; return; }
  (void)kbw;
  // ---- the buttons ----
  int last_row = nup ? VB_ROWS : VB_ROWS - 1, cur_n;
  in.pressed |= stick_dirs(); // (the left stick as the D-pad)
  if (!vb_row_shown(vb_row)) vb_row = 0;
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) vb_free = 0; // (the D-pad: the view follows the chosen row again)
  if (in.pressed & SCE_CTRL_DOWN) { int r2 = vb_row + 1; while (r2 <= last_row && !vb_row_shown(r2)) r2++; if (r2 <= last_row) { vb_row = r2; sfx(SFX_TICK); } }
  if (in.pressed & SCE_CTRL_UP) { int r2 = vb_row - 1; while (r2 > 0 && !vb_row_shown(r2)) r2--; if (r2 >= 0 && r2 != vb_row) { vb_row = r2; sfx(SFX_TICK); } }
  VRow *cur = vb_row < VB_ROWS ? &vrows[vb_row] : NULL;
  // (a row's places: its cards, up to 14, then All; Up next: its favourites)
  cur_n = cur ? (cur->state == 2 ? (cur->n < VB_SHOW ? cur->n : VB_SHOW) + 1 : 0) : nup;
  if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_LEFT)) vb_free_x[vb_row] = 0; // (the D-pad: the row follows the chosen card again)
  if (in.pressed & SCE_CTRL_RIGHT) { if (vb_col[vb_row] < cur_n - 1) { vb_col[vb_row]++; sfx(SFX_TICK); } }
  if (in.pressed & SCE_CTRL_LEFT) { if (vb_col[vb_row] > 0) { vb_col[vb_row]--; sfx(SFX_TICK); } }
  if ((in.pressed & SCE_CTRL_CROSS) && cur_n) {
    if (cur && vb_col[vb_row] >= (cur->n < VB_SHOW ? cur->n : VB_SHOW)) { sfx(SFX_OPEN); vb_all_row = vb_row; vb_all_scroll = 0; vb_all_at = frame_no; return; }
    sfx(SFX_SELECT); if (cur) vb_play(vb_row, vb_col[vb_row]); else upnext_play(vb_col[vb_row]); return;
  }
  if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_BACK); screen = S_LIBRARY; return; } // (nothing behind it: the Library)
  if (in.pressed & SCE_CTRL_START) { sfx(SFX_OPEN); screen = S_WEB; sheet = SH_SITES; sites_tab = 1; sheet_sel = vb_src; sites_scroll = 0; return; }
  // the next ones just out of sight, ready
  if (cur) for (int k = vb_col[vb_row]; k < vb_col[vb_row] + 6 && k < cur->n; k++) vb_ask(vb_row, k);
  for (int k = 0; k < 5; k++) vb_ask(0, k); // (the carousel's)
  draw_bar();
  in = vb_keep;
  if (vb_all_row >= 0) draw_vb_all();
}
// All: a window a little smaller than the screen over the page, the row's videos as a grid in it,
// scrolling by touch or the D-pad; a card plays; ✕ or ○ closes it
static void draw_vb_all(void) {
  int r = vb_all_row;
  if (r < 0 || r >= VB_ROWS) { vb_all_row = -1; return; }
  VRow *row = &vrows[r];
  float u = fminf(1, (frame_no - vb_all_at) / 14.0f), e = 1 - (1 - u) * (1 - u) * (1 - u);
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, (int)(170 * e)));
  int wx = 24, wy = 18 + (int)((1 - e) * 60), ww = W - 48, wh = H - 36;
  rrect(wx, wy + 4, ww, wh, 24, RGBA(0, 0, 0, 100));
  rrect(wx, wy, ww, wh, 24, with_alpha(SURF, 250));
  rrect(wx + 2, wy + 2, ww - 4, 56, 22, with_alpha(TEXT, 8));
  icon("grid", wx + 30, wy + 30, ACC);
  text(wx + 52, wy + 38, TEXT, 21, VROWS[r].title);
  // ✕
  circle(wx + ww - 34, wy + 30, 19, SURF2);
  icon("xmark", wx + ww - 34, wy + 30, TEXT);
  if ((in.tap && hypotf(in.tap_x - (wx + ww - 34), in.tap_y - (wy + 30)) < 26) || (in.pressed & SCE_CTRL_CIRCLE)) { sfx(SFX_CLOSE); vb_all_row = -1; in.tap = 0; in.pressed = 0; return; }
  const int cols = 4, gap = 12, cw = (ww - 40 - (cols - 1) * gap) / cols, ch = cw * 9 / 16, top = wy + 66;
  int n = row->n < VB_PER ? row->n : VB_PER, rows = (n + cols - 1) / cols;
  float vmax = rows * (ch + gap) - (wh - 76);
  if (vmax < 0) vmax = 0;
  if (in.touching && in.down_y > top) vb_all_scroll -= in.drag_dy;
  vb_all_scroll += in.ly * 12 * ft;
  if (vb_all_scroll < 0) vb_all_scroll = 0;
  if (vb_all_scroll > vmax) vb_all_scroll = vmax;
  static int sel;
  if (sel >= n) sel = 0;
  if (in.pressed & SCE_CTRL_RIGHT && sel < n - 1) sel++;
  if (in.pressed & SCE_CTRL_LEFT && sel > 0) sel--;
  if (in.pressed & SCE_CTRL_DOWN && sel + cols < n) sel += cols;
  if (in.pressed & SCE_CTRL_UP && sel - cols >= 0) sel -= cols;
  vita2d_set_clip_rectangle(wx, top, wx + ww, wy + wh - 6);
  vita2d_enable_clipping();
  for (int k = 0; k < n; k++) {
    float x = wx + 20 + (k % cols) * (cw + gap), y = top + (k / cols) * (ch + gap) - vb_all_scroll;
    if (y > wy + wh || y + ch < top) continue;
    vb_ask(r, k);
    vita2d_texture *t = row->tex[k];
    if (t) draw_cover_round(t, x, y, cw, ch, 10, RGBA(255, 255, 255, 255));
    else skeleton(x, y, cw, ch, 10);
    char tt[120];
    vb_title_of(&row->p[k], tt, sizeof tt);
    grad(x, y + ch - 40, cw, 40, RGBA(0, 0, 0, 255), 0, 190, 0);
    text_fit((int)x + 10, (int)(y + ch - 10), TEXT, 13, tt, cw - 20);
    if (k == sel && !in.touching) focus_ring(x, y, cw, ch);
    if (hit((int)x, (int)y, cw, ch) && in.tap_y > top) { sel = k; sfx(SFX_SELECT); vb_all_row = -1; vb_play(r, k); vita2d_disable_clipping(); return; }
  }
  vita2d_disable_clipping();
  if ((in.pressed & SCE_CTRL_CROSS) && n) { sfx(SFX_SELECT); vb_all_row = -1; vb_play(r, sel); return; }
  in.pressed = 0; in.tap = 0;
}
