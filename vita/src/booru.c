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
};
const int LV_NBUILTIN = 14;
int LV_NSITES = 14;
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
static int any_never(const char *tags) { return lv_booru_never_tags(tags); }
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
    const char *by = sort == LV_SORT_HOT ? "video_viewed_week" : sort == LV_SORT_TOP ? top[span >= 0 && span < LV_NSPANS ? span : 0]
                   : sort == LV_SORT_RANDOM ? "video_viewed" : "post_date";
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
    q.tags = sdup(tags);
    if (!q.preview || !q.file || !q.page || !q.tags) { free(q.preview); free(q.sample); free(q.file); free(q.page); free(q.source); free(q.tags); continue; }
    out[(*n)++] = q;
  }
  return out;
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
  // (the first, the smallest: 360p on most, played whole from the card, so it comes fastest; the
  //  Vita's screen is 544 lines)
  const char *keys[] = {"video_url:", "video_alt_url:"};
  for (int k = 0; k < 2; k++) {
    const char *v = strstr(html, keys[k]);
    if (!v) continue;
    v += strlen(keys[k]);
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

LvPost *lv_booru_parse(const LvSite *s, const char *text, size_t len, int ratings, int *n) {
  *n = 0;
  if (s->kind == BK_KVS) return kvs_parse(text, len, n);
  Json j;
  if (json_parse(&j, text, len) != 0) return NULL;
  int list = 0;
  if (s->kind == BK_E621) list = json_get(&j, 0, "posts");
  else if (j.t[0].type == J_OBJ) list = json_get(&j, 0, "post"); // (some Gelbooru answers wrap the list)
  if (list < 0 || j.t[list].type != J_ARR) { json_free(&j); return NULL; }
  int count = j.t[list].size;
  LvPost *out = calloc(count ? count : 1, sizeof *out);
  char *tags = malloc(16384);
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
