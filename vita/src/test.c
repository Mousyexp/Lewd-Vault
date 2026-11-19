// The test kit (a separate VPK, LWDT00001 "Lewd Vault Test", built with -DLV_TESTKIT=ON): the real app,
// with a menu first that runs it through its paces by itself and writes what it measured to a
// report. Part of main.c, included after every screen (it drives them).
//
//   Open the app normally     the app as usual, a line of live numbers at the top, everything logged
//   Run every test            each of the tests below in turn, one report
//   one test                  just that one, its own report
//   Reports                   how many there are; the Wi-Fi page lists them (http://<the Vita>/reports)
//
// Reports go to ux0:data/LWDV00001/reports/. With a web address in ux0:data/LWDV00001/report_url.txt
// (a Discord webhook, or anything that takes a POST), each one is also sent there. A run that ended
// without a goodbye (a crash) is noticed at the next start: the end of its log becomes a crash report.
// The settings are kept before a test and put back after it.

#define TK_DIR DATA "/reports"
#define TK_MARKER DATA "/testkit_marker.txt"
#include <errno.h>
static char tk_boot_param[160]; // (what the app was started with: the LiveArea's frames say)

// ---------- the report being written ----------
static char *rp;
static size_t rp_n, rp_cap;
static void rpf(const char *fmt, ...) {
  char b[1024];
  va_list a;
  va_start(a, fmt);
  int n = vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  if (n <= 0) return;
  if ((size_t)n >= sizeof b) n = sizeof b - 1;
  if (rp_n + n + 1 > rp_cap) {
    size_t cap = rp_cap ? rp_cap * 2 : 16384;
    while (cap < rp_n + n + 1) cap *= 2;
    char *p = realloc(rp, cap);
    if (!p) return;
    rp = p; rp_cap = cap;
  }
  memcpy(rp + rp_n, b, n);
  rp_n += n;
  rp[rp_n] = 0;
  LOG("tk %s", b); // (the log has it too, should the report never be written)
}

// ---------- where the app is, kept on the card as it goes: what a crash leaves behind ----------
static char tk_where[160] = "starting";
static void tk_mark(const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vsnprintf(tk_where, sizeof tk_where, fmt, a);
  va_end(a);
  FILE *f = fopen(TK_MARKER, "w");
  if (f) { fprintf(f, "%s\n", tk_where); fclose(f); }
}

// ---------- measuring: each frame's work and its length, between a begin and an end ----------
#define TK_SAMPLES 4096
static struct {
  int on;
  char name[64];
  unsigned n;
  float work[TK_SAMPLES], len[TK_SAMPLES]; // (ms)
  uint64_t t0;
  int fu0, fc0, fu_min, fc_min;
  size_t heap0, heap_max;
} ms;
static size_t heap_used(void) { struct mallinfo mi = mallinfo(); return (size_t)mi.uordblks; }
static void meas_begin(const char *name) {
  memset(&ms, 0, sizeof ms);
  snprintf(ms.name, sizeof ms.name, "%s", name);
  ms.on = 1;
  ms.t0 = sceKernelGetProcessTimeWide();
  mem_free(&ms.fu0, &ms.fc0);
  ms.fu_min = ms.fu0; ms.fc_min = ms.fc0;
  ms.heap0 = ms.heap_max = heap_used();
}
static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static float pct(float *v, unsigned n, float p) { return n ? v[(unsigned)(p * (n - 1) + 0.5f)] : 0; }
static void meas_end(void) {
  if (!ms.on) return;
  ms.on = 0;
  unsigned n = ms.n < TK_SAMPLES ? ms.n : TK_SAMPLES;
  if (!n) { rpf("  %-26s no frames\n", ms.name); return; }
  double sw = 0, sl = 0;
  unsigned late = 0, very = 0;
  for (unsigned k = 0; k < n; k++) { sw += ms.work[k]; sl += ms.len[k]; late += ms.len[k] > 17.5f; very += ms.len[k] > 34; }
  qsort(ms.work, n, sizeof *ms.work, cmpf);
  qsort(ms.len, n, sizeof *ms.len, cmpf);
  float secs = (sceKernelGetProcessTimeWide() - ms.t0) / 1e6f;
  rpf("  %-26s %u frames in %.1f s = %.1f fps | frame length avg %.1f ms, p95 %.1f, p99 %.1f, max %.1f | late %u (%.1f%%), over 2 frames %u\n",
      ms.name, n, secs, n / (secs > 0 ? secs : 1), sl / n, pct(ms.len, n, 0.95f), pct(ms.len, n, 0.99f), ms.len[n - 1], late, 100.0 * late / n, very);
  // (the aim: 60 frames a second, every frame on time; held if 99% of them are)
  float on_time = 100.0f * (n - late) / n;
  rpf("  %-26s 60 fps %s: %.1f%% of frames on time (the aim: 99%%+)%s\n", "", on_time >= 99 ? "HELD" : "MISSED", on_time,
      on_time >= 99 ? "" : sw / n > 14 ? "; the work itself is too long" : "; the work is short: the drawing (GPU) or waits");
  rpf("  %-26s work avg %.2f ms, p95 %.2f, max %.2f | heap %.1f -> max %.1f MB | main memory free min %.1f MB, video %.1f MB\n", "",
      sw / n, pct(ms.work, n, 0.95f), ms.work[n - 1], ms.heap0 / 1048576.0, ms.heap_max / 1048576.0, ms.fu_min / 1048576.0, ms.fc_min / 1048576.0);
}
// (each frame, at its end: main.c)
static void meas_frame(float work_ms, float len_ms) {
  if (!ms.on) return;
  if (ms.n < TK_SAMPLES) { ms.work[ms.n] = work_ms; ms.len[ms.n] = len_ms; }
  ms.n++;
  if (ms.n % 15 == 0) {
    int fu, fc;
    if (mem_free(&fu, &fc) == 0) { if (fu < ms.fu_min) ms.fu_min = fu; if (fc < ms.fc_min) ms.fc_min = fc; }
    size_t h = heap_used();
    if (h > ms.heap_max) ms.heap_max = h;
  }
}

// ---------- the tests ----------
enum { TK_NORMAL, TK_ALL, TK_LIB, TK_VIEW, TK_WEB, TK_VIDEOS, TK_COMICS, TK_WEBTOON, TK_MEM, TK_NET, TK_CARD, TK_STRESS, TK_VIDFMT, TK_DRIVE, TK_REPORTS, TK_N };
static const char *const TK_NAME[TK_N] = {
  "Open the app normally", "Run every test", "Library: scrolling", "Full view: pictures", "Browser: loading, scrolling",
  "Video browser", "Comics: MangaDex", "Webtoon strips", "Memory: give back, come back", "Network: every source",
  "Memory card speed", "Stress: five tours", "Video formats", "Google Drive", "Reports and diagnostics",
};
static const char *const TK_ABOUT[TK_N] = {
  "The app as usual: live numbers at the top, everything logged",
  "Each test below in turn, one report (a few minutes)",
  "Scrolls the Library down and up: frame times, thumbnails",
  "Opens pictures one after another: how long each takes",
  "First page, first previews, then scrolling",
  "The video browser's rows and their pictures",
  "A list, a book, its chapters and five pages",
  "A HentaiWebtoon chapter: strips cut into pages",
  "Every part made to give its memory back, then visited again",
  "Every source asked once: answer, time, size",
  "Writes, reads and deletes 16 MB; reads vault files",
  "Five rounds of every screen: does memory creep up?",
  "H.264 in MKV and AVI, MPEG-4 AVI, VP9 WebM: plays?",
  "Favourites both ways, a report up (signed in)",
  "Diagnostics: can it write, its certificates, the LiveArea",
};
static struct {
  int active, test, phase, queue_pos;
  unsigned t;          // (frames in this phase)
  uint64_t p0;         // (when this phase began)
  int menu_sel, menu_done, hud;
  int saved_lmode;
  int n_reports, last_crash;
  char status[200];
  int k, n;            // (a loop's counter, its count)
  float times[64];
  int ntimes;
} T = {.menu_sel = 2};
static const int TK_ORDER[] = {TK_LIB, TK_VIEW, TK_WEB, TK_VIDEOS, TK_COMICS, TK_WEBTOON, TK_MEM, TK_NET, TK_CARD, TK_VIDFMT, TK_DRIVE, TK_STRESS};
#define TK_NORDER ((int)(sizeof TK_ORDER / sizeof *TK_ORDER))
static typeof(cfg) tk_cfg; // (the settings as they were: back after each test)

static void tk_next(void) { T.phase++; T.t = 0; T.p0 = sceKernelGetProcessTimeWide(); tk_mark("test %s, step %d", TK_NAME[T.test], T.phase); }
static float tk_secs(void) { return (sceKernelGetProcessTimeWide() - T.p0) / 1e6f; }
// (a wait: 1 once cond holds (the time it took written down), 2 when it hasn't by max_s)
static int tk_wait(int cond, float max_s, const char *what) {
  if (cond) { rpf("  %-26s %.2f s\n", what, tk_secs()); T.times[T.ntimes < 64 ? T.ntimes++ : 63] = tk_secs(); return 1; }
  if (tk_secs() > max_s) { rpf("  %-26s NOT within %.0f s\n", what, max_s); return 2; }
  return 0;
}
static void tk_press(unsigned b) { in.pressed |= b; in.now |= b; }

// the video formats test: the player open now, its last picture, pictures counted
static LvVideo *tk_vid;
static vita2d_texture *tk_vlast;
static int tk_vframes;
static uint64_t tk_vpos;
// network test: a thread asks each source once
typedef struct { char name[48], url[300]; int st; float ms; size_t bytes; char err[80]; } NetRes;
static NetRes *net_res;
static volatile int net_n, net_done, net_total;
static void *net_main(void *arg) {
  (void)arg;
  uint64_t t0 = sceKernelGetProcessTimeWide();
  LvHttp *h = lv_http_new();
  float start_ms = (sceKernelGetProcessTimeWide() - t0) / 1000.0f;
  LOG("tk net: started in %.0f ms\n", start_ms);
  for (int k = 0; k < net_total && h; k++) {
    NetRes *r = &net_res[k];
    uint8_t *b = NULL; size_t n = 0; const char *err = NULL;
    uint64_t a = sceKernelGetProcessTimeWide();
    LvReq rq = {0};
    r->st = lv_http_req(h, r->url, &rq, &b, &n, 512u << 10, NULL, &err);
    r->ms = (sceKernelGetProcessTimeWide() - a) / 1000.0f;
    r->bytes = n;
    if (err && *err) snprintf(r->err, sizeof r->err, "%s", err);
    free(b);
    lv_http_drop(h); // (each one from nothing: a fresh connection, as the app's first ask would be)
    net_done = k + 1;
  }
  lv_http_free(h);
  net_done = net_total;
  return NULL;
}
// memory card test: a thread writes and reads
static volatile int card_done;
static float card_w_mb, card_r_mb, vault_r_mb;
static int vault_files;
static void *card_main(void *arg) {
  (void)arg;
  const size_t CH = 256u << 10, TOTAL = 16u << 20;
  uint8_t *buf = malloc(CH);
  if (buf) {
    for (size_t k = 0; k < CH; k++) buf[k] = (uint8_t)(k * 31 + 7);
    const char *p = DATA "/tk_speed.bin";
    uint64_t a = sceKernelGetProcessTimeWide();
    SceUID fd = sceIoOpen(p, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) { for (size_t w = 0; w < TOTAL; w += CH) sceIoWrite(fd, buf, CH); sceIoClose(fd); }
    card_w_mb = TOTAL / 1048576.0f / ((sceKernelGetProcessTimeWide() - a) / 1e6f);
    a = sceKernelGetProcessTimeWide();
    fd = sceIoOpen(p, SCE_O_RDONLY, 0);
    if (fd >= 0) { while (sceIoRead(fd, buf, CH) > 0) {} sceIoClose(fd); }
    card_r_mb = TOTAL / 1048576.0f / ((sceKernelGetProcessTimeWide() - a) / 1e6f);
    sceIoRemove(p);
    // (the vault's own files: the first twenty, up to 4 MB each)
    size_t got = 0;
    a = sceKernelGetProcessTimeWide();
    for (int i = 0; i < vault.n && vault_files < 20; i++) {
      if (!vault.items[i].path) continue;
      fd = sceIoOpen(vault.items[i].path, SCE_O_RDONLY, 0);
      if (fd < 0) continue;
      int r;
      size_t one = 0;
      while (one < (4u << 20) && (r = sceIoRead(fd, buf, CH)) > 0) one += (size_t)r;
      sceIoClose(fd);
      got += one;
      vault_files++;
    }
    float s = (sceKernelGetProcessTimeWide() - a) / 1e6f;
    vault_r_mb = s > 0 ? got / 1048576.0f / s : 0;
    free(buf);
  }
  card_done = 1;
  return NULL;
}
static void tk_thread(void *(*fn)(void *)) {
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 256 * 1024);
  if (pthread_create(&t, &at, fn, NULL) == 0) pthread_detach(t);
  else fn(NULL);
}
static int count_previews(int upto) { int c = 0; for (int k = 0; k < nposts && k < upto; k++) c += wstate && wstate[k] == 2; return c; }
static int count_thumbs(int upto) { int c = 0; for (int k = 0; k < nshown && k < upto; k++) c += thumb_of(shown[k]) != NULL; return c; }
static void tk_mem_line(const char *what) {
  int fu = 0, fc = 0;
  mem_free(&fu, &fc);
  rpf("  %-26s heap %.1f MB, main memory free %.1f MB, video memory free %.1f MB\n", what, heap_used() / 1048576.0, fu / 1048576.0, fc / 1048576.0);
}
static void tk_finish_test(void);

// One frame of the test running (after the input is read, before the screen is drawn). Returns 1
// while it goes on.
static void tk_step(void) {
  T.t++;
  switch (T.test) {
    case TK_LIB:
      switch (T.phase) {
        case 0: screen = S_LIBRARY; set_mode(0); scroll = 0; scroll_v = 0; tk_next(); break;
        case 1: if (tk_wait(count_thumbs(9) >= (nshown < 9 ? nshown : 9), 15, "first thumbnails")) { meas_begin("scrolling down"); tk_next(); } break;
        case 2: in.ly = 1; if (T.t > 360) { meas_end(); meas_begin("scrolling up"); tk_next(); } break;
        case 3: in.ly = -1; if (T.t > 360) { meas_end(); rpf("  %-26s %d items, %d thumbnails held\n", "the Library", nshown, count_thumbs(nshown)); tk_finish_test(); } break;
      }
      break;
    case TK_VIEW:
      switch (T.phase) {
        case 0: screen = S_LIBRARY; set_mode(1); if (!nshown) { rpf("  no pictures in the Library\n"); tk_finish_test(); break; } open_viewer(0, 0); meas_begin("going through pictures"); T.k = 0; T.n = nshown < 15 ? nshown : 15; T.ntimes = 0; tk_next(); break;
        case 1: {
          char w[40];
          snprintf(w, sizeof w, "picture %d sharp", T.k + 1);
          int r = tk_wait(screen == S_VIEWER && big_item == shown[vk], 8, w);
          if (r) { if (++T.k >= T.n) { meas_end(); float s = 0, mx = 0; for (int i = 0; i < T.ntimes; i++) { s += T.times[i]; if (T.times[i] > mx) mx = T.times[i]; } rpf("  %-26s avg %.2f s, slowest %.2f s\n", "a picture", T.ntimes ? s / T.ntimes : 0, mx); close_viewer(); tk_finish_test(); } else { tk_press(SCE_CTRL_RIGHT); T.p0 = sceKernelGetProcessTimeWide(); } }
          break;
        }
      }
      break;
    case TK_WEB:
      switch (T.phase) {
        case 0: screen = S_WEB; wscroll = 0; tk_next(); break;
        case 1: if (tk_wait(nposts > 0, 25, "first page of results")) { if (!nposts) { tk_finish_test(); break; } T.p0 = sceKernelGetProcessTimeWide(); tk_next(); } break;
        case 2: if (tk_wait(count_previews(12) >= (nposts < 12 ? nposts : 12), 25, "first 12 previews")) { meas_begin("scrolling down"); tk_next(); } break;
        case 3: in.ly = 1; if (T.t > 420) { meas_end(); rpf("  %-26s %d results, %d previews held\n", "the Browser", nposts, count_previews(nposts)); meas_begin("scrolling up"); tk_next(); } break;
        case 4: in.ly = -1; if (T.t > 420) { meas_end(); tk_finish_test(); } break;
      }
      break;
    case TK_VIDEOS:
      switch (T.phase) {
        case 0: vb_open(0); tk_next(); break;
        case 1: if (tk_wait(vrows[0].state >= 2, 25, "first row")) tk_next(); break;
        case 2: {
          int c = 0;
          for (int k = 0; k < VB_PER && k < 4; k++) c += vrows[0].ts[k] == 2;
          if (tk_wait(c >= (vrows[0].n < 4 ? vrows[0].n : 4), 25, "first 4 pictures")) { meas_begin("going along a row"); tk_next(); }
          break;
        }
        case 3: if (T.t % 20 == 1) tk_press(SCE_CTRL_RIGHT); if (T.t > 240) { meas_end(); meas_begin("going down the rows"); tk_next(); } break;
        case 4: if (T.t % 30 == 1) tk_press(SCE_CTRL_DOWN); if (T.t > 180) { meas_end(); screen = S_WEB; tk_finish_test(); } break;
      }
      break;
    case TK_COMICS:
    case TK_WEBTOON: {
      int src = T.test == TK_COMICS ? MS_MANGADEX : MS_HWEBTOON;
      switch (T.phase) {
        case 0: strip_stats_reset(); manga_open(src); tk_next(); break;
        case 1: if (tk_wait(mg.nbooks > 0 && !mg.loading, 25, "list")) { if (!mg.nbooks) { tk_finish_test(); break; } T.p0 = sceKernelGetProcessTimeWide(); tk_next(); } break;
        case 2: {
          int c = 0;
          for (int k = 0; k < mg.nbooks && k < 12; k++) c += mg.cstate && mg.cstate[k] == 2;
          if (tk_wait(c >= (mg.nbooks < 6 ? mg.nbooks : 6), 20, "first 6 covers")) { mg_open_book(0); tk_next(); }
          break;
        }
        case 3: if (tk_wait(!mg_chapters(mg.src) || (!mg.ch_loading && mg.nch > 0), 25, "chapters")) { mg_read(0); meas_begin("reading"); tk_next(); } break;
        case 4: if (tk_wait(mg.npages > 0 && mg.ptex[0] && mg.ptex_page[0] == mg.page, 30, "first page")) { T.k = 0; T.n = T.test == TK_WEBTOON ? 12 : 5; T.p0 = sceKernelGetProcessTimeWide(); tk_next(); } break;
        case 5: {
          if (T.t == 1) { if (mg.page + 1 >= mg.npages) { T.k = T.n; } else mg_rd_go(mg.page + 1); }
          char w[40];
          snprintf(w, sizeof w, "page %d", mg.page + 1);
          // (the page's time written down once it's there; then a second on it, as a quick reader
          //  would, before the next: what's done ahead of the reader gets its chance)
          static int shown_at = -1;
          int r = T.k >= T.n ? 1 : shown_at >= 0 ? 0 : tk_wait(mg.ptex[0] && mg.ptex_page[0] == mg.page, 20, w);
          if (r && T.k < T.n && T.test == TK_WEBTOON && shown_at < 0) { shown_at = (int)T.t; r = 0; }
          if (shown_at >= 0 && (int)T.t - shown_at >= 60) { shown_at = -1; r = 1; }
          if (r) {
            if (++T.k >= T.n) {
              meas_end();
              if (T.test == TK_WEBTOON) rpf("  %-26s %d cut, %.0f ms each on average, slowest %.0f ms, coefficients held at most %u KB\n", "strips",
                                            strip_stats.cuts, strip_stats.cuts ? strip_stats.ms / strip_stats.cuts : 0.0f, strip_stats.ms_max, strip_stats.store_kb);
              rpf("  %-26s %d pages in the chapter\n", "the chapter", mg.npages);
              mg_rd_close(); screen = S_WEB;
              tk_finish_test();
            } else { T.t = 0; T.p0 = sceKernelGetProcessTimeWide(); }
          }
          break;
        }
      }
      break;
    }
    case TK_MEM:
      switch (T.phase) {
        case 0: screen = S_LIBRARY; tk_mem_line("at the start"); tk_released = 0; tk_next(); break;
        case 1: if (T.t == 1) screen = S_WEB; if (T.t > 300) { tk_mem_line("in the Browser"); manga_open(MS_MANGADEX); tk_next(); } break;
        case 2: if (T.t > 300) { tk_mem_line("in Comics"); test_lowmem = 1; screen = S_LIBRARY; tk_next(); } break;
        case 3: if (T.t > 120) { tk_mem_line("Library, memory short"); rpf("  %-26s %u parts gave their memory back\n", "", tk_released); test_lowmem = 0; screen = S_WEB; tk_next(); } break;
        case 4: if (tk_wait(count_previews(12) >= (nposts < 12 ? nposts : 12), 25, "Browser pictures again")) { tk_mem_line("back in the Browser"); screen = S_LIBRARY; tk_next(); } break;
        case 5: if (T.t > 120) { tk_mem_line("Library at the end"); tk_finish_test(); } break;
      }
      break;
    case TK_NET:
      switch (T.phase) {
        case 0: {
          free(net_res);
          net_total = 0;
          net_res = calloc(64 + MS_N, sizeof *net_res);
          if (!net_res) { tk_finish_test(); break; }
          for (int k = 0; k < LV_NSITES && net_total < 64; k++) {
            int vsite = LV_SITES[k].kind == BK_KVS || LV_SITES[k].kind == BK_XV || LV_SITES[k].kind == BK_HWORLD || LV_SITES[k].kind == BK_HCLOUD || LV_SITES[k].kind == BK_HCITY;
            if ((LV_SITES[k].hidden && !vsite) || !LV_SITES[k].base) continue; // (the video browser's sites too)
            snprintf(net_res[net_total].name, sizeof net_res[0].name, "%s", LV_SITES[k].name);
            snprintf(net_res[net_total].url, sizeof net_res[0].url, "%s/", LV_SITES[k].base);
            net_total++;
          }
          for (int k = 0; k < MS_N; k++) {
            snprintf(net_res[net_total].name, sizeof net_res[0].name, "%s (comics)", LV_MSRC[k].name);
            snprintf(net_res[net_total].url, sizeof net_res[0].url, "%s/", k == MS_MANGADEX ? "https://api.mangadex.org/ping" : LV_MSRC[k].base);
            if (k == MS_MANGADEX) net_res[net_total].url[strlen(net_res[net_total].url) - 1] = 0;
            if (k == MS_EHENTAI) snprintf(net_res[net_total].url, sizeof net_res[0].url, "https://e-hentai.org/");
            if (net_res[net_total].url[0] == '/') continue;
            net_total++;
          }
          net_done = 0;
          tk_thread(net_main);
          tk_next();
          break;
        }
        case 1: {
          snprintf(T.status, sizeof T.status, "Asking %d of %d…", net_done + 1, net_total);
          if (net_done >= net_total || tk_secs() > 240) {
            int ok = 0;
            for (int k = 0; k < net_done; k++) {
              NetRes *r = &net_res[k];
              ok += r->st >= 200 && r->st < 400;
              rpf("  %-26s %3d  %6.0f ms  %7lu bytes  %s\n", r->name, r->st, r->ms, (unsigned long)r->bytes, r->err);
            }
            rpf("  %-26s %d of %d answered\n", "in all", ok, net_total);
            tk_finish_test();
          }
          break;
        }
      }
      break;
    case TK_CARD:
      switch (T.phase) {
        case 0: card_done = 0; vault_files = 0; tk_thread(card_main); tk_next(); break;
        case 1:
          snprintf(T.status, sizeof T.status, "Writing and reading…");
          if (card_done) {
            rpf("  %-26s %.1f MB/s\n  %-26s %.1f MB/s\n  %-26s %.1f MB/s (%d files)\n", "write", card_w_mb, "read", card_r_mb, "vault files", vault_r_mb, vault_files);
            tk_finish_test();
          }
          break;
      }
      break;
    case TK_VIDFMT: {
      // each sample: opened, its first picture, then how many new pictures in 3 s
      static const char *const CLIP[] = {"h264.mkv", "h264.avi", "mpeg4.avi", "vp9.webm"};
      static const char *const WHAT[] = {"H.264 in MKV (hardware)", "H.264 in AVI (hardware)", "MPEG-4 in AVI (software)", "VP9 in WebM (software)"};
      int c = T.phase / 3, step = T.phase % 3;
      if (c >= 4) { tk_finish_test(); break; }
      if (step == 0) {
        char p[96];
        snprintf(p, sizeof p, "app0:testclips/%s", CLIP[c]);
        if (vid) stop_video();
        tk_vid = lv_video_open(p, NULL, 1);
        tk_vframes = 0; tk_vlast = NULL; tk_vpos = 0;
        if (!tk_vid) { rpf("  %-26s did NOT open\n", WHAT[c]); T.phase += 2; }
        tk_next();
        break;
      }
      vita2d_texture *f = tk_vid ? lv_video_frame(tk_vid) : NULL;
      uint64_t pos = tk_vid ? lv_video_pos(tk_vid) : 0;
      if (f && pos != tk_vpos) { tk_vframes++; tk_vpos = pos; }
      tk_vlast = f;
      if (step == 1) { if (tk_wait(f != NULL, 8, WHAT[c])) { tk_vframes = 0; tk_next(); } if (!f && tk_secs() > 8) { lv_video_close(tk_vid); tk_vid = NULL; tk_vlast = NULL; } }
      else if (step == 2 && tk_secs() >= 3) {
        rpf("  %-26s %dx%d, %.0f pictures a second\n", "", lv_video_width(tk_vid), lv_video_height(tk_vid), tk_vframes / 3.0f);
        lv_video_close(tk_vid); tk_vid = NULL; tk_vlast = NULL;
        tk_next();
      }
      break;
    }
    case TK_DRIVE: {
      // the favourites with Drive, then a small report up (the Vita's own folder, "Lewd Vault (Vita)")
      static int runs0, fails0;
      if (T.phase == 0) {
        if (!drive_signed_in()) { rpf("  not signed in to Google Drive (Settings): skipped\n"); tk_finish_test(); break; }
        if (T.t < 2) break;
        if (drive_side_busy || drive.state == DRV_SYNCING) { if (tk_secs() > 60) { rpf("  Drive stayed busy: skipped\n"); tk_finish_test(); } break; }
        runs0 = drive_favs_runs; fails0 = drive_favs_fails;
        if (drive_favs_sync() != 0) { rpf("  favourites: the sync didn't start\n"); T.phase = 1; }
        tk_next();
      } else if (T.phase == 1) {
        if (drive_favs_runs == runs0 && tk_secs() < 60) break;
        if (drive_favs_runs == runs0) rpf("  favourites: no answer in 60 s\n");
        else if (drive_favs_fails != fails0) rpf("  favourites: FAILED: %s\n", drive_favs_msg);
        else rpf("  favourites: in step, %.1f s (%d new from Drive, %d taken out)\n", tk_secs(), drive_favs_added, drive_favs_removed);
        tk_next();
      } else if (T.phase == 2) {
        if (drive_side_busy) break;
        FILE *f = fopen(TK_DIR "/drive-check.txt", "wb");
        if (f) { fputs("Lewd Vault test kit: a report up to Drive works.\n", f); fclose(f); }
        if (drive_send_report(TK_DIR "/drive-check.txt", "drive-check.txt") != 0) { rpf("  report: the upload didn't start\n"); tk_finish_test(); break; }
        tk_next();
      } else if (T.phase == 3) {
        if (drive_report_state == 1 && tk_secs() < 60) break;
        rpf("  report: %s, %.1f s\n", drive_report_state == 2 ? "up in Lewd Vault (Vita)/Test reports" : "FAILED (log.txt says why)", tk_secs());
        sceIoRemove(TK_DIR "/drive-check.txt");
        tk_finish_test();
      }
      break;
    }
    case TK_STRESS: {
      // each round: Library, a picture, Browser, Comics, Settings, Library; memory written down after each
      static const int SCR[] = {S_LIBRARY, S_VIEWER, S_WEB, S_MANGA, S_SETTINGS, S_LIBRARY};
      int round = T.phase / 6, step = T.phase % 6;
      if (round >= 5) { meas_end(); tk_finish_test(); break; }
      if (T.phase == 0 && T.t == 1) meas_begin("five tours");
      if (T.t == 1) {
        int s = SCR[step];
        if (s == S_VIEWER) { screen = S_LIBRARY; set_mode(1); if (nshown) open_viewer(round % nshown, 0); }
        else if (s == S_MANGA) manga_open(MS_MANGADEX);
        else { if (screen == S_VIEWER) close_viewer(); screen = s; }
      }
      if (step == 2 || step == 3) in.ly = 1; // (scrolled, so it has things to fetch)
      if (T.t > 150) {
        if (step == 5) { char w[40]; snprintf(w, sizeof w, "after round %d", round + 1); tk_mem_line(w); }
        tk_next();
      }
      break;
    }
  }
}

// ---------- reports: written, kept, sent ----------
static void tk_device(void) {
  char when[40];
  now_iso(when, sizeof when);
  SceKernelSystemSwVersion v;
  memset(&v, 0, sizeof v);
  v.size = sizeof v;
  sceKernelGetSystemSwVersion(&v);
  int fu = 0, fc = 0;
  mem_free(&fu, &fc);
  rpf("Lewd Vault test kit report\n");
  int model = sceKernelGetModel();
  rpf("when       %s\nbuilt      %s %s\nfirmware   %s\nmodel      %s (0x%x)\n", when, __DATE__, __TIME__, v.versionString[0] ? v.versionString : "(not said)",
      model == 0x10000 ? "PS Vita" : model == 0x20000 ? "PS TV" : "not known", model);
  rpf("clocks     CPU %d MHz, GPU %d MHz, bus %d MHz\nbattery    %d%%%s\n", scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency(),
      scePowerGetBusClockFrequency(), scePowerGetBatteryLifePercent(), scePowerIsBatteryCharging() ? " (charging)" : "");
  rpf("memory     heap %.1f MB used of %d MB, main free %.1f MB, video free %.1f MB\n", heap_used() / 1048576.0, _newlib_heap_size_user >> 20, fu / 1048576.0, fc / 1048576.0);
  rpf("vault      %s, %d items\nnetwork    %s\n\n", loaded_vault ? vault.root : "(none)", vault.n, lv_net_online() ? "online" : "offline");
}
// (the log's last lines, for the report's end)
static void tk_log_tail(const char *path, int lines) {
  FILE *f = fopen(path, "rb");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  long n = ftell(f), from = n > 48000 ? n - 48000 : 0;
  fseek(f, from, SEEK_SET);
  char *b = malloc(48001);
  if (!b) { fclose(f); return; }
  size_t got = fread(b, 1, 48000, f);
  fclose(f);
  b[got] = 0;
  const char *p = b + got;
  int k = 0;
  while (p > b && k <= lines) { p--; if (*p == '\n') k++; }
  rpf("\n== the log's last %d lines (%s) ==\n", lines, path);
  // (written as it is: in pieces, rpf's buffer is 1 KB)
  for (const char *q = p; *q;) { size_t l = strcspn(q, "\n"); rpf("%.*s\n", (int)(l > 1000 ? 1000 : l), q); q += l; if (*q) q++; }
  free(b);
}
static char tk_url[512];
static char *tk_send_body; static size_t tk_send_n; static char tk_send_name[96];
static void *tk_send_main(void *arg) {
  (void)arg;
  LvHttp *h = lv_http_new();
  if (!h) return NULL;
  uint8_t *out = NULL; size_t n = 0; const char *err = NULL;
  int st;
  if (strstr(tk_url, "discord.com/api/webhooks") || strstr(tk_url, "discordapp.com/api/webhooks")) {
    // (Discord: the report as a file in a multipart message)
    static const char *B = "lvtkboundary7MA4YWxkTrZu0gW";
    size_t cap = tk_send_n + 1024;
    char *body = malloc(cap);
    if (!body) { lv_http_free(h); return NULL; }
    int a = snprintf(body, cap, "--%s\r\nContent-Disposition: form-data; name=\"content\"\r\n\r\nLewd Vault test kit: %s\r\n--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\nContent-Type: text/plain\r\n\r\n", B, tk_send_name, B, tk_send_name);
    memcpy(body + a, tk_send_body, tk_send_n);
    size_t total = a + tk_send_n;
    total += snprintf(body + total, cap - total, "\r\n--%s--\r\n", B);
    char ct[96];
    snprintf(ct, sizeof ct, "Content-Type: multipart/form-data; boundary=%s", B);
    const char *hd[] = {ct, NULL};
    LvReq rq = {.method = "POST", .headers = hd, .body = body, .body_len = total};
    st = lv_http_req(h, tk_url, &rq, &out, &n, 1u << 20, NULL, &err);
    free(body);
  } else {
    const char *hd[] = {"Content-Type: text/plain; charset=utf-8", NULL};
    LvReq rq = {.method = "POST", .headers = hd, .body = tk_send_body, .body_len = tk_send_n};
    st = lv_http_req(h, tk_url, &rq, &out, &n, 1u << 20, NULL, &err);
  }
  LOG("tk send %s -> %d %s\n", tk_send_name, st, err ? err : "");
  free(out);
  free(tk_send_body); tk_send_body = NULL;
  lv_http_free(h);
  return NULL;
}
static void tk_send(const char *name, const char *text, size_t n) {
  if (!tk_url[0] || tk_send_body) return; // (no address; or one still going)
  tk_send_body = malloc(n + 1);
  if (!tk_send_body) return;
  memcpy(tk_send_body, text, n);
  tk_send_n = n;
  snprintf(tk_send_name, sizeof tk_send_name, "%s", name);
  tk_thread(tk_send_main);
}
static void tk_count_reports(void) {
  T.n_reports = 0;
  SceUID d = sceIoDopen(TK_DIR);
  if (d < 0) return;
  SceIoDirent e;
  memset(&e, 0, sizeof e);
  while (sceIoDread(d, &e) > 0) T.n_reports += strstr(e.d_name, ".txt") != NULL;
  sceIoDclose(d);
}
static char tk_drive_path[200], tk_drive_name[96]; // (a report waiting for Drive to be free)
static void tk_drive_pump(void) {
  if (tk_drive_name[0] && !drive_side_busy && drive.state != DRV_SYNCING && drive.state != DRV_CODE && drive_send_report(tk_drive_path, tk_drive_name) == 0) tk_drive_name[0] = 0;
}
static void tk_save_report(const char *kind) {
  if (!rp) return;
  tk_log_tail(DATA "/log.txt", 120);
  sceIoMkdir(TK_DIR, 0777);
  time_t t = time(NULL);
  struct tm g;
  gmtime_r(&t, &g); // (UTC: the names sort by time)
  char name[96], path[200];
  snprintf(name, sizeof name, "%s-%04d%02d%02d-%02d%02d%02d.txt", kind, g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
  snprintf(path, sizeof path, TK_DIR "/%s", name);
  // (written with the Vita's own calls, and checked: it said "saved" whether it was or not)
  SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  int wrote = fd >= 0 ? sceIoWrite(fd, rp, (SceSize)rp_n) : (int)fd;
  if (fd >= 0) sceIoClose(fd);
  int saved_ok = fd >= 0 && wrote == (int)rp_n;
  LOG("tk report saved: %s (%lu bytes) -> %08x\n", path, (unsigned long)rp_n, (unsigned)wrote);
  tk_send(name, rp, rp_n);
  // (signed in to Drive: up to "Lewd Vault (Vita)/Test reports" too, once Drive is free)
  if (drive_signed_in()) { snprintf(tk_drive_path, sizeof tk_drive_path, "%s", path); snprintf(tk_drive_name, sizeof tk_drive_name, "%s", name); }
  if (saved_ok) snprintf(T.status, sizeof T.status, "Report saved: %s/%s%s%s", TK_DIR, name, tk_url[0] ? " (and sent)" : "", drive_signed_in() ? " · to Drive" : "");
  else snprintf(T.status, sizeof T.status, "The report COULDN'T be written (%08x): see Reports and diagnostics", (unsigned)wrote);
  free(rp); rp = NULL; rp_n = rp_cap = 0;
  tk_count_reports();
}

// ---------- diagnostics: on the screen, so they can be read even when nothing can be written ----------
// (Reports in the menu: the data folder written to both ways the app writes, the reports folder,
//  the log, the certificates, how the app was started and the last LiveArea event)
static char tk_diag[16][160];
static int tk_ndiag, tk_diag_open;
static void tk_diag_add(const char *fmt, ...) {
  if (tk_ndiag >= 16) return;
  va_list a;
  va_start(a, fmt);
  vsnprintf(tk_diag[tk_ndiag], sizeof tk_diag[0], fmt, a);
  va_end(a);
  LOG("diag: %s\n", tk_diag[tk_ndiag]);
  tk_ndiag++;
}
static void tk_diag_run(void) {
  tk_ndiag = 0;
  SceIoStat st;
  int r = sceIoMkdir(DATA, 0777);
  tk_diag_add("data folder %s: %s (mkdir %08x)", DATA, sceIoGetstat(DATA, &st) >= 0 ? "there" : "NOT THERE", (unsigned)r);
  // (written with the Vita's own calls)
  SceUID fd = sceIoOpen(DATA "/diag_sceio.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  int wr = fd >= 0 ? sceIoWrite(fd, "ok\n", 3) : -1;
  if (fd >= 0) sceIoClose(fd);
  int sz = sceIoGetstat(DATA "/diag_sceio.txt", &st) >= 0 ? (int)st.st_size : -1;
  tk_diag_add("write (sceIo): open %08x, wrote %d, on the card %d bytes", (unsigned)fd, wr, sz);
  sceIoRemove(DATA "/diag_sceio.txt");
  // (written with C's stdio, as the reports are)
  errno = 0;
  FILE *f = fopen(DATA "/diag_stdio.txt", "wb");
  int fe = errno;
  size_t fw = f ? fwrite("ok\n", 1, 3, f) : 0;
  int fc = f ? fclose(f) : -1;
  sz = sceIoGetstat(DATA "/diag_stdio.txt", &st) >= 0 ? (int)st.st_size : -1;
  tk_diag_add("write (stdio): %s (errno %d), wrote %d, closed %d, on the card %d bytes", f ? "opened" : "NOT OPENED", fe, (int)fw, fc, sz);
  sceIoRemove(DATA "/diag_stdio.txt");
  r = sceIoMkdir(TK_DIR, 0777);
  tk_diag_add("reports folder: %s (mkdir %08x), %d reports", sceIoGetstat(TK_DIR, &st) >= 0 ? "there" : "NOT THERE", (unsigned)r, T.n_reports);
  tk_diag_add("log.txt: %s", sceIoGetstat(DATA "/log.txt", &st) >= 0 ? "there" : "NOT THERE");
  if (sceIoGetstat(DATA "/log.txt", &st) >= 0) tk_diag_add("log.txt: %d bytes", (int)st.st_size);
  tk_diag_add("%s", lv_net_ca_status());
  tk_diag_add("started with: %s", tk_boot_param[0] ? tk_boot_param : "(nothing: the bubble)");
  tk_diag_add("LiveArea events: %d, the last: %s", la_events, la_last_event[0] ? la_last_event : "(none)");
  { int fu = 0, fc = 0; mem_free(&fu, &fc); tk_diag_add("memory: heap %.1f MB used, main free %.1f MB, video free %.1f MB", heap_used() / 1048576.0, fu / 1048576.0, fc / 1048576.0); }
}
static void tk_diag_draw(void) {
  vita2d_draw_rectangle(0, 0, W, H, BG);
  text(30, 44, TEXT, 26, "Diagnostics");
  text(30, 70, FAINT, 14, "What the app could and couldn't do on this Vita (a photo of this is a report too)  ·  ○ back  ✕ again");
  for (int k = 0; k < tk_ndiag; k++) {
    unsigned c = strstr(tk_diag[k], "NOT") || strstr(tk_diag[k], " not ") || strstr(tk_diag[k], "couldn't") ? ACC2 : TEXT;
    text_fit(30, 110 + k * 28, c, 16, tk_diag[k], W - 60);
  }
  if (in.pressed & SCE_CTRL_CROSS) { sfx(SFX_SELECT); tk_diag_run(); }
  if ((in.pressed & SCE_CTRL_CIRCLE) || in.tap) { sfx(SFX_BACK); tk_diag_open = 0; }
  in.pressed = 0; in.tap = 0;
}

// ---------- starting and ending ----------
static void tk_start(int test) {
  T.test = test; T.phase = -1; T.t = 0; T.ntimes = 0; T.status[0] = 0;
  if (test == TK_NORMAL) { T.active = 0; T.hud = 1; screen = S_LIBRARY; tk_mark("normal run"); LOG("tk: the app, normally\n"); return; }
  if (test == TK_REPORTS) {
    tk_count_reports();
    tk_diag_run();
    tk_diag_open = 1;
    snprintf(T.status, sizeof T.status, "%d reports in ux0:data/LWDV00001/reports  ·  Wi-Fi page: /reports  ·  %s", T.n_reports,
             tk_url[0] ? "sent to the address in report_url.txt" : "report_url.txt: an address to send them to");
    return;
  }
  tk_cfg = cfg; // (the settings, to put back)
  if (!rp) tk_device();
  if (test == TK_ALL) { T.queue_pos = 0; T.test = TK_ORDER[0]; }
  else T.queue_pos = -1;
  rpf("== %s ==\n", TK_NAME[T.test]);
  T.active = 1;
  tk_next();
  T.phase = 0;
}
static void tk_finish_test(void) {
  meas_end();
  rpf("\n");
  if (T.queue_pos >= 0 && T.queue_pos + 1 < TK_NORDER) {
    T.queue_pos++;
    T.test = TK_ORDER[T.queue_pos];
    rpf("== %s ==\n", TK_NAME[T.test]);
    T.phase = 0; T.t = 0; T.p0 = sceKernelGetProcessTimeWide();
    return;
  }
  T.active = 0;
  test_lowmem = 0;
  if (screen == S_VIEWER) close_viewer();
  // (the settings as they were)
  int lm = tk_cfg.lmode;
  cfg = tk_cfg;
  cfg.lmode = -1; set_mode(lm);
  cfg_save();
  tk_save_report(T.queue_pos >= 0 ? "all" : "test");
  tk_mark("menu");
  screen = S_TEST;
}

// ---------- at the start: the last run's end ----------
static void tk_boot(void) {
  sceIoMkdir(TK_DIR, 0777);
  FILE *f = fopen(DATA "/report_url.txt", "r");
  if (f) { if (fgets(tk_url, sizeof tk_url, f)) tk_url[strcspn(tk_url, "\r\n \t")] = 0; fclose(f); }
  // the last run's log kept, before this run's starts
  sceIoRemove(DATA "/log-prev.txt");
  sceIoRename(DATA "/log.txt", DATA "/log-prev.txt");
  char was[200] = "";
  f = fopen(TK_MARKER, "r");
  if (f) { if (fgets(was, sizeof was, f)) was[strcspn(was, "\r\n")] = 0; fclose(f); }
  if (was[0] && strcmp(was, "menu") && strncmp(was, "put away", 8)) {
    // (it was somewhere, and never said it was put away: it ended there)
    T.last_crash = 1;
    rpf("Lewd Vault test kit CRASH REPORT\nThe last run ended without a goodbye while: %s\n\n", was);
    // (the system's own crash dumps, if there are any)
    SceUID d = sceIoDopen("ux0:data");
    if (d >= 0) {
      SceIoDirent e;
      memset(&e, 0, sizeof e);
      while (sceIoDread(d, &e) > 0) if (strstr(e.d_name, ".psp2dmp")) rpf("system crash dump: ux0:data/%s, %lld bytes\n", e.d_name, (long long)e.d_stat.st_size);
      sceIoDclose(d);
    }
    tk_log_tail(DATA "/log-prev.txt", 200);
    snprintf(T.status, sizeof T.status, "The last run ended while: %s. A crash report was saved.", was);
    char status[200];
    snprintf(status, sizeof status, "%s", T.status);
    tk_save_report("crash");
    snprintf(T.status, sizeof T.status, "%s", status);
  }
  tk_mark("starting");
  tk_count_reports();
}

// ---------- the menu ----------
static void draw_test_menu(void) {
  if (tk_diag_open) { tk_diag_draw(); return; }
  vita2d_draw_rectangle(0, 0, W, H, BG);
  text(30, 44, TEXT, 26, "Lewd Vault test kit");
  text(30, 70, FAINT, 14, "Runs the app through its paces and writes what it measured to ux0:data/LWDV00001/reports");
  const int cols = 3, tw = 290, th = 76, gx = 30, gy = 92, gap = 10;
  for (int k = 0; k < TK_N; k++) {
    int x = gx + (k % cols) * (tw + gap), y = gy + (k / cols) * (th + gap);
    int on = k == T.menu_sel;
    rrect(x, y, tw, th, 16, on ? SURF2 : with_alpha(SURF2, 150));
    if (k == TK_ALL || k == TK_NORMAL) rrect(x, y, 6, th, 3, ACC);
    text_fit(x + 16, y + 30, TEXT, 17, TK_NAME[k], tw - 28);
    text_fit(x + 16, y + 54, FAINT, 13, TK_ABOUT[k], tw - 28);
    if (on) focus_ring(x, y, tw, th);
    if (hit(x, y, tw, th)) { T.menu_sel = k; sfx(SFX_SELECT); tk_start(k); return; }
  }
  if (T.status[0]) text_fit(30, H - 16, T.last_crash ? ACC2 : ACC, 15, T.status, W - 60);
  if (in.pressed & SCE_CTRL_RIGHT) T.menu_sel = (T.menu_sel + 1) % TK_N;
  if (in.pressed & SCE_CTRL_LEFT) T.menu_sel = (T.menu_sel + TK_N - 1) % TK_N;
  if (in.pressed & SCE_CTRL_DOWN) T.menu_sel = T.menu_sel + cols < TK_N ? T.menu_sel + cols : T.menu_sel % cols;
  if (in.pressed & SCE_CTRL_UP) T.menu_sel = T.menu_sel - cols >= 0 ? T.menu_sel - cols : T.menu_sel;
  if (in.pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) sfx(SFX_TICK);
  if (in.pressed & (SCE_CTRL_CROSS | SCE_CTRL_START)) { sfx(SFX_SELECT); tk_start(T.menu_sel); }
}

// (the PS button: the app put away, not ended; noted so a close from the LiveArea isn't a crash, and
//  what it was doing written back once it's woken)
static char tk_before_away[160];
static int tk_away;
static void tk_put_away(void) {
  if (tk_away) return;
  snprintf(tk_before_away, sizeof tk_before_away, "%s", tk_where);
  tk_mark("put away (the PS button) during: %s", tk_before_away);
  tk_away = 1;
}
// ---------- each frame: main.c calls these ----------
// (after the input is read: the menu first, a test's step, the input logged)
static void tk_frame_input(void) {
  // (once, at the start; started from a LiveArea frame: the app as usual, that vault open)
  if (!T.menu_done && screen == S_LIBRARY && loaded_vault) {
    T.menu_done = 1;
    if (la_from_frame) { T.hud = 1; tk_mark("normal run"); LOG("tk: started from a LiveArea frame (%d): the app, normally\n", la_from_frame); }
    else { screen = S_TEST; tk_mark("menu"); }
  }
  if (T.active) {
    // (the person can stop a test: ○ and △ together)
    if ((in.now & SCE_CTRL_CIRCLE) && (in.now & SCE_CTRL_TRIANGLE)) { rpf("  stopped by hand\n"); T.queue_pos = -1; tk_finish_test(); return; }
    in.pressed = 0; in.tap = 0; in.touching = 0; in.dbl = 0; in.lx = in.ly = 0; // (the test's own input only)
    tk_step();
  } else if (in.pressed || in.tap) {
    LOG("in screen=%d pressed=%x tap=%d (%d,%d)\n", screen, in.pressed, in.tap, in.tap_x, in.tap_y);
  }
}
// (at the frame's end: measured, the screen noted, the numbers drawn on top)
static void tk_frame_end(float work_ms, float len_ms) {
  if (frame_no % 60 == 0) tk_drive_pump();
  if (tk_away && len_ms > 1000) { tk_away = 0; tk_mark("%s", tk_before_away); LOG("tk: woken after %.1f s\n", len_ms / 1000); } // (back from the LiveArea)
  else if (len_ms > 1000) len_ms = 1000; // (a sleep: one long frame, not a measure)
  meas_frame(work_ms, len_ms);
  static int last_screen = -1;
  if (screen != last_screen && !T.active && T.menu_done && screen != S_TEST) { tk_mark("screen %d", screen); last_screen = screen; }
  if (T.active && tk_vlast) { // (the video formats test: its picture, fitted)
    float tw = lv_video_width(tk_vid), th = lv_video_height(tk_vid);
    if (tw > 0 && th > 0) { float sc = fminf(W / tw, H / th); vita2d_draw_texture_scale(tk_vlast, (W - tw * sc) / 2, (H - th * sc) / 2, sc, sc); }
  }
  if (T.active || T.hud) {
    static float avg_len = 16.7f, avg_work;
    avg_len += (len_ms - avg_len) * 0.05f;
    avg_work += (work_ms - avg_work) * 0.05f;
    int fu = 0, fc = 0;
    static int hfu, hfc;
    if (frame_no % 30 == 0 && mem_free(&fu, &fc) == 0) { hfu = fu; hfc = fc; }
    char b[200];
    snprintf(b, sizeof b, "%.0f fps  ·  work %.1f ms  ·  heap %.1f MB  ·  RAM %d MB  ·  VRAM %d MB%s%s", 1000.0f / (avg_len > 1 ? avg_len : 1), avg_work,
             heap_used() / 1048576.0, hfu >> 20, hfc >> 20, T.active ? "  ·  " : "", T.active ? TK_NAME[T.test] : "");
    int w = text_w(12, b) + 16;
    rrect(W / 2 - w / 2, 2, w, 18, 6, RGBA(0, 0, 0, 170));
    text_center(W / 2, 15, avg_len > 17.2f ? ACC2 : T.active ? ACC : TEXT, 12, b); // (red under 58 fps: below the aim)
    if (T.active && T.status[0]) text_center(W / 2, 34, DIM, 12, T.status);
  }
}
