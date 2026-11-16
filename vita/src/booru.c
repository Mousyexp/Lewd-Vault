#include "booru.h"
#include "json.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// No account needed for these, but Gelbooru and Rule34 (the last two) want an account's API key.
const LvSite LV_SITES[] = {
  {"Danbooru", BK_DANBOORU, "https://danbooru.donmai.us", NULL, 1, 0, "danbooru", "Anime art, finely tagged"},
  {"Yande.re", BK_MOEBOORU, "https://yande.re", NULL, 0, 0, "yandere", "High-resolution anime scans"},
  {"Konachan", BK_MOEBOORU, "https://konachan.com", NULL, 0, 0, "konachan", "Anime wallpapers"},
  {"e621", BK_E621, "https://e621.net", NULL, 1, 0, "e621", "Furry art"},
  {"Xbooru", BK_GELBOORU, "https://xbooru.com", NULL, 0, 0, "xbooru", "Hentai"},
  {"TBIB", BK_GELBOORU, "https://tbib.org", NULL, 0, 0, "tbib", "The big imageboard"},
  {"Hypnohub", BK_GELBOORU, "https://hypnohub.net", NULL, 0, 0, "hypnohub", "Hypnosis art"},
  {"AIBooru", BK_DANBOORU, "https://aibooru.online", NULL, 1, 0, "aibooru", "AI-made anime art"},
  {"Safebooru", BK_GELBOORU, "https://safebooru.org", NULL, 0, 0, "safebooru", "Anime art, all ages"},
  {"e926", BK_E621, "https://e926.net", NULL, 1, 0, "e926", "Furry art, all ages"},
  {"Gelbooru", BK_GELBOORU, "https://gelbooru.com", NULL, 0, 1, "gelbooru", "Anime and hentai (API key)"},
  {"Rule34", BK_GELBOORU, "https://api.rule34.xxx", "https://rule34.xxx", 0, 1, "rule34", "Rule 34, games and anime (API key)"},
  {"Sakugabooru", BK_MOEBOORU, "https://www.sakugabooru.com", NULL, 0, 0, "sakugabooru", "Anime animation clips (videos)"},
};
const int LV_NSITES = sizeof LV_SITES / sizeof *LV_SITES;
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
static int any_never(const char *tags) {
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
  snprintf(q, sizeof q, "%s", clean);
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

LvPost *lv_booru_parse(const LvSite *s, const char *text, size_t len, int ratings, int *n) {
  *n = 0;
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
