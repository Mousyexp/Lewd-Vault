#define _GNU_SOURCE // (strcasestr)
#include "booru.h"
#include "json.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// No account needed for these, but Gelbooru and Rule34 (the last two) want an account's API key.
LvSite LV_SITES[LV_MAX_SITES] = {
  {"Danbooru", BK_DANBOORU, "https://danbooru.donmai.us", NULL, 1, 0, "danbooru", "Anime art, finely tagged"},
  {"Yande.re", BK_MOEBOORU, "https://yande.re", NULL, 0, 0, "yandere", "High-resolution anime scans"},
  {"Konachan", BK_MOEBOORU, "https://konachan.com", NULL, 0, 0, "konachan", "Anime wallpapers"},
  {"Xbooru Comics", BK_GELBOORU, "https://xbooru.com", NULL, 0, 0, "xcomics", "Hentai comic pages", "comic"},
  {"Xbooru", BK_GELBOORU, "https://xbooru.com", NULL, 0, 0, "xbooru", "Hentai"},
  // (TBIB and Rule34: much western and 3D: not offered, the app being about anime and hentai)
  {"TBIB", BK_GELBOORU, "https://tbib.org", NULL, 0, 0, "tbib", "The big imageboard", NULL, 1},
  {"Hypnohub", BK_GELBOORU, "https://hypnohub.net", NULL, 0, 0, "hypnohub", "Hypnosis art"},
  // (AIBooru, AI-made pictures, isn't offered: its place is kept, saved settings count sites by place)
  {"AIBooru", BK_DANBOORU, "https://aibooru.online", NULL, 1, 0, "aibooru", "AI-made anime art", NULL, 1},
  {"Safebooru", BK_GELBOORU, "https://safebooru.org", NULL, 0, 0, "safebooru", "Anime art, all ages"},
  {"Danbooru Comics", BK_DANBOORU, "https://danbooru.donmai.us", NULL, 1, 0, "dancomics", "Manga-style comic pages", "comic"},
  {"Gelbooru", BK_GELBOORU, "https://gelbooru.com", NULL, 0, 1, "gelbooru", "Anime and hentai (API key)"},
  {"Rule34", BK_GELBOORU, "https://api.rule34.xxx", "https://rule34.xxx", 0, 1, "rule34", "Rule 34, games and anime (API key)", NULL, 1},
  {"Sakugabooru", BK_MOEBOORU, "https://www.sakugabooru.com", NULL, 0, 0, "sakugabooru", "Anime animation clips (videos)"},
  // (video sites: only in the video browser)
  {"Rule34Video", BK_KVS, "https://rule34video.com", NULL, 1, 0, "rule34video", "Hentai and game animations", "hentai", 1},
  // (image sites, not boorus)
  {"E-Shuushuu", BK_ESHUU, "https://e-shuushuu.net", NULL, 1, 0, "eshuushuu", "Anime art, all ages"},
  {"booru.io", BK_BOORUIO, "https://booru.io", NULL, 0, 0, "booruio", "Anime and hentai art"},
  // (whole hentai episodes: only in the video browser)
  {"XVideos Hentai", BK_XV, "https://www.xvideos.com", NULL, 1, 0, "xvideos", "Hentai episodes, 10 min and longer", "hentai", 1},
  {"HentaiWorld", BK_HWORLD, "https://hentaiworld.tv", NULL, 1, 0, "hentaiworld", "Hentai episodes", NULL, 1},
  {"HentaiCloud", BK_HCLOUD, "https://www.hentaicloud.com", NULL, 1, 0, "hentaicloud", "Hentai episodes", NULL, 1},
  {"HentaiCity", BK_HCITY, "https://www.hentaicity.com", NULL, 1, 0, "hentaicity", "Hentai episodes, 8 min and longer", NULL, 1},
};
const int LV_NBUILTIN = 20;
int LV_NSITES = 20;
int lv_site_checks_items(const LvSite *s) { return s->kind == BK_HWORLD || s->kind == BK_HCLOUD || s->kind == BK_HCITY; }
int lv_is_long(const LvSite *s, const LvPost *p) { return lv_post_is_video(p) && (p->duration >= LV_LONG_VIDEO || (s && (s->kind == BK_XV || s->kind == BK_HWORLD || s->kind == BK_HCLOUD || s->kind == BK_HCITY))); }
int lv_site_is_booru(const LvSite *s) { return s->kind <= BK_E621; }
// (the strings of the sites added by hand, kept here)
static char added_names[LV_MAX_SITES][48], added_bases[LV_MAX_SITES][160];
int lv_booru_add_site(const char *name, int kind, const char *base) {
  if (LV_NSITES >= LV_MAX_SITES || kind < BK_DANBOORU || kind > BK_E621) return -1;
  int k = LV_NSITES;
  snprintf(added_names[k], sizeof added_names[k], "%s", name);
  snprintf(added_bases[k], sizeof added_bases[k], "%s", base);
  size_t n = strlen(added_bases[k]);
  while (n && added_bases[k][n - 1] == '/') added_bases[k][--n] = 0;
  LV_SITES[k] = (LvSite){added_names[k], kind, added_bases[k], NULL, kind == BK_DANBOORU || kind == BK_E621, 0, NULL, "Added by you"};
  LV_NSITES++;
  return 0;
}
void lv_booru_probe_url(int kind, const char *base, char *out, size_t cap) {
  switch (kind) {
    case BK_DANBOORU: snprintf(out, cap, "%s/posts.json?limit=1", base); break;
    case BK_MOEBOORU: snprintf(out, cap, "%s/post.json?limit=1", base); break;
    case BK_E621: snprintf(out, cap, "%s/posts.json?limit=1", base); break;
    default: snprintf(out, cap, "%s/index.php?page=dapi&s=post&q=index&json=1&limit=1", base);
  }
}
// 1 when the answer is a page of posts this kind reads
int lv_booru_probe(int kind, const char *json, size_t len) {
  LvSite s = {"probe", kind, "https://x", NULL, 0, 0, NULL, NULL};
  int n = 0;
  LvPost *p = lv_booru_parse(&s, json, len, LV_R_ALL, &n);
  int ok = p != NULL;
  lv_posts_free(p, n);
  return ok;
}
const char *LV_SORT_NAMES[LV_NSORTS] = {"Latest", "Hot", "Top", "Random"};
const char *LV_SPAN_NAMES[LV_NSPANS] = {"All time", "Today", "This week", "This month"};
int lv_booru_has_spans(const LvSite *s) { return s->kind != BK_GELBOORU; }

// never shown, never searched for: sexual content involving minors (each site's own word for it),
// the same list as the desktop app's (ui/discover.js, NEVER)
static const char *NEVER[] = {"loli", "lolicon", "shota", "shotacon", "toddlercon", "toddler", "child_on_child", "child",
                              "children", "young", "cub", "foal", "filly", "foalcon", "aged_down", "underage", "preteen"};
static int never(const char *tag, size_t n) {
  // (a tag may carry a kind: "character:..." isn't one of these, but "-loli" and "~loli" are)
  while (n && (*tag == '-' || *tag == '~')) { tag++; n--; }
  for (size_t k = 0; k < sizeof NEVER / sizeof *NEVER; k++)
    if (strlen(NEVER[k]) == n && !strncasecmp(NEVER[k], tag, n)) return 1;
  return 0;
}
int lv_booru_never_tags(const char *tags);
// AI-made pictures and videos aren't offered either: their tags (ai-generated, ...) or, for the video
// sites read by their titles, the phrases ("ai generated", "stable diffusion", ...)
int lv_ai_made(const char *tags) {
  static const char *const tagw[] = {"ai-generated", "ai_generated", "ai-assisted", "ai_assisted", "aigenerated", "ai_art", "aiart",
                                     "stable_diffusion", "novelai", "midjourney", "ai-created", NULL};
  static const char *const phrase[] = {"ai generated", "ai assisted", "ai art", "stable diffusion", "ai created", "made with ai", NULL};
  for (const char *p = tags; *p; ) {
    while (*p == ' ') p++;
    const char *e = p;
    while (*e && *e != ' ') e++;
    for (int k = 0; tagw[k]; k++) if ((size_t)(e - p) == strlen(tagw[k]) && !strncasecmp(p, tagw[k], e - p)) return 1;
    p = e;
  }
  for (int k = 0; phrase[k]; k++) {
    const char *f = strcasestr(tags, phrase[k]);
    size_t n = strlen(phrase[k]);
    if (f && (f == tags || f[-1] == ' ') && (f[n] == ' ' || !f[n])) return 1;
  }
  return 0;
}
static int any_never(const char *tags) { return lv_booru_never_tags(tags) || lv_ai_made(tags); }
int lv_booru_never_tags(const char *tags) {
  const char *p = tags;
  while (*p) {
    while (*p == ' ') p++;
    const char *e = p;
    while (*e && *e != ' ') e++;
    if (e > p && never(p, (size_t)(e - p))) return 1;
    p = e;
  }
  return 0;
}

char *lv_booru_clean(const char *q, char *out, size_t cap) {
  size_t o = 0;
  const char *p = q;
  while (*p) {
    while (*p == ' ') p++;
    const char *e = p;
    while (*e && *e != ' ') e++;
    size_t n = (size_t)(e - p);
    if (n && !never(p, n) && o + n + 2 < cap) {
      if (o) out[o++] = ' ';
      memcpy(out + o, p, n);
      o += n;
    }
    p = e;
  }
  out[o] = 0;
  return out;
}

static void enc(const char *s, char *out, size_t cap) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p && o + 4 < cap; p++) {
    if (isalnum(*p) || strchr("-_.~", *p)) out[o++] = (char)*p;
    else if (*p == ' ') out[o++] = '+';
    else o += (size_t)snprintf(out + o, cap - o, "%%%02X", *p);
  }
  out[o] = 0;
}

void lv_booru_url(const LvSite *s, const char *tags, int ratings, int sort, int page, char *out, size_t cap) {
  lv_booru_url_ex(s, tags, ratings, sort, LV_SPAN_ALL, NULL, page, out, cap);
}
void lv_booru_url_ex(const LvSite *s, const char *tags, int ratings, int sort, int span, const char *key, int page, char *out, size_t cap) {
  char q[1024], clean[512];
  lv_booru_clean(tags, clean, sizeof clean);
  snprintf(q, sizeof q, "%s%s%s", s->preset ? s->preset : "", s->preset && clean[0] ? " " : "", clean);
  // the ratings: Danbooru takes a list (and it doesn't count towards its 2 free tags); the others
  // leave out the ones not wanted
  if ((ratings & LV_R_ALL) != LV_R_ALL) {
    char r[64] = "";
    if (s->kind == BK_DANBOORU) {
      const char *c[] = {"g", "s", "q", "e"};
      strcat(r, " rating:");
      int first = 1;
      for (int k = 0; k < 4; k++) if (ratings & (1 << k)) { if (!first) strcat(r, ","); strcat(r, c[k]); first = 0; }
    } else {
      // (Moebooru and e621 have safe/questionable/explicit: "sensitive" is part of their safe)
      const char *gel[] = {"general", "sensitive", "questionable", "explicit"};
      const char *sqe[] = {"s", NULL, "q", "e"};
      for (int k = 0; k < 4; k++) {
        if (ratings & (1 << k)) continue;
        if (s->kind == BK_GELBOORU) { strcat(r, " -rating:"); strcat(r, gel[k]); }
        else if (sqe[k] && !(k == 0 && (ratings & LV_R_SENSITIVE))) { strcat(r, " -rating:"); strcat(r, sqe[k]); }
      }
    }
    strncat(q, r, sizeof q - strlen(q) - 1);
  }
  const char *order = "";
  switch (sort) {
    case LV_SORT_HOT: order = s->kind == BK_DANBOORU ? " order:rank" : s->kind == BK_E621 ? " order:hot" : ""; break;
    case LV_SORT_TOP: order = s->kind == BK_GELBOORU ? " sort:score" : " order:score"; break;
    case LV_SORT_RANDOM: order = s->kind == BK_GELBOORU ? " sort:random" : " order:random"; break;
  }
  strncat(q, order, sizeof q - strlen(q) - 1);
  // Top's span: Danbooru's age, e621's date, Moebooru's date from (Gelbooru's kind has none)
  if (sort == LV_SORT_TOP && span > LV_SPAN_ALL && span < LV_NSPANS) {
    char sp[48] = "";
    static const char *age[] = {"", "1d", "1w", "1mo"}, *e6[] = {"", "day", "week", "month"};
    if (s->kind == BK_DANBOORU) snprintf(sp, sizeof sp, " age:<%s", age[span]);
    else if (s->kind == BK_E621) snprintf(sp, sizeof sp, " date:%s", e6[span]);
    else if (s->kind == BK_MOEBOORU) {
      time_t t = time(NULL) - (span == LV_SPAN_DAY ? 86400 : span == LV_SPAN_WEEK ? 7 * 86400 : 30 * 86400);
      struct tm g;
      gmtime_r(&t, &g);
      strftime(sp, sizeof sp, " date:>=%Y-%m-%d", &g);
    }
    strncat(q, sp, sizeof q - strlen(q) - 1);
  }
  if (s->kind == BK_ESHUU || s->kind == BK_BOORUIO) {
    // the search's first word only (both read one tag reliably; e-shuushuu as its number, "#169")
    char w[128] = "";
    sscanf(clean, "%127s", w);
    if (s->kind == BK_ESHUU) {
      static const char *by[LV_NSORTS] = {"date_added", "bayesian_rating", "favorites", "date_added"};
      int pg = sort == LV_SORT_RANDOM ? 1 + (int)(time(NULL) / 60 % 500) + page : page;
      int n = snprintf(out, cap, "%s/api/v1/images?page=%d&per_page=40&sort_by=%s", s->base, pg < 1 ? 1 : pg, by[sort >= 0 && sort < LV_NSORTS ? sort : 0]);
      if (w[0] == '#' && n > 0 && (size_t)n < cap) snprintf(out + n, cap - n, "&tags=%d", atoi(w + 1));
    } else {
      // (booru.io: one page a search; past it, no address: the end)
      if (page > 1) { out[0] = 0; return; }
      char e[400];
      enc(w, e, sizeof e);
      snprintf(out, cap, "%s/api/legacy/query/entity?query=%s", s->base, e);
    }
    return;
  }
  if (s->kind == BK_HCITY) {
    // (eight minutes and longer only: its own filter)
    static const char *by[LV_NSORTS] = {"recent", "popular", "rate", "view"};
    int pg = sort == LV_SORT_RANDOM ? 1 + (int)(time(NULL) / 60 % 30) + page - 1 : page;
    char sfx[16] = "";
    if (pg > 1) snprintf(sfx, sizeof sfx, "-%d", pg);
    if (clean[0]) {
      if (page > 1) { out[0] = 0; return; } // (its search: one page)
      char e[400];
      enc(clean, e, sizeof e);
      snprintf(out, cap, "%s/search/video/%s?min_duration=480", s->base, e);
    } else snprintf(out, cap, "%s/videos/straight/all-%s%s.html?min_duration=480", s->base, by[sort >= 0 && sort < LV_NSORTS ? sort : 0], sfx);
    return;
  }
  if (s->kind == BK_HWORLD || s->kind == BK_HCLOUD) {
    char e[400];
    enc(clean, e, sizeof e);
    int pg = sort == LV_SORT_RANDOM ? 1 + (int)(time(NULL) / 60 % 30) + page - 1 : page;
    if (pg < 1) pg = 1;
    if (s->kind == BK_HWORLD) {
      if (clean[0]) snprintf(out, cap, "%s/search?q=%s&page=%d", s->base, e, pg);
      else snprintf(out, cap, "%s/%s?page=%d", s->base, sort == LV_SORT_HOT ? "trending" : sort == LV_SORT_TOP ? "most-viewed" : "new-releases", pg);
    } else {
      // (its search has one page: past it, the end)
      if (clean[0]) { if (page > 1) { out[0] = 0; return; } snprintf(out, cap, "%s/search?search_query=%s&search_type=videos", s->base, e); }
      else snprintf(out, cap, "%s/videos?o=%s&page=%d", s->base, sort == LV_SORT_HOT ? "mv" : sort == LV_SORT_TOP ? "tr" : "mr", pg);
    }
    return;
  }
  if (s->kind == BK_XV) {
    // the search, always with "hentai", ten minutes and longer
    static const char *by[LV_NSORTS] = {"uploaddate", "relevance", "rating", "random"};
    static const char *datef[LV_NSPANS] = {"", "&datef=today", "&datef=week", "&datef=month"};
    char words[300], e[700];
    snprintf(words, sizeof words, "%s%s%s", s->preset ? s->preset : "hentai", clean[0] ? " " : "", clean);
    enc(words, e, sizeof e);
    snprintf(out, cap, "%s/?k=%s&durf=10min_more&sort=%s%s&p=%d", s->base, e, by[sort >= 0 && sort < LV_NSORTS ? sort : 0],
             sort == LV_SORT_TOP && span > 0 && span < LV_NSPANS ? datef[span] : "", page > 1 ? page - 1 : 0);
    return;
  }
  if (s->kind == BK_KVS) {
    // a tag's page, in the site's order: the search's first word (or the preset), as the site's slug
    char slug[128] = "";
    const char *w = clean[0] ? clean : s->preset ? s->preset : "hentai";
    size_t o = 0;
    for (const char *c = w; *c && *c != ' ' && o + 1 < sizeof slug; c++) {
      unsigned char ch = (unsigned char)tolower((unsigned char)*c);
      slug[o++] = ch == '_' ? '-' : isalnum(ch) || ch == '-' ? (char)ch : '-';
    }
    slug[o] = 0;
    static const char *top[] = {"rating", "rating_today", "rating_week", "rating_month"};
    // (a search word "longest": the site's longest first)
    int longest = strstr(clean, "longest") != NULL;
    const char *by = longest ? "duration" : sort == LV_SORT_HOT ? "video_viewed_week" : sort == LV_SORT_TOP ? top[span >= 0 && span < LV_NSPANS ? span : 0]
                   : sort == LV_SORT_RANDOM ? "video_viewed" : "post_date";
    if (longest) { const char *w2 = s->preset ? s->preset : "hentai"; snprintf(slug, sizeof slug, "%s", w2); }
    // (Random: a page of the most watched, somewhere further along each time)
    int from = sort == LV_SORT_RANDOM ? page + (int)(time(NULL) / 60 % 40) : page;
    snprintf(out, cap, "%s/tags/%s/?mode=async&function=get_block&block_id=custom_list_videos_common_videos&sort_by=%s&from=%d",
             s->base, slug, by, from < 1 ? 1 : from);
    return;
  }
  const char *t = q;
  while (*t == ' ') t++;
  char e[2048];
  enc(t, e, sizeof e);
  switch (s->kind) {
    case BK_DANBOORU: case BK_E621:
      snprintf(out, cap, "%s/posts.json?limit=%d&page=%d&tags=%s", s->base, LV_PAGE, page, e); break;
    case BK_MOEBOORU:
      snprintf(out, cap, "%s/post.json?limit=%d&page=%d&tags=%s", s->base, LV_PAGE, page, e); break;
    default:
      snprintf(out, cap, "%s/index.php?page=dapi&s=post&q=index&json=1&limit=%d&pid=%d&tags=%s", s->base, LV_PAGE, page - 1, e);
  }
  // an account's key: "<user id> <api key>"
  if (key && *key) {
    char user[64] = "", api[160] = "";
    if (sscanf(key, "%63s %159s", user, api) == 2) {
      char ue[200], ae[400];
      enc(user, ue, sizeof ue);
      enc(api, ae, sizeof ae);
      size_t n = strlen(out);
      snprintf(out + n, cap > n ? cap - n : 0, "&user_id=%s&api_key=%s", ue, ae);
    }
  }
}

static char *sdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); if (d) memcpy(d, s, n); return d; }
static void ext_of(const char *url, char *out) {
  out[0] = 0;
  const char *q = strchr(url, '?');
  size_t n = q ? (size_t)(q - url) : strlen(url);
  const char *d = NULL;
  for (size_t i = 0; i < n; i++) if (url[i] == '.') d = url + i;
  if (!d) return;
  size_t l = (size_t)(url + n - d - 1);
  if (l > 5) return;
  for (size_t i = 0; i < l; i++) out[i] = (char)tolower((unsigned char)d[1 + i]);
  out[l] = 0;
}
static int pic(const char *url) {
  char e[8];
  ext_of(url, e);
  return !strcmp(e, "jpg") || !strcmp(e, "jpeg") || !strcmp(e, "png") || !strcmp(e, "webp");
}
int lv_post_is_video(const LvPost *p) { return !strcmp(p->ext, "mp4") || !strcmp(p->ext, "webm"); }

static int rating_of(const char *r) {
  switch (tolower((unsigned char)r[0])) {
    case 'e': return LV_R_EXPLICIT;
    case 'q': return LV_R_QUESTIONABLE;
    case 's': return !strncasecmp(r, "sensitive", 9) ? LV_R_SENSITIVE : r[1] ? LV_R_GENERAL : LV_R_GENERAL; // ("s": Moebooru's safe)
    default: return LV_R_GENERAL;
  }
}

// e621's tags come in groups: all of them, joined
static void e621_tags(const Json *j, int tags, char *out, size_t cap) {
  size_t o = 0;
  out[0] = 0;
  const char *groups[] = {"artist", "character", "copyright", "species", "general", "meta"};
  for (size_t g = 0; g < 6; g++) {
    int arr = json_get(j, tags, groups[g]);
    for (int k = 0; arr >= 0 && k < j->t[arr].size; k++) {
      char t[256];
      json_str(j, json_at(j, arr, k), t, sizeof t);
      size_t n = strlen(t);
      if (!n || o + n + 2 >= cap) continue;
      if (o) out[o++] = ' ';
      memcpy(out + o, t, n + 1);
      o += n;
    }
  }
}

// ---------- KVS: the list page's cards, read as HTML ----------
// (an attribute's value inside [p, end): after name=" up to the next quote)
static int attr(const char *p, const char *end, const char *name, char *out, size_t cap) {
  out[0] = 0;
  const char *a = strstr(p, name);
  if (!a || a >= end) return 0;
  a += strlen(name);
  char q = *a == '\'' ? '\'' : '"';
  if (*a == q) a++;
  const char *e = strchr(a, q);
  if (!e || e > end) return 0;
  size_t n = (size_t)(e - a) < cap - 1 ? (size_t)(e - a) : cap - 1;
  memcpy(out, a, n);
  out[n] = 0;
  return 1;
}
// (HTML's &amp; &quot; &#039; ... in place)
static void unent(char *s) {
  static const struct { const char *e; char c; } E[] = {{"&amp;", '&'}, {"&quot;", '"'}, {"&#039;", '\''}, {"&#39;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}};
  char *o = s;
  for (char *p = s; *p; ) {
    int done = 0;
    for (size_t k = 0; k < sizeof E / sizeof *E && *p == '&'; k++)
      if (!strncmp(p, E[k].e, strlen(E[k].e))) { *o++ = E[k].c; p += strlen(E[k].e); done = 1; break; }
    if (!done) *o++ = *p++;
  }
  *o = 0;
}
static LvPost *kvs_parse(const char *text, size_t len, int *n) {
  (void)len;
  int cap = 0;
  for (const char *p = strstr(text, "data-video-card-id="); p; p = strstr(p + 1, "data-video-card-id=")) cap++;
  LvPost *out = calloc(cap + 1, sizeof *out);
  if (!out) return NULL;
  for (const char *p = strstr(text, "data-video-card-id="); p; ) {
    const char *next = strstr(p + 1, "data-video-card-id=");
    const char *end = next ? next : text + strlen(text);
    LvPost q;
    memset(&q, 0, sizeof q);
    char id[24], page[600], title[400], img[600], tm[16] = "";
    attr(p, end, "data-video-card-id=", id, sizeof id);
    q.id = atoll(id);
    // (its own page: the link to /video/, not the popup's)
    page[0] = 0;
    for (const char *a = strstr(p, "href="); a && a < end && !strstr(page, "/video/"); a = strstr(a + 5, "href=")) attr(a, end, "href=", page, sizeof page);
    if (!strstr(page, "/video/")) page[0] = 0;
    attr(p, end, "title=", title, sizeof title);
    unent(title);
    const char *im = strstr(p, "class=\"thumb");
    // (lazy pictures: the real address is in one of these; src may be a placeholder)
    static const char *const imgattr[] = {"data-rd-src=", "data-original=", "data-src=", "src="};
    img[0] = 0;
    for (int k = 0; k < 4 && im && im < end && strncmp(img, "http", 4); k++) {
      const char *tagend = strchr(im, '>');
      attr(im, tagend && tagend < end ? tagend : end, imgattr[k], img, sizeof img);
    }
    if (strncmp(img, "http", 4)) img[0] = 0;
    const char *t = strstr(p, "class=\"time\">");
    if (t && t < end) { t += 13; size_t k = 0; while (t[k] && t[k] != '<' && k < sizeof tm - 1) { tm[k] = t[k]; k++; } tm[k] = 0; }
    p = next;
    if (!q.id || strncmp(page, "http", 4) || !img[0]) continue;
    // the title's words, as tags: what the never-shown words are checked against (and searched)
    char tags[600];
    size_t o = 0;
    for (const char *c = title; *c && o + 2 < sizeof tags; c++) {
      unsigned char ch = (unsigned char)*c;
      tags[o++] = isalnum(ch) ? (char)tolower(ch) : ' ';
    }
    tags[o] = 0;
    if (any_never(tags)) continue;
    snprintf(q.ext, sizeof q.ext, "mp4");
    q.width = 1280; q.height = 720;
    q.rating = LV_R_EXPLICIT;
    q.score = 0;
    q.preview = sdup(img);
    q.sample = sdup(img);
    q.file = sdup(page); // (its video page: lv_kvs_video finds the MP4 there)
    q.page = sdup(page);
    q.source = sdup(tm);
    // ("m:ss" or "h:mm:ss": seconds)
    { int a = 0, b = 0, c = 0, nf = sscanf(tm, "%d:%d:%d", &a, &b, &c); q.duration = nf == 3 ? a * 3600 + b * 60 + c : nf == 2 ? a * 60 + b : 0; }
    q.tags = sdup(tags);
    if (!q.preview || !q.file || !q.page || !q.tags) { free(q.preview); free(q.sample); free(q.file); free(q.page); free(q.source); free(q.tags); continue; }
    out[(*n)++] = q;
  }
  return out;
}
// ---------- xvideos: its search page's thumbnails, read as HTML ----------
static LvPost *xv_parse(const char *text, size_t len, int *n) {
  (void)len;
  int cap = 0;
  for (const char *p = strstr(text, "data-eid="); p; p = strstr(p + 1, "data-eid=")) cap++;
  LvPost *out = calloc(cap + 1, sizeof *out);
  if (!out) return NULL;
  for (const char *p = strstr(text, "data-eid="); p; ) {
    const char *next = strstr(p + 1, "data-eid=");
    const char *end = next ? next : text + strlen(text);
    LvPost q;
    memset(&q, 0, sizeof q);
    char id[24] = "", href[400] = "", img[600] = "", title[400] = "", dur[40] = "";
    attr(p, end, "data-id=", id, sizeof id);
    const char *th = strstr(p, "class=\"thumb\"");
    if (th && th < end) attr(th, end, "href=", href, sizeof href);
    const char *im = strstr(p, "<img");
    if (im && im < end) attr(im, end, "data-src=", img, sizeof img);
    const char *ti = strstr(p, "class=\"title\"");
    if (ti && ti < end) attr(ti, end, "title=", title, sizeof title);
    unent(title);
    const char *du = strstr(p, "class=\"duration\">");
    if (du && du < end) { du += strlen("class=\"duration\">"); size_t k = 0; while (du[k] && du[k] != '<' && k < sizeof dur - 1) { dur[k] = du[k]; k++; } dur[k] = 0; }
    p = next;
    q.id = atoll(id);
    if (!q.id || href[0] != '/' || strncmp(img, "http", 4)) continue;
    // ("51 min", "1 h 3 min", "34 sec": seconds)
    { int h = 0, m = 0, sec = 0; const char *c = dur;
      while (*c) { int v = atoi(c); while (*c && isdigit((unsigned char)*c)) c++; while (*c == ' ') c++;
        if (*c == 'h') h = v; else if (*c == 'm') m = v; else if (*c == 's') sec = v;
        while (*c && !isdigit((unsigned char)*c)) c++; }
      q.duration = h * 3600 + m * 60 + sec; }
    char tags[600];
    size_t o = 0;
    for (const char *c = title; *c && o + 2 < sizeof tags; c++) { unsigned char ch = (unsigned char)*c; tags[o++] = isalnum(ch) ? (char)tolower(ch) : ' '; }
    tags[o] = 0;
    if (any_never(tags)) continue;
    char page[600];
    snprintf(page, sizeof page, "https://www.xvideos.com%s", href);
    snprintf(q.ext, sizeof q.ext, "mp4");
    q.width = 1280; q.height = 720; q.rating = LV_R_EXPLICIT;
    q.preview = sdup(img); q.sample = sdup(img); q.file = sdup(page); q.page = sdup(page); q.source = sdup(dur); q.tags = sdup(tags);
    if (!q.preview || !q.sample || !q.file || !q.page || !q.source || !q.tags) { free(q.preview); free(q.sample); free(q.file); free(q.page); free(q.source); free(q.tags); continue; }
    out[(*n)++] = q;
  }
  return out;
}
// ---------- the episode sites: their list cards, then each episode's page ----------
static int dur_of(const char *t) { int a = 0, b = 0, c = 0, n = sscanf(t, "%d:%d:%d", &a, &b, &c); return n == 3 ? a * 3600 + b * 60 + c : n == 2 ? a * 60 + b : 0; }
static void words_of(const char *in, char *out, size_t cap) {
  size_t o = 0;
  for (const char *c = in; *c && o + 2 < cap; c++) { unsigned char ch = (unsigned char)*c; out[o++] = isalnum(ch) ? (char)tolower(ch) : ' '; }
  out[o] = 0;
}
static LvPost *episodes_parse(const LvSite *s, const char *text, int *n) {
  const char *mark = s->kind == BK_HWORLD ? "<article" : s->kind == BK_HCITY ? "class=\"outer-item\"" : "id=\"playvideo_";
  int cap = 0;
  for (const char *p = strstr(text, mark); p; p = strstr(p + 1, mark)) cap++;
  LvPost *out = calloc(cap + 1, sizeof *out);
  if (!out) return NULL;
  for (const char *p = strstr(text, mark); p; ) {
    const char *next = strstr(p + 1, mark);
    const char *end = next ? next : text + strlen(text);
    LvPost q;
    memset(&q, 0, sizeof q);
    char href[400] = "", img[600] = "", title[300] = "", dur[24] = "", file[600] = "";
    if (s->kind == BK_HCITY) {
      attr(p, end, "href=", href, sizeof href);
      // (its links go through a counter, /click/1-2/video/...: the page itself is /video/...)
      char *ck = strstr(href, "/click/");
      if (ck) { char *v = strstr(ck, "/video/"); if (v) memmove(ck, v, strlen(v) + 1); }
      char *h0 = strstr(href, "://");
      if (h0) { char *sl = strchr(h0 + 3, '/'); if (sl) memmove(href, sl, strlen(sl) + 1); } // (the path: the base goes back on below)
      const char *im = strstr(p, "<img");
      if (im && im < end) { attr(im, end, "src=", img, sizeof img); attr(im, end, "alt=", title, sizeof title); }
      const char *d = strstr(p, "class=\"time\">");
      if (d && d < end) { d += 13; size_t k = 0; while (d[k] && d[k] != '<' && k < sizeof dur - 1) { dur[k] = d[k]; k++; } dur[k] = 0; }
      q.duration = dur_of(dur);
      q.width = 320; q.height = 180;
      unsigned long long h = 1469598103934665603ull;
      for (const char *x = href; *x; x++) h = (h ^ (unsigned char)*x) * 1099511628211ull;
      q.id = (long long)(h >> 1);
    } else if (s->kind == BK_HWORLD) {
      attr(p, end, "aria-label=", title, sizeof title);
      if (!title[0]) attr(p, end, "alt=", title, sizeof title); // (the site's newer cards: the title on the cover's alt)
      attr(p, end, "href=", href, sizeof href);
      attr(p, end, "src=", img, sizeof img);
      // (the smaller cover: 268 wide, near a card's size)
      char *c5 = strstr(img, "cover-536.webp");
      if (c5) memcpy(c5, "cover-268", 9);
      const char *d = strstr(p, "tabular-nums");
      if (d && d < end) d = strchr(d, '>'); // (whatever attributes follow it)
      if (d && d < end) { d += 1; size_t k = 0; while (d[k] && d[k] != '<' && k < sizeof dur - 1) { dur[k] = d[k]; k++; } dur[k] = 0; }
      q.duration = dur_of(dur);
      q.width = 268; q.height = 394;
      unsigned long long h = 1469598103934665603ull;
      for (const char *x = href; *x; x++) h = (h ^ (unsigned char)*x) * 1099511628211ull;
      q.id = (long long)(h >> 1);
    } else {
      char id[24] = "";
      sscanf(p + strlen(mark), "%23[0-9]", id);
      q.id = atoll(id);
      attr(p, end, "href=", href, sizeof href);
      const char *im = strstr(p, "<img");
      if (im && im < end) { char rel[300] = ""; attr(im, end, "src=", rel, sizeof rel); snprintf(img, sizeof img, "%s%s", rel[0] == '/' ? s->base : "", rel); attr(im, end, "title=", title, sizeof title); }
      // ("16 Min": the last of the card's details)
      const char *m = strstr(p, " Min<");
      if (m && m < end) { const char *b = m; while (b > p && isdigit((unsigned char)b[-1])) b--; q.duration = atoi(b) * 60; }
      snprintf(file, sizeof file, "%s/media/videos/hd/%lld.mp4", s->base, q.id); // (its "hd" copy: 480 lines, the Vita's screen)
      q.width = 276; q.height = 460;
    }
    p = next;
    unent(title);
    if (!q.id || href[0] != '/' || strncmp(img, "http", 4)) continue;
    char tags[600];
    words_of(title, tags, sizeof tags);
    if (any_never(tags)) continue;
    char page[600];
    snprintf(page, sizeof page, "%s%s", s->base, href);
    snprintf(q.ext, sizeof q.ext, "mp4");
    q.rating = LV_R_EXPLICIT;
    q.preview = sdup(img); q.sample = sdup(img); q.file = sdup(file[0] ? file : page); q.page = sdup(page); q.source = sdup(dur); q.tags = sdup(tags);
    if (!q.preview || !q.sample || !q.file || !q.page || !q.source || !q.tags) { free(q.preview); free(q.sample); free(q.file); free(q.page); free(q.source); free(q.tags); continue; }
    out[(*n)++] = q;
  }
  return out;
}
int lv_item_check(const LvSite *s, LvPost *post, const char *html) {
  char text[4000] = "", meta[1200];
  size_t o = 0;
  // the page's own words for it: its title, its description, its keywords, and (HentaiCloud) its categories
  static const char *const metas[] = {"property=\"og:title\" content=", "name=\"description\" content=", "name=\"keywords\" content=", NULL};
  for (int k = 0; metas[k]; k++) {
    const char *m = strstr(html, metas[k]);
    if (!m) continue;
    attr(m, m + strlen(m), "content=", meta, sizeof meta);
    o += (size_t)snprintf(text + o, sizeof text - o, " %s", meta);
    if (o >= sizeof text) o = sizeof text - 1;
  }
  // (HentaiCity: its tags, /tags/video/mind+control)
  for (const char *t = strstr(html, "/tags/video/"); t && o + 2 < sizeof text; t = strstr(t + 12, "/tags/video/")) {
    text[o++] = ' ';
    for (const char *c = t + 12; *c && *c != '"' && *c != '/' && o + 2 < sizeof text; c++) text[o++] = *c == '+' ? ' ' : *c;
    text[o] = 0;
  }
  const char *cat = strstr(html, "class=\"category\"");
  if (cat) {
    const char *ce = strstr(cat, "</ul>");
    for (const char *a = strstr(cat, "<a "); a && ce && a < ce && o + 2 < sizeof text; a = strstr(a + 3, "<a ")) {
      const char *gt = strchr(a, '>');
      if (!gt || gt > ce) break;
      gt++;
      text[o++] = ' '; // (a word apart from what came before: "movies" and "Loli" aren't "moviesloli")
      while (*gt && *gt != '<' && o + 2 < sizeof text) text[o++] = *gt++;
      text[o++] = ' ';
    }
    text[o] = 0;
  }
  unent(text);
  char words[4000];
  words_of(text, words, sizeof words);
  if (any_never(words)) return 0;
  // the MP4, where the page names it (HentaiWorld: its og:video)
  if (s->kind == BK_HWORLD || s->kind == BK_HCITY) {
    char mp4[600] = "";
    if (s->kind == BK_HWORLD) {
      const char *v = strstr(html, "property=\"og:video\" content=");
      if (v) attr(v, v + strlen(v), "content=", mp4, sizeof mp4);
    } else {
      // (its 480p copy, next to the one for phones: .../flv/0807/38331/mobile.mp4 -> 480p.mp4; if that
      //  isn't there, lv_video_fallback goes back to mobile.mp4)
      const char *m = strstr(html, "/mobile.mp4");
      if (m) {
        const char *b = m;
        while (b > html && b[-1] != '"' && b[-1] != '\'' && b[-1] != ' ' && b - html > 0 && m - b < 500) b--;
        size_t n = (size_t)(m - b);
        if (n + 10 < sizeof mp4) { memcpy(mp4, b, n); memcpy(mp4 + n, "/480p.mp4", 10); }
      }
    }
    if (strncmp(mp4, "http", 4)) return 0;
    char *d = malloc(strlen(mp4) + 1);
    if (!d) return 0;
    strcpy(d, mp4);
    free(post->file);
    post->file = d;
  }
  return 1;
}

// Its video page: the MP4 (the smaller one, quickest to come), and its keywords checked.
int lv_xv_video(const char *html, char *out, size_t cap, int *nev) {
  *nev = 0;
  out[0] = 0;
  const char *k = strstr(html, "\"keywords\":\"");
  if (k) {
    char kw[1200];
    k += 12;
    size_t n = 0;
    while (k[n] && k[n] != '"' && n < sizeof kw - 1) { kw[n] = k[n] == ',' ? ' ' : (char)tolower((unsigned char)k[n]); n++; }
    kw[n] = 0;
    if (any_never(kw)) { *nev = 1; return -1; }
  }
  static const char *keys[] = {"setVideoUrlLow('", "setVideoUrlHigh('"};
  for (int i = 0; i < 2; i++) {
    const char *v = strstr(html, keys[i]);
    if (!v) continue;
    v += strlen(keys[i]);
    const char *e = strchr(v, '\'');
    if (!e || (size_t)(e - v) >= cap || strncmp(v, "http", 4)) continue;
    memcpy(out, v, e - v);
    out[e - v] = 0;
    return 0;
  }
  return -1;
}

// A video's address that didn't answer: the site's smaller copy of it, where there's one. 0 and out, or -1.
int lv_video_fallback(const char *url, char *out, size_t cap) {
  static const char *const from[] = {"/media/videos/hd/", "/480p.mp4", NULL}, *const to[] = {"/media/videos/iphone/", "/mobile.mp4", NULL};
  for (int k = 0; from[k]; k++) {
    const char *f = strstr(url, from[k]);
    if (!f) continue;
    snprintf(out, cap, "%.*s%s%s", (int)(f - url), url, to[k], f + strlen(from[k]));
    return 0;
  }
  return -1;
}

int lv_kvs_video(const char *html, char *out, size_t cap, int *nev) {
  *nev = 0;
  out[0] = 0;
  // the tags: links to /tags/<slug>/
  for (const char *p = strstr(html, "/tags/"); p; p = strstr(p + 6, "/tags/")) {
    char slug[128];
    size_t k = 0;
    const char *c = p + 6;
    while (c[k] && c[k] != '/' && c[k] != '"' && k < sizeof slug - 1) { slug[k] = c[k] == '-' ? '_' : c[k]; k++; }
    slug[k] = 0;
    // (each part of "loli-elf", and the whole)
    char words[160];
    snprintf(words, sizeof words, "%s", slug);
    for (char *w = words; *w; w++) if (*w == '_') *w = ' ';
    if (never(slug, strlen(slug)) || any_never(words)) { *nev = 1; return -1; }
  }
  // 480p, the Vita's screen (544 lines): the one whose label says so; else the first (the smallest)
  static const char *const keys[] = {"video_url:", "video_alt_url:", "video_alt_url2:", "video_alt_url3:"};
  int pick = 0;
  for (int k = 0; k < 4; k++) {
    char lab[32];
    snprintf(lab, sizeof lab, "%.*s_text:", (int)strlen(keys[k]) - 1, keys[k]);
    const char *t = strstr(html, lab);
    if (t && strstr(t, "480") && strstr(t, "480") < t + strlen(lab) + 12) { pick = k; break; }
  }
  for (int pass = 0; pass < 2; pass++) {
    const char *v = strstr(html, keys[pass ? 0 : pick]);
    if (!v) continue;
    v += strlen(keys[pass ? 0 : pick]);
    while (*v == ' ') v++;
    char q = *v;
    if (q != '\'' && q != '"') continue;
    const char *e = strchr(v + 1, q);
    if (!e) continue;
    const char *u = v + 1;
    if (!strncmp(u, "function/0/", 11)) u += 11; // (some KVS sites wrap it)
    size_t n = (size_t)(e - u);
    if (n >= cap || strncmp(u, "http", 4)) continue;
    memcpy(out, u, n);
    out[n] = 0;
    return 0;
  }
  return -1;
}

// ---------- the image sites ----------
void lv_esh_tag_url(const LvSite *s, const char *word, char *out, size_t cap) {
  char w[200], e[400];
  snprintf(w, sizeof w, "%s", word);
  for (char *c = w; *c; c++) if (*c == '_') *c = ' ';
  enc(w, e, sizeof e);
  snprintf(out, cap, "%s/api/v1/tags/?search=%s&limit=20", s->base, e);
}
int lv_esh_tag_id(const char *text, size_t len, const char *word, char *out, size_t cap) {
  Json j;
  if (json_parse(&j, text, len) != 0) return -1;
  int tags = json_get(&j, 0, "tags"), best = -1, best_exact = 0;
  double best_use = -1;
  char w[200];
  snprintf(w, sizeof w, "%s", word);
  for (char *c = w; *c; c++) if (*c == '_') *c = ' ';
  for (int k = tags >= 0 ? tags + 1 : -1, i = 0; tags >= 0 && i < j.t[tags].size; i++, k = j.t[k].next) {
    char title[200];
    json_gs(&j, k, "title", title, sizeof title);
    int exact = !strcasecmp(title, w);
    double use = json_gn(&j, k, "usage_count");
    if ((exact && !best_exact) || (exact == best_exact && use > best_use)) { best = (int)json_gn(&j, k, "tag_id"); best_exact = exact; best_use = use; }
  }
  json_free(&j);
  if (best <= 0) return -1;
  snprintf(out, cap, "#%d", best);
  return 0;
}
// (the words of a picture's tags: "blonde hair" -> blonde_hair, lower case; booru.io's "x#character" -> x)
static size_t tag_add(char *out, size_t o, size_t cap, const char *t) {
  if (!t[0] || o + 2 >= cap) return o;
  if (o) out[o++] = ' ';
  for (const char *c = t; *c && *c != '#' && o + 1 < cap; c++) out[o++] = *c == ' ' ? '_' : (char)tolower((unsigned char)*c);
  out[o] = 0;
  return o;
}
// (no ratings on these sites: explicit when the tags say so, else general)
static int rating_from_tags(const char *tags) {
  static const char *x[] = {"nipples", "pussy", "penis", "sex", "nude", "cum", "anus", "vaginal", "fellatio", "completely_nude", NULL};
  for (int k = 0; x[k]; k++) {
    const char *p = tags;
    size_t n = strlen(x[k]);
    while ((p = strstr(p, x[k]))) {
      if ((p == tags || p[-1] == ' ') && (p[n] == ' ' || !p[n])) return LV_R_EXPLICIT;
      p += n;
    }
  }
  return LV_R_GENERAL;
}
static LvPost *image_site_parse(const LvSite *s, const char *text, size_t len, int ratings, int *n) {
  Json j;
  if (json_parse(&j, text, len) != 0) return NULL;
  int list = json_get(&j, 0, s->kind == BK_ESHUU ? "images" : "data");
  if (list < 0 || j.t[list].type != J_ARR) { json_free(&j); return NULL; }
  LvPost *out = calloc(j.t[list].size + 1, sizeof *out);
  char *tags = malloc(8192);
  if (!out || !tags) { free(out); free(tags); json_free(&j); return NULL; }
  for (int k = list + 1, i = 0; i < j.t[list].size; i++, k = j.t[k].next) {
    LvPost p;
    memset(&p, 0, sizeof p);
    char a[600] = "", b[600] = "", c[600] = "", page[600] = "";
    size_t o = 0;
    tags[0] = 0;
    if (s->kind == BK_ESHUU) {
      char fn[128], ext[16];
      json_gs(&j, k, "filename", fn, sizeof fn);
      json_gs(&j, k, "ext", ext, sizeof ext);
      p.id = (long long)json_gn(&j, k, "image_id");
      p.width = (int)json_gn(&j, k, "width");
      p.height = (int)json_gn(&j, k, "height");
      if (!fn[0] || !ext[0]) continue;
      snprintf(a, sizeof a, "https://cdn.e-shuushuu.net/thumbs/%s.webp", fn);
      // (the medium copy, made for screens, where there is one; else the picture)
      snprintf(c, sizeof c, "%s/images/%s.%s", s->base, fn, ext);
      if (json_gn(&j, k, "medium") >= 1) snprintf(b, sizeof b, "https://cdn.e-shuushuu.net/medium/%s.jpg", fn);
      else snprintf(b, sizeof b, "%s", c);
      snprintf(page, sizeof page, "%s/images/%lld", s->base, p.id);
      snprintf(p.ext, sizeof p.ext, "%s", !strcmp(ext, "jpeg") ? "jpg" : ext);
      int tl = json_get(&j, k, "tags");
      for (int t = tl >= 0 ? tl + 1 : -1, m = 0; tl >= 0 && m < j.t[tl].size; m++, t = j.t[t].next) {
        char tt[200];
        json_gs(&j, t, "title", tt, sizeof tt);
        o = tag_add(tags, o, 8192, tt);
      }
    } else {
      char key[64], ctype[40];
      json_gs(&j, k, "key", key, sizeof key);
      json_gs(&j, k, "contentType", ctype, sizeof ctype);
      if (!key[0] || strncmp(ctype, "image/", 6)) continue;
      // (an id from the key, for the saved marks)
      unsigned long long h = 1469598103934665603ull;
      for (const char *q = key; *q; q++) h = (h ^ (unsigned char)*q) * 1099511628211ull;
      p.id = (long long)(h >> 1);
      int at = json_get(&j, k, "attributes");
      p.width = (int)json_gn(&j, at, "width");
      p.height = (int)json_gn(&j, at, "height");
      int tr = json_get(&j, k, "transforms");
      char t300[200] = "", t900[200] = "", t1500[200] = "", t2400[200] = "";
      json_gs(&j, tr, "width=300:image/jpeg", t300, sizeof t300);
      json_gs(&j, tr, "width=900:image/jpeg", t900, sizeof t900);
      json_gs(&j, tr, "width=1500:image/jpeg", t1500, sizeof t1500);
      json_gs(&j, tr, "width=2400:image/jpeg", t2400, sizeof t2400);
      if (!t300[0]) continue;
      snprintf(a, sizeof a, "%s/api/legacy/data/%s", s->base, t300);
      snprintf(b, sizeof b, "%s/api/legacy/data/%s", s->base, t1500[0] ? t1500 : t900[0] ? t900 : t300);
      snprintf(c, sizeof c, "%s/api/legacy/data/%s", s->base, t2400[0] ? t2400 : t1500[0] ? t1500 : t900[0] ? t900 : t300);
      snprintf(page, sizeof page, "%s/p/%s", s->base, key);
      snprintf(p.ext, sizeof p.ext, "jpg");
      int tg = json_get(&j, k, "tags");
      for (int t = tg >= 0 ? tg + 1 : -1, m = 0; tg >= 0 && m < j.t[tg].size; m++, t = j.t[t + 1].next) {
        char tt[200];
        json_str(&j, t, tt, sizeof tt); // (the key: the tag)
        o = tag_add(tags, o, 8192, tt);
      }
    }
    // minors never; a rating the search didn't want
    if (any_never(tags)) continue;
    p.rating = rating_from_tags(tags);
    if (!(p.rating & ratings)) continue;
    p.preview = sdup(a); p.sample = sdup(b); p.file = sdup(c); p.page = sdup(page); p.source = sdup(""); p.tags = sdup(tags);
    if (!p.preview || !p.sample || !p.file || !p.page || !p.source || !p.tags) { free(p.preview); free(p.sample); free(p.file); free(p.page); free(p.source); free(p.tags); continue; }
    out[(*n)++] = p;
  }
  free(tags);
  json_free(&j);
  return out;
}

LvPost *lv_booru_parse(const LvSite *s, const char *text, size_t len, int ratings, int *n) {
  *n = 0;
  if (s->kind == BK_KVS) return kvs_parse(text, len, n);
  if (s->kind == BK_XV) return xv_parse(text, len, n);
  if (s->kind == BK_HWORLD || s->kind == BK_HCLOUD || s->kind == BK_HCITY) return episodes_parse(s, text, n);
  if (s->kind == BK_ESHUU || s->kind == BK_BOORUIO) return image_site_parse(s, text, len, ratings, n);
  Json j;
  if (json_parse(&j, text, len) != 0) return NULL;
  int list = 0;
  if (s->kind == BK_E621) list = json_get(&j, 0, "posts");
  else if (j.t[0].type == J_OBJ) list = json_get(&j, 0, "post"); // (some Gelbooru answers wrap the list)
  if (list < 0 || j.t[list].type != J_ARR) { json_free(&j); return NULL; }
  int count = j.t[list].size;
  LvPost *out = calloc(count ? count : 1, sizeof *out);
  char *tags = malloc(16384);
  if (!out || !tags) { free(out); free(tags); json_free(&j); return NULL; } // (no memory: no page, not a crash)
  char a[1024], b[1024], c[1024], r[32];
  int k = list + 1;
  for (int i = 0; i < count; i++, k = j.t[k].next) {
    if (j.t[k].type != J_OBJ) continue;
    LvPost p;
    memset(&p, 0, sizeof p);
    p.id = (long long)json_gn(&j, k, "id");
    const char *page_base = s->page_base ? s->page_base : s->base;
    char page[512];
    a[0] = b[0] = c[0] = tags[0] = 0;
    switch (s->kind) {
      case BK_DANBOORU: {
        // (the 360 px variant for the grid; the sample, ~850 px, for the full view)
        int vars = json_get(&j, json_get(&j, k, "media_asset"), "variants");
        for (int v = 0; vars >= 0 && v < j.t[vars].size; v++) {
          int x = json_at(&j, vars, v);
          char ty[16];
          json_gs(&j, x, "type", ty, sizeof ty);
          if (!strcmp(ty, "360x360")) json_gs(&j, x, "url", a, sizeof a);
        }
        if (!a[0]) json_gs(&j, k, "preview_file_url", a, sizeof a);
        json_gs(&j, k, "large_file_url", b, sizeof b);
        json_gs(&j, k, "file_url", c, sizeof c);
        json_gs(&j, k, "tag_string", tags, 16384);
        json_gs(&j, k, "file_ext", p.ext, sizeof p.ext);
        p.width = (int)json_gn(&j, k, "image_width");
        p.height = (int)json_gn(&j, k, "image_height");
        p.score = (int)json_gn(&j, k, "score");
        json_gs(&j, k, "rating", r, sizeof r);
        p.rating = r[0] == 'g' ? LV_R_GENERAL : r[0] == 's' ? LV_R_SENSITIVE : r[0] == 'q' ? LV_R_QUESTIONABLE : LV_R_EXPLICIT;
        snprintf(page, sizeof page, "%s/posts/%lld", page_base, p.id);
        break;
      }
      case BK_MOEBOORU:
        json_gs(&j, k, "preview_url", a, sizeof a);
        json_gs(&j, k, "sample_url", b, sizeof b);
        json_gs(&j, k, "file_url", c, sizeof c);
        json_gs(&j, k, "tags", tags, 16384);
        json_gs(&j, k, "file_ext", p.ext, sizeof p.ext);
        p.width = (int)json_gn(&j, k, "width");
        p.height = (int)json_gn(&j, k, "height");
        p.score = (int)json_gn(&j, k, "score");
        json_gs(&j, k, "rating", r, sizeof r);
        p.rating = rating_of(r);
        snprintf(page, sizeof page, "%s/post/show/%lld", page_base, p.id);
        break;
      case BK_E621: {
        int f = json_get(&j, k, "file"), sm = json_get(&j, k, "sample"), pv = json_get(&j, k, "preview");
        json_gs(&j, pv, "url", a, sizeof a);
        if (json_gn(&j, sm, "has")) json_gs(&j, sm, "url", b, sizeof b);
        json_gs(&j, f, "url", c, sizeof c);
        json_gs(&j, f, "ext", p.ext, sizeof p.ext);
        p.width = (int)json_gn(&j, f, "width");
        p.height = (int)json_gn(&j, f, "height");
        p.score = (int)json_gn(&j, json_get(&j, k, "score"), "total");
        e621_tags(&j, json_get(&j, k, "tags"), tags, 16384);
        json_gs(&j, k, "rating", r, sizeof r);
        p.rating = rating_of(r);
        snprintf(page, sizeof page, "%s/posts/%lld", page_base, p.id);
        break;
      }
      default: { // Gelbooru 0.2
        json_gs(&j, k, "preview_url", a, sizeof a);
        json_gs(&j, k, "sample_url", b, sizeof b);
        json_gs(&j, k, "file_url", c, sizeof c);
        // (older ones: only the folder and the file's name)
        if (!c[0]) {
          char dir[256], img[256];
          json_gs(&j, k, "directory", dir, sizeof dir);
          json_gs(&j, k, "image", img, sizeof img);
          if (dir[0] && img[0]) {
            char stem[256];
            snprintf(stem, sizeof stem, "%s", img);
            char *dot = strrchr(stem, '.');
            if (dot) *dot = 0;
            snprintf(c, sizeof c, "%s/images/%s/%s", page_base, dir, img);
            if (!a[0]) snprintf(a, sizeof a, "%s/thumbnails/%s/thumbnail_%s.jpg", page_base, dir, stem);
            if (!b[0] && json_gn(&j, k, "sample")) snprintf(b, sizeof b, "%s/samples/%s/sample_%s.jpg", page_base, dir, stem);
          }
        }
        json_gs(&j, k, "tags", tags, 16384);
        ext_of(c, p.ext);
        p.width = (int)json_gn(&j, k, "width");
        p.height = (int)json_gn(&j, k, "height");
        p.score = (int)json_gn(&j, k, "score");
        json_gs(&j, k, "rating", r, sizeof r);
        p.rating = rating_of(r);
        snprintf(page, sizeof page, "%s/index.php?page=post&s=view&id=%lld", page_base, p.id);
      }
    }
    // hidden from guests or deleted; minors; a rating the search didn't want
    if (!p.id || !c[0] || any_never(tags) || !(p.rating & ratings)) continue;
    if (!b[0] || (!pic(b) && !lv_post_is_video(&p))) snprintf(b, sizeof b, "%s", c);
    if (!a[0]) snprintf(a, sizeof a, "%s", pic(b) ? b : c);
    char src[512];
    json_gs(&j, k, "source", src, sizeof src);
    if (!src[0] && s->kind == BK_E621) json_str(&j, json_at(&j, json_get(&j, k, "sources"), 0), src, sizeof src);
    if (!p.ext[0]) ext_of(c, p.ext);
    p.preview = sdup(a);
    p.sample = sdup(b);
    p.file = sdup(c);
    p.page = sdup(page);
    p.source = sdup(src);
    p.tags = sdup(tags);
    out[(*n)++] = p;
  }
  free(tags);
  json_free(&j);
  return out;
}

void lv_posts_free(LvPost *p, int n) {
  for (int i = 0; i < n; i++) { free(p[i].preview); free(p[i].sample); free(p[i].file); free(p[i].page); free(p[i].source); free(p[i].tags); }
  free(p);
}
