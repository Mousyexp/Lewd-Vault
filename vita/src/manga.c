#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "manga.h"
#include "json.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// (the same words the Browser never shows, ui/discover.js's NEVER, with each site's own for them)
static const char *NEVER[] = {"loli", "lolicon", "lolis", "shota", "shotacon", "shotas", "toddlercon", "toddler", "child", "children",
                              "underage", "preteen", "cub", "aged_down", "aged down", "kid", "kids"};
int lv_manga_never(const char *tags) {
  // word by word: letters and digits make a word; anything else ends one ("female:oppai loli|Loli")
  const char *p = tags;
  while (*p) {
    while (*p && !isalnum((unsigned char)*p)) p++;
    const char *e = p;
    while (*e && isalnum((unsigned char)*e)) e++;
    size_t n = (size_t)(e - p);
    for (size_t k = 0; n && k < sizeof NEVER / sizeof *NEVER; k++)
      if (strlen(NEVER[k]) == n && !strncasecmp(NEVER[k], p, n)) return 1;
    p = e;
  }
  // (and the two-word ones)
  return strcasestr(tags, "aged down") != NULL;
}

static void enc(const char *s, char *out, size_t cap) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p && o + 4 < cap; p++) {
    if (isalnum(*p) || strchr("-_.~", *p)) out[o++] = (char)*p;
    else o += (size_t)snprintf(out + o, cap - o, "%%%02X", *p);
  }
  out[o] = 0;
}
static void unescape_html(char *s) {
  static const struct { const char *e; char c; } E[] = {{"&amp;", '&'}, {"&quot;", '"'}, {"&#039;", '\''}, {"&#39;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}};
  char *o = s;
  for (char *p = s; *p;) {
    int done = 0;
    for (size_t k = 0; k < sizeof E / sizeof *E && !done; k++) {
      size_t n = strlen(E[k].e);
      if (!strncmp(p, E[k].e, n)) { *o++ = E[k].c; p += n; done = 1; }
    }
    // (a numbered one, &#038; or &#x27;: as UTF-8)
    if (!done && p[0] == '&' && p[1] == '#') {
      char *e;
      long c = p[2] == 'x' || p[2] == 'X' ? strtol(p + 3, &e, 16) : strtol(p + 2, &e, 10);
      if (*e == ';' && c > 0 && c < 0x110000) {
        if (c < 0x80) *o++ = (char)c;
        else if (c < 0x800) { *o++ = (char)(0xC0 | c >> 6); *o++ = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { *o++ = (char)(0xE0 | c >> 12); *o++ = (char)(0x80 | (c >> 6 & 63)); *o++ = (char)(0x80 | (c & 63)); }
        else { *o++ = (char)(0xF0 | c >> 18); *o++ = (char)(0x80 | (c >> 12 & 63)); *o++ = (char)(0x80 | (c >> 6 & 63)); *o++ = (char)(0x80 | (c & 63)); }
        p = e + 1;
        done = 1;
      }
    }
    if (!done) *o++ = *p++;
  }
  *o = 0;
}
static const char *RATINGS_ALL = "&contentRating[]=safe&contentRating[]=suggestive&contentRating[]=erotica&contentRating[]=pornographic";
static const char *RATINGS_SAFE = "&contentRating[]=safe&contentRating[]=suggestive";

// ---------- MangaDex ----------
void lv_md_list_url(const char *query, int adult, int order, int offset, char *out, size_t cap) {
  static const char *ORDER[] = {"&order[followedCount]=desc", "&order[latestUploadedChapter]=desc", "&order[createdAt]=desc"};
  char q[300] = "";
  if (query && query[0]) { char e[240]; enc(query, e, sizeof e); snprintf(q, sizeof q, "&title=%s", e); }
  snprintf(out, cap, "https://api.mangadex.org/manga?limit=30&offset=%d&includes[]=cover_art&hasAvailableChapters=true&availableTranslatedLanguage[]=en%s%s%s",
           offset, adult ? RATINGS_ALL : RATINGS_SAFE, order >= 0 && order < 3 ? ORDER[order] : ORDER[0], q);
}
// a title: English if there is one, else the first
static void md_title(const Json *j, int t, char *out, size_t cap) {
  out[0] = 0;
  json_gs(j, t, "en", out, cap);
  if (!out[0] && t >= 0 && j->t[t].type == J_OBJ && j->t[t].size) json_str(j, json_at(j, t, 0), out, cap);
}
int lv_md_parse_list(const char *json, size_t n, LvBook *out, int cap, int *total) {
  Json j;
  if (json_parse(&j, json, n) != 0) return -1;
  if (total) *total = (int)json_gn(&j, 0, "total");
  int data = json_get(&j, 0, "data"), got = 0;
  // (item to item along the list: not the k-th looked for from the start each time)
  for (int k = 0, m = data >= 0 ? data + 1 : -1; data >= 0 && j.t[data].type == J_ARR && k < j.t[data].size && got < cap; k++, m = j.t[m].next) {
    int a = json_get(&j, m, "attributes");
    LvBook b;
    memset(&b, 0, sizeof b);
    json_gs(&j, m, "id", b.id, sizeof b.id);
    md_title(&j, json_get(&j, a, "title"), b.title, sizeof b.title);
    int tags = json_get(&j, a, "tags");
    size_t o = 0;
    for (int t = 0; tags >= 0 && t < j.t[tags].size; t++) {
      char name[64];
      json_gs(&j, json_get(&j, json_get(&j, json_at(&j, tags, t), "attributes"), "name"), "en", name, sizeof name);
      int w = snprintf(b.tags + o, sizeof b.tags - o, "%s%s", o ? "|" : "", name);
      if (w > 0 && o + (size_t)w < sizeof b.tags) o += (size_t)w;
    }
    int rel = json_get(&j, m, "relationships");
    for (int r = 0; rel >= 0 && r < j.t[rel].size; r++) {
      int x = json_at(&j, rel, r);
      char ty[24], file[160];
      json_gs(&j, x, "type", ty, sizeof ty);
      if (strcmp(ty, "cover_art")) continue;
      json_gs(&j, json_get(&j, x, "attributes"), "fileName", file, sizeof file);
      // (the 256 px one: a card is smaller than that)
      if (file[0]) snprintf(b.cover, sizeof b.cover, "https://uploads.mangadex.org/covers/%s/%s.256.jpg", b.id, file);
    }
    if (!b.id[0] || lv_manga_never(b.tags) || lv_manga_never(b.title)) continue;
    out[got++] = b;
  }
  json_free(&j);
  return got;
}
void lv_md_feed_url(const char *manga_id, int adult, int offset, char *out, size_t cap) { lv_md_feed_url_lang(manga_id, adult, offset, 0, out, cap); }
// any_lang: every language's chapters (a title whose English ones are all on a publisher's site often
// has others, Spanish, Portuguese, Russian...: read rather than nothing)
void lv_md_feed_url_lang(const char *manga_id, int adult, int offset, int any_lang, char *out, size_t cap) {
  snprintf(out, cap, "https://api.mangadex.org/manga/%s/feed?%sorder[volume]=asc&order[chapter]=asc&limit=100&offset=%d%s",
           manga_id, any_lang ? "" : "translatedLanguage[]=en&", offset, adult ? RATINGS_ALL : RATINGS_SAFE);
}
int lv_md_parse_feed(const char *json, size_t n, LvChapter *out, int cap, int *total) {
  Json j;
  if (json_parse(&j, json, n) != 0) return -1;
  if (total) *total = (int)json_gn(&j, 0, "total");
  int data = json_get(&j, 0, "data"), got = 0;
  for (int k = 0, c = data >= 0 ? data + 1 : -1; data >= 0 && j.t[data].type == J_ARR && k < j.t[data].size && got < cap; k++, c = j.t[c].next) {
    int a = json_get(&j, c, "attributes");
    int ext = json_get(&j, a, "externalUrl");
    if (ext >= 0 && j.t[ext].type == J_STR) continue; // (hosted elsewhere: nothing to read here)
    LvChapter ch;
    memset(&ch, 0, sizeof ch);
    ch.pages = (int)json_gn(&j, a, "pages");
    if (ch.pages <= 0) continue;
    json_gs(&j, c, "id", ch.id, sizeof ch.id);
    char num[16] = "", title[96] = "";
    int tn = json_get(&j, a, "chapter"), tt = json_get(&j, a, "title");
    if (tn >= 0 && j.t[tn].type == J_STR) json_str(&j, tn, num, sizeof num);
    if (tt >= 0 && j.t[tt].type == J_STR) json_str(&j, tt, title, sizeof title);
    if (num[0] && title[0]) snprintf(ch.label, sizeof ch.label, "Chapter %s: %s", num, title);
    else if (num[0]) snprintf(ch.label, sizeof ch.label, "Chapter %s", num);
    else snprintf(ch.label, sizeof ch.label, "%s", title[0] ? title : "One-shot");
    // (another language than English: said, after it)
    char lang[16] = "";
    json_gs(&j, a, "translatedLanguage", lang, sizeof lang);
    if (lang[0] && strcmp(lang, "en")) { size_t l = strlen(ch.label); snprintf(ch.label + l, sizeof ch.label - l, "  \u00b7 %s", lang); }
    out[got++] = ch;
  }
  json_free(&j);
  return got;
}
void lv_md_pages_url(const char *chapter_id, char *out, size_t cap) { snprintf(out, cap, "https://api.mangadex.org/at-home/server/%s", chapter_id); }
int lv_md_parse_pages(const char *json, size_t n, char ***urls) {
  *urls = NULL;
  Json j;
  if (json_parse(&j, json, n) != 0) return -1;
  char base[200], hash[80];
  json_gs(&j, 0, "baseUrl", base, sizeof base);
  int ch = json_get(&j, 0, "chapter");
  json_gs(&j, ch, "hash", hash, sizeof hash);
  int list = json_get(&j, ch, "dataSaver");
  int cnt = list >= 0 ? j.t[list].size : 0, got = 0;
  char **u = cnt ? calloc(cnt, sizeof *u) : NULL;
  for (int k = 0; u && base[0] && hash[0] && k < cnt; k++) {
    char f[200], url[520];
    json_str(&j, json_at(&j, list, k), f, sizeof f);
    snprintf(url, sizeof url, "%s/data-saver/%s/%s", base, hash, f);
    u[got] = strdup(url);
    if (u[got]) got++;
  }
  json_free(&j);
  if (!got) { free(u); return -1; }
  *urls = u;
  return got;
}

// ---------- E-Hentai ----------
void lv_eh_list_url(const char *query, int page, char *out, size_t cap) {
  // f_cats: the categories left out (Misc, Image Set, Cosplay, Asian Porn, Non-H, Western), and
  // never the tags for minors
  char q[400], e[1200];
  snprintf(q, sizeof q, "%s -\"female:lolicon$\" -\"male:shotacon$\" -\"female:toddlercon$\" -\"male:toddlercon$\"", query ? query : "");
  enc(q, e, sizeof e);
  snprintf(out, cap, "https://e-hentai.org/?f_cats=993&page=%d&f_search=%s", page, e);
}
int lv_eh_parse_list(const char *html, long long *gids, char (*tokens)[16], int cap) {
  int n = 0;
  for (const char *p = strstr(html, "https://e-hentai.org/g/"); p && n < cap; p = strstr(p + 1, "https://e-hentai.org/g/")) {
    const char *q = p + 23;
    char *end;
    long long gid = strtoll(q, &end, 10);
    if (end == q || *end != '/') continue;
    q = end + 1;
    size_t tl = strspn(q, "0123456789abcdef");
    if (tl < 6 || tl > 15 || q[tl] != '/') continue;
    int dup = 0;
    for (int k = 0; k < n; k++) if (gids[k] == gid) dup = 1;
    if (dup) continue;
    gids[n] = gid;
    memcpy(tokens[n], q, tl);
    tokens[n][tl] = 0;
    n++;
  }
  return n;
}
void lv_eh_gdata_body(const long long *gids, char (*tokens)[16], int n, char *out, size_t cap) {
  size_t o = (size_t)snprintf(out, cap, "{\"method\":\"gdata\",\"namespace\":1,\"gidlist\":[");
  for (int k = 0; k < n && o + 40 < cap; k++) o += (size_t)snprintf(out + o, cap - o, "%s[%lld,\"%s\"]", k ? "," : "", gids[k], tokens[k]);
  snprintf(out + o, cap - o, "]}");
}
int lv_eh_parse_gdata(const char *json, size_t n, LvBook *out, int cap) {
  Json j;
  if (json_parse(&j, json, n) != 0) return -1;
  int list = json_get(&j, 0, "gmetadata"), got = 0;
  for (int k = 0, g = list >= 0 ? list + 1 : -1; list >= 0 && j.t[list].type == J_ARR && k < j.t[list].size && got < cap; k++, g = j.t[g].next) {
    if (json_get(&j, g, "error") >= 0) continue;
    LvBook b;
    memset(&b, 0, sizeof b);
    snprintf(b.id, sizeof b.id, "%lld", (long long)json_gn(&j, g, "gid"));
    json_gs(&j, g, "token", b.token, sizeof b.token);
    json_gs(&j, g, "title", b.title, sizeof b.title);
    unescape_html(b.title);
    json_gs(&j, g, "thumb", b.cover, sizeof b.cover);
    char fc[16];
    json_gs(&j, g, "filecount", fc, sizeof fc);
    b.pages = atoi(fc);
    int tags = json_get(&j, g, "tags");
    size_t o = 0;
    for (int t = 0; tags >= 0 && t < j.t[tags].size; t++) {
      char tg[96];
      json_str(&j, json_at(&j, tags, t), tg, sizeof tg);
      int w = snprintf(b.tags + o, sizeof b.tags - o, "%s%s", o ? "|" : "", tg);
      if (w > 0 && o + (size_t)w < sizeof b.tags) o += (size_t)w;
    }
    if (!b.token[0] || lv_manga_never(b.tags)) continue;
    out[got++] = b;
  }
  json_free(&j);
  return got;
}
void lv_eh_gallery_url(const LvBook *b, int p, char *out, size_t cap) { snprintf(out, cap, "https://e-hentai.org/g/%s/%s/?p=%d", b->id, b->token, p); }
int lv_eh_parse_gallery(const char *html, const char *gid, char ***urls) {
  *urls = NULL;
  char key[40];
  snprintf(key, sizeof key, "/%s-", gid);
  int n = 0, cap = 0;
  char **u = NULL;
  for (const char *p = strstr(html, "https://e-hentai.org/s/"); p; p = strstr(p + 1, "https://e-hentai.org/s/")) {
    size_t l = strcspn(p, "\"' >");
    if (l > 200) continue;
    char url[220];
    memcpy(url, p, l);
    url[l] = 0;
    if (!strstr(url, key)) continue;
    int dup = 0;
    for (int k = 0; k < n && !dup; k++) if (!strcmp(u[k], url)) dup = 1;
    if (dup) continue;
    if (n == cap) { int c = cap ? cap * 2 : 64; char **x = realloc(u, c * sizeof *x); if (!x) break; u = x; cap = c; }
    if ((u[n] = strdup(url))) n++;
  }
  *urls = u;
  return n;
}
int lv_eh_parse_page(const char *html, char *out, size_t cap) {
  out[0] = 0;
  const char *p = strstr(html, "<img id=\"img\" src=\"");
  if (!p) return -1;
  p += 19;
  size_t l = strcspn(p, "\"");
  if (l >= cap) return -1;
  memcpy(out, p, l);
  out[l] = 0;
  unescape_html(out);
  return 0;
}
int lv_eh_parse_reload(const char *html, char *out, size_t cap) {
  out[0] = 0;
  const char *p = strstr(html, "nl('");
  if (!p) return -1;
  p += 4;
  size_t l = strcspn(p, "'");
  if (!l || l >= cap) return -1;
  memcpy(out, p, l);
  out[l] = 0;
  return 0;
}
void lv_strings_free(char **s, int n) {
  for (int k = 0; s && k < n; k++) free(s[k]);
  free(s);
}

// ======================= the other sites =======================
const char *const LV_MK_NAME[MK_N] = {"MANGA", "H MANGA", "H WEBTOON", "SHORT COMICS"};
const LvMSrc LV_MSRC[MS_N] = {
  {"MangaDex", "mangadex", "Manga of all kinds, 18+ included", MF_MANGADEX, MK_MANGA},
  {"E-Hentai", "ehentai", "Doujinshi, hentai manga, CG", MF_EHENTAI, MK_HMANGA},
  {"HentaiFox", "hentaifox", "Hentai manga and doujinshi", MF_GALLERY, MK_HMANGA, "https://hentaifox.com", "/?page=%d", "/search/?q=%s&page=%d", "/gallery/"},
  {"AsmHentai", "asmhentai", "Doujinshi, many in English", MF_GALLERY, MK_HMANGA, "https://asmhentai.com", "/?page=%d", "/search/?q=%s&page=%d", "/g/"},
  {"HentaiEra", "hentaiera", "Hentai manga, new every day", MF_GALLERY, MK_HMANGA, "https://hentaiera.com", "/?page=%d", "/search/?key=%s&page=%d", "/gallery/"},
  {"nhentai.xxx", "nhentaixxx", "Doujinshi and hentai manga", MF_GALLERY, MK_HMANGA, "https://nhentai.xxx", "/?page=%d", "/search/?key=%s&page=%d", "/g/"},
  {"HentaiZap", "hentaizap", "Hentai manga, short and long", MF_GALLERY, MK_HMANGA, "https://hentaizap.com", "/?page=%d", "/search/?key=%s&page=%d", "/gallery/"},
  {"HentaiEnvy", "hentaienvy", "Hentai manga and artist CG", MF_GALLERY, MK_HMANGA, "https://hentaienvy.com", "/?page=%d", "/search/?key=%s&page=%d", "/gallery/"},
  {"3Hentai", "3hentai", "Doujinshi, lots in English", MF_GALLERY, MK_HMANGA, "https://3hentai.net", "/%d", "/search?q=%s&page=%d", "/d/"},
  {"HentaiHand", "hentaihand", "Hentai manga, a huge library", MF_HHAND, MK_HMANGA, "https://hentaihand.com", "/api/comics?page=%d", "/api/comics?q=%s&page=%d"},
  {"Manhwa18", "manhwa18", "Adult webtoons (manhwa)", MF_SERIES, MK_HWEBTOON, "https://manhwa18.cc", "/page/%d", "/search?q=%s&page=%d", "/webtoon/", NULL, "/chapters/|/uploads/"},
  {"HentaiWebtoon", "hentaiwebtoon", "Webtoons, adult ones among them", MF_SERIES, MK_HWEBTOON, "https://hentaiwebtoon.com", NULL, "/manga/?q=%s", "/manga/", NULL, "/assets/images/chapters/"},
  {"MangaDistrict", "mangadistrict", "Adult manhwa and manga", MF_SERIES, MK_HWEBTOON, "https://mangadistrict.com", "/page/%d/", "/page/%d/?s=%s&post_type=wp-manga", "/series/", NULL, "/publication/manga_", 0, 1},
  {"Multporn", "multporn", "Short hentai and western comics", MF_SERIES, MK_SHORT, "https://multporn.net", "/updated_comics?page=%d", "/search?search_api_views_fulltext=%s&page=%d", "/comics/", "/hentai_manga/", "/juicebox_medium/", 1, 0, 1},
  {"FreeAdultComix", "freeadultcomix", "Short adult comics", MF_SERIES, MK_SHORT, "https://freeadultcomix.com", "/page/%d/", "/page/%d/?s=%s", "*", NULL, "/wp-content/uploads/", 1},
  {"HentaiRox", "hentairox", "Hentai manga and doujinshi", MF_GALLERY, MK_HMANGA, "https://hentairox.com", "/?page=%d", "/search/?q=%s&page=%d", "/gallery/"},
  {"Pururin", "pururin", "Doujinshi, English and Japanese", MF_GALLERY, MK_HMANGA, "https://pururin.me", "/?page=%d", "/search?q=%s&page=%d", "/gallery/+"},
  {"HentaiManga", "hentaimanga", "Hentai manga, in chapters", MF_SERIES, MK_HMANGA, "https://hentaimanga.me", NULL, "/manga/?q=%s", "/manga/", NULL, "/assets/images/chapters/"},
  {"Manga18fx", "manga18fx", "Adult webtoons, some raw (Korean)", MF_SERIES, MK_HWEBTOON, "https://manga18fx.com", "/page/%d", "/search?q=%s&page=%d", "/manga/", NULL, "/chapters/|/uploads/"},
  {"ManhwaDen", "manhwaden", "Adult manhwa", MF_SERIES, MK_HWEBTOON, "https://www.manhwaden.com", "/page/%d/", "/page/%d/?s=%s&post_type=wp-manga", "/manga/", NULL, "-Chapter-|/WP-manga/data/|/manga_", 0, 1},
};

void lv_ms_url(int src, const char *path, char *out, size_t cap) {
  if (!strncmp(path, "http", 4)) snprintf(out, cap, "%s", path);
  else if (!strncmp(path, "//", 2)) snprintf(out, cap, "https:%s", path);
  else snprintf(out, cap, "%s%s%s", LV_MSRC[src].base, path[0] == '/' ? "" : "/", path);
}
void lv_ms_list_url(int src, const char *query, int page, char *out, size_t cap) {
  const LvMSrc *s = &LV_MSRC[src];
  int pg = s->page0 ? page - 1 : page;
  char path[600];
  if (query && query[0]) {
    char e[300];
    enc(query, e, sizeof e);
    // (the search's words and page, in whichever order its address has them)
    const char *fs = strstr(s->search, "%s"), *fd = strstr(s->search, "%d");
    if (fd && fd < fs) snprintf(path, sizeof path, s->search, pg, e);
    else if (fd) snprintf(path, sizeof path, s->search, e, pg);
    else snprintf(path, sizeof path, s->search, e);
  } else if (s->list) snprintf(path, sizeof path, s->list, pg);
  else snprintf(path, sizeof path, "%s", s->item);
  snprintf(out, cap, "%s%s", s->base, path);
}

// --- small helpers on HTML ---
// the value of an attribute inside the tag starting at p (to its '>'): 0 and the value, or -1
static int attr(const char *tag, const char *name, char *out, size_t cap) {
  const char *end = strchr(tag, '>');
  size_t nl = strlen(name);
  for (const char *p = tag; p && end && p < end; p++) {
    if (strncmp(p, name, nl) || p[nl] != '=' || (p > tag && !isspace((unsigned char)p[-1]))) continue;
    const char *v = p + nl + 1;
    char q = *v == '"' || *v == '\'' ? *v++ : ' ';
    const char *e = v;
    while (*e && *e != q && (q != ' ' || (*e != '>' && !isspace((unsigned char)*e)))) e++;
    snprintf(out, cap, "%.*s", (int)(e - v), v);
    return 0;
  }
  return -1;
}
// an <img>'s picture: its lazy address first (data-src, data-lazy-src, data-original), else src
static int img_src(const char *tag, char *out, size_t cap) {
  // (a set of sizes: the one nearest 800 wide, enough for the Vita's screen, and a fraction of the
  //  biggest's download)
  static const char *const SS[] = {"data-jg-srcset", "data-srcset", "srcset", NULL};
  char set[3000];
  for (int k = 0; SS[k]; k++) {
    if (attr(tag, SS[k], set, sizeof set) != 0 || !set[0]) continue;
    unescape_html(set);
    int best_d = 1 << 30;
    out[0] = 0;
    for (char *e = strtok(set, ","); e; e = strtok(NULL, ",")) {
      while (isspace((unsigned char)*e)) e++;
      char u[600]; int w = 0;
      if (sscanf(e, "%599s %dw", u, &w) < 1 || !strncmp(u, "data:", 5)) continue;
      int d = w ? (w >= 700 ? w - 800 : (800 - w) * 3) : 5000;
      if (d < 0) d = -d;
      if (d < best_d) { best_d = d; snprintf(out, cap, "%s", u); }
    }
    if (out[0]) return 0;
  }
  static const char *const A[] = {"data-src", "data-lazy-src", "data-original", "src", NULL};
  for (int k = 0; A[k]; k++)
    if (attr(tag, A[k], out, cap) == 0 && out[0] && strncmp(out, "data:", 5)) {
      // (spaces around it, and its own &amp;s)
      char *s = out; while (isspace((unsigned char)*s)) s++;
      memmove(out, s, strlen(s) + 1);
      for (size_t n = strlen(out); n && isspace((unsigned char)out[n - 1]); n--) out[n - 1] = 0;
      unescape_html(out);
      return 0;
    }
  return -1;
}
// the text of an element from p (after its tag), tags inside skipped, spaces tidied
static void inner_text(const char *p, char *out, size_t cap) {
  size_t n = 0;
  int sp = 0;
  while (*p && n + 1 < cap) {
    if (*p == '<') {
      // (the element's end: its closing tag of a block)
      if (!strncmp(p, "</a", 3) || !strncmp(p, "</h", 3) || !strncmp(p, "</div", 5) || !strncmp(p, "</span", 6) || !strncmp(p, "</p", 3)) { if (n) break; }
      const char *e = strchr(p, '>');
      if (!e) break;
      p = e + 1;
      continue;
    }
    if (isspace((unsigned char)*p)) { sp = n > 0; p++; continue; }
    if (sp) { out[n++] = ' '; sp = 0; }
    if (n + 1 < cap) out[n++] = *p;
    p++;
  }
  out[n] = 0;
  unescape_html(out);
}
// the path of an address on the source's site (its own site's prefix taken off), or NULL
static const char *site_path(int src, const char *href) {
  const char *b = LV_MSRC[src].base;
  size_t bl = strlen(b);
  if (!strncmp(href, b, bl)) return href + bl;
  if (!strncmp(href, "https://www.", 12) && !strncmp(href + 12, b + 8, bl - 8)) return href + 12 + bl - 8;
  if (href[0] == '/' && href[1] != '/') return href;
  return NULL;
}
// a book's path: the item prefix and one more part ("/gallery/123/", "/webtoon/name"), else 0
static int book_path(int src, const char *path, char *out, size_t cap) {
  const LvMSrc *s = &LV_MSRC[src];
  const char *pre[2] = {s->item, s->item2};
  for (int k = 0; k < 2; k++) {
    const char *it = pre[k];
    if (!it) continue;
    const char *rest;
    if (!strcmp(it, "*")) {
      // (any one-part path, but not the site's own pages)
      static const char *const NOT[] = {"/page/", "/category/", "/tag/", "/author/", "/wp-", "/feed", "/search", "/comments", "/privacy", "/contact", "/dmca", "/terms", "/about", "/2257", "/advertis", "/upload", NULL};
      if (path[0] != '/' || !path[1]) continue;
      int bad = 0;
      for (int j = 0; NOT[j] && !bad; j++) bad = !strncmp(path, NOT[j], strlen(NOT[j]));
      if (bad) continue;
      rest = path + 1;
    } else {
      size_t l = strlen(it);
      // ("/gallery/+": the whole path is the book's, a name after its number too: Pururin's)
      if (l > 1 && it[l - 1] == '+') {
        if (strncmp(path, it, l - 1) || !path[l - 1]) continue;
        size_t all = strcspn(path, "?#\"");
        if (all <= l) continue;
        snprintf(out, cap, "%.*s", (int)all, path);
        return 1;
      }
      if (strncmp(path, it, l) || !path[l]) continue;
      rest = path + l;
    }
    size_t seg = strcspn(rest, "/?#\"");
    if (seg < 2) continue;
    // (nothing after that part but a slash)
    if (rest[seg] == '/' && rest[seg + 1] && rest[seg + 1] != '"' && rest[seg + 1] != '?' && rest[seg + 1] != '#') continue;
    if (rest[seg] == '?' || rest[seg] == '#') continue;
    snprintf(out, cap, "%.*s%s", (int)(rest - path + seg), path, rest[seg] == '/' ? "/" : "");
    return 1;
  }
  return 0;
}
// the tags or genres a page links to, joined, for the never-shown check
// where a page's "related galleries" block starts (class="related_galleries", id="related-..."):
// the other galleries' tags there aren't this one's
static const char *related_start(const char *html) {
  for (const char *p = strstr(html, "related"); p; p = strstr(p + 7, "related")) {
    const char *q = p;
    while (q > html && q[-1] != '"' && q[-1] != '<' && q[-1] != '>' && p - q < 60) q--;
    if (q - html >= 7 && q[-1] == '"' && (!strncmp(q - 7, "class=\"", 7) || !strncmp(q - 4, "id=\"", 4))) return p;
  }
  return NULL;
}
static void page_tags(const char *html, char *out, size_t cap) {
  size_t n = 0;
  out[0] = 0;
  static const char *const T[] = {"/tag/", "/tags/", "/genre/", "/genres/", "/manga-genre/", "/category/", "/parody/", "/character/", NULL};
  const char *end = related_start(html);
  for (const char *p = strstr(html, "<a "); p && n + 64 < cap && (!end || p < end); p = strstr(p + 3, "<a ")) {
    char href[300];
    if (attr(p, "href", href, sizeof href) != 0) continue;
    int ok = 0;
    for (int k = 0; T[k] && !ok; k++) ok = strstr(href, T[k]) != NULL;
    if (!ok) continue;
    const char *gt = strchr(p, '>');
    if (!gt) break;
    char t[80];
    inner_text(gt + 1, t, sizeof t);
    // (the link's own last part too: "/tag/lolicon/")
    const char *slug = strstr(href, "/tag");
    n += snprintf(out + n, cap - n, "%s %s ", t, slug ? slug : href);
  }
}

// a link's text that isn't a name: "Read now →", "Read", "Chapter 12", a number
static int weak_title(const char *t) {
  if (strlen(t) < 3) return 1;
  int alnum = 0; // (only symbols or an emoji: not a name)
  for (const char *c = t; *c && !alnum; c++) alnum = isalnum((unsigned char)*c) || (unsigned char)*c >= 0xC0;
  if (!alnum) return 1;
  if ((unsigned char)t[0] >= 0xF0 && strlen(t) <= 8) return 1;
  if (!strncasecmp(t, "read", 4) || !strncasecmp(t, "chapter", 7) || !strncasecmp(t, "ch.", 3) || !strncasecmp(t, "view", 4)) return 1;
  return isdigit((unsigned char)t[0]) && strspn(t, "0123456789 .,") == strlen(t);
}
int lv_ms_parse_list(int src, const char *html, LvBook *out, int cap) {
  int got = 0;
  for (const char *p = strstr(html, "<a "); p && got < cap; p = strstr(p + 3, "<a ")) {
    char href[400], path[200];
    if (attr(p, "href", href, sizeof href) != 0) continue;
    const char *sp = site_path(src, href);
    if (!sp || !book_path(src, sp, path, sizeof path)) continue;
    // (once each: a card links to its book several times)
    int dup = -1;
    for (int k = 0; k < got; k++) if (!strcmp(out[k].id, path)) dup = k;
    LvBook *b = dup >= 0 ? &out[dup] : &out[got];
    if (dup < 0) { memset(b, 0, sizeof *b); snprintf(b->id, sizeof b->id, "%s", path); }
    const char *gt = strchr(p, '>');
    if (!gt) break;
    // its cover: the first picture in or right after the link (within the card), else just before it
    if (!b->cover[0]) {
      const char *im = strstr(gt, "<img"), *close = strstr(gt, "</a>");
      if (im && close && im < close + 600 && im - gt < 1500) img_src(im, b->cover, sizeof b->cover);
      if (!b->cover[0]) {
        const char *q = p;
        for (int k = 0; k < 1200 && q > html; k++, q--) if (!strncmp(q, "<img", 4)) { img_src(q, b->cover, sizeof b->cover); break; }
      }
      if (b->cover[0] && strstr(b->cover, "logo")) b->cover[0] = 0;
    }
    // its name: the link's title, the link's text, or the picture's alt, or a title after it (a
    // button's "Read now" replaced by a better one further on)
    if (!b->title[0] || weak_title(b->title)) {
      char t[200] = "";
      if (attr(p, "title", t, sizeof t) == 0) unescape_html(t);
      if (!t[0]) inner_text(gt + 1, t, sizeof t);
      if (!t[0]) { const char *im = strstr(gt, "<img"); if (im && im - gt < 400) { attr(im, "alt", t, sizeof t); unescape_html(t); } }
      if (!t[0] || strlen(t) < 2) {
        const char *ti = strstr(gt, "title");
        if (ti && ti - gt < 1500) { const char *e = strchr(ti, '>'); if (e) inner_text(e + 1, t, sizeof t); }
      }
      if (!b->title[0] || !weak_title(t)) snprintf(b->title, sizeof b->title, "%s", t);
    }
    if (dup < 0) got++;
  }
  for (int k = 0; k < got; k++) {
    if (out[k].cover[0]) { char c[400]; lv_ms_url(src, out[k].cover, c, sizeof c); snprintf(out[k].cover, sizeof out[k].cover, "%s", c); }
    // (no better name found: one made from its address, "the-chairmans-son" "The chairmans son")
    if (!out[k].title[0] || weak_title(out[k].title)) {
      const char *e = out[k].id + strlen(out[k].id);
      while (e > out[k].id && e[-1] == '/') e--;
      const char *s0 = e;
      while (s0 > out[k].id && s0[-1] != '/') s0--;
      char t[200];
      snprintf(t, sizeof t, "%.*s", (int)(e - s0), s0);
      for (char *c = t; *c; c++) if (*c == '-' || *c == '_') *c = ' ';
      if (t[0] >= 'a' && t[0] <= 'z') t[0] -= 32;
      if (!isdigit((unsigned char)t[0]) || LV_MSRC[src].family != MF_GALLERY) snprintf(out[k].title, sizeof out[k].title, "%s", t);
    }
  }
  // (the never-shown words in a title: left out; a book with no cover or name is the site's own link)
  int kept = 0, any_cover = 0;
  for (int k = 0; k < got; k++) any_cover |= out[k].cover[0] != 0;
  for (int k = 0; k < got; k++) {
    if (!out[k].title[0] || weak_title(out[k].title) || lv_manga_never(out[k].title)) continue;
    if (!out[k].cover[0] && any_cover) continue; // (a carousel's or a menu's link: the list's cards have covers)
    out[kept++] = out[k];
  }
  return kept;
}

// the first number from p (past any tags or words, at most 120 characters on), "1,391" read as 1391
static int num_at(const char *p) {
  for (int k = 0; k < 120 && *p && !isdigit((unsigned char)*p); k++) p++;
  int n = 0;
  for (; isdigit((unsigned char)*p) || (*p == ',' && isdigit((unsigned char)p[1])); p++) if (*p != ',') n = n * 10 + (*p - '0');
  return n;
}
static char **urls_new(int n) { return calloc(n > 0 ? n : 1, sizeof(char *)); }
int lv_ms_parse_gallery(int src, const char *html, char ***urls) {
  (void)src;
  *urls = NULL;
  char tags[3000];
  page_tags(html, tags, sizeof tags);
  if (lv_manga_never(tags)) return -2;
  // the first page's thumbnail: ".../1t.jpg"; its pages are the same address without the t
  const char *t1 = NULL;
  for (const char *p = strstr(html, "1t."); p; p = strstr(p + 3, "1t.")) if (p[-1] == '/') { t1 = p; break; }
  if (!t1) return -1;
  const char *st = t1;
  while (st > html && st[-1] != '"' && st[-1] != '\'') st--;
  char base[400];
  snprintf(base, sizeof base, "%.*s", (int)(t1 - st), st);
  if (!strncmp(base, "//", 2)) { memmove(base + 6, base, strlen(base) + 1); memcpy(base, "https:", 6); }
  char text_ext[8] = "jpg";
  sscanf(t1 + 3, "%7[a-z]", text_ext);
  // how many: said in a hidden field or as "Pages: N", else the highest numbered thumbnail
  int n = 0;
  const char *q;
  if ((q = strstr(html, "id=\"load_pages\" value=\""))) n = num_at(q + 23);
  if (!n && (q = strstr(html, "data-total-pages=\""))) n = num_at(q + 18);
  if (!n && (q = strstr(html, "Pages:"))) n = num_at(q + 6);
  if (!n && (q = strstr(html, "pages=\""))) n = num_at(q + 7);
  if (!n) {
    size_t bl = strlen(base);
    for (const char *p = strstr(html, base); p; p = strstr(p + bl, base)) { int k = atoi(p + bl); const char *r = p + bl; while (isdigit((unsigned char)*r)) r++; if (*r == 't' && k > n) n = k; }
  }
  if (n <= 0 || n > 3000) return -1;
  // each page's kind: the g_th list ("1":"w,1280,1810": j jpg, p png, w webp, g gif), else the
  // thumbnail's (the reader tries the others when it isn't)
  const char *gth = strstr(html, "g_th");
  char **u = urls_new(n);
  if (!u) return -1;
  for (int k = 1; k <= n; k++) {
    const char *ext = text_ext;
    if (gth) {
      char key[16];
      snprintf(key, sizeof key, "\"%d\":\"", k);
      const char *e = strstr(gth, key);
      if (e && e - gth < 400000) { char c = e[strlen(key)]; ext = c == 'p' ? "png" : c == 'w' ? "webp" : c == 'g' ? "gif" : "jpg"; }
    }
    char a[460];
    snprintf(a, sizeof a, "%s%d.%s", base, k, ext);
    u[k - 1] = strdup(a);
  }
  *urls = u;
  return n;
}

// "chapter-12-5" -> "Chapter 12.5"
static void chapter_label(const char *path, char *out, size_t cap) {
  const char *e = path + strlen(path);
  while (e > path && e[-1] == '/') e--;
  const char *s = e;
  while (s > path && s[-1] != '/') s--;
  char seg[120];
  snprintf(seg, sizeof seg, "%.*s", (int)(e - s), s);
  for (char *c = seg; *c; c++) if (*c == '-' || *c == '_') *c = ' ';
  // (a number's parts: "12 5" is 12.5)
  char *d = strpbrk(seg, "0123456789");
  if (d) { char *sp = strchr(d, ' '); if (sp && isdigit((unsigned char)sp[1])) *sp = '.'; }
  if (seg[0] >= 'a' && seg[0] <= 'z') seg[0] -= 32;
  snprintf(out, cap, "%s", seg);
}
static double chapter_num(const char *label) { const char *d = strpbrk(label, "0123456789"); return d ? atof(d) : 0; }
static int cmp_ch(const void *a, const void *b) {
  double x = chapter_num(((const LvChapter *)a)->label), y = chapter_num(((const LvChapter *)b)->label);
  return x < y ? -1 : x > y;
}
int lv_ms_parse_chapters(int src, const char *html, const char *book, LvChapter *out, int cap) {
  char tags[3000];
  page_tags(html, tags, sizeof tags);
  if (lv_manga_never(tags)) return -2;
  const LvMSrc *s = &LV_MSRC[src];
  int got = 0;
  for (const char *p = strstr(html, "<a "); p && got < cap; p = strstr(p + 3, "<a ")) {
    char href[400];
    if (attr(p, "href", href, sizeof href) != 0) continue;
    const char *sp = site_path(src, href);
    if (!sp || strncmp(sp, s->item, strlen(s->item))) continue;
    // (this book's own: other titles' chapters are linked round the page too)
    if (book && book[0]) { size_t bl = strlen(book); while (bl && book[bl - 1] == '/') bl--; if (strncmp(sp, book, bl) || sp[bl] != '/') continue; }
    // ("<item><book>/<chapter>": a chapter has a part after the book's)
    const char *rest = sp + strlen(s->item);
    const char *sl = strchr(rest, '/');
    if (!sl || !sl[1] || sl[1] == '?' || !strstr(sl, "chapter")) continue;
    if (strstr(sl, "/feed") || strstr(sl, "#")) continue;
    int dup = 0;
    for (int k = 0; k < got && !dup; k++) dup = !strcmp(out[k].id, sp);
    if (dup) continue;
    memset(&out[got], 0, sizeof out[got]);
    snprintf(out[got].id, sizeof out[got].id, "%s", sp);
    chapter_label(sp, out[got].label, sizeof out[got].label);
    got++;
  }
  qsort(out, got, sizeof *out, cmp_ch); // (first to last: sites list them newest first)
  return got;
}
// whether an address holds one of the paths in a "|" list ("/chapters/|/uploads/")
static int path_in(const char *a, const char *list) {
  for (const char *p = list; p && *p;) {
    size_t l = strcspn(p, "|");
    char one[80];
    snprintf(one, sizeof one, "%.*s", (int)(l < sizeof one ? l : sizeof one - 1), p);
    if (one[0] && strstr(a, one)) return 1;
    p += l; if (*p == '|') p++;
  }
  return 0;
}
int lv_ms_parse_pages(int src, const char *html, char ***urls) {
  *urls = NULL;
  const LvMSrc *s = &LV_MSRC[src];
  char tags[3000];
  page_tags(html, tags, sizeof tags);
  if (s->single && lv_manga_never(tags)) return -2; // (a single gallery: its own tags are checked here)
  int cap = 64, n = 0;
  char **u = urls_new(cap);
  if (!u) return -1;
  for (const char *p = strstr(html, "<img"); p; p = strstr(p + 4, "<img")) {
    char a[600];
    if (img_src(p, a, sizeof a) != 0 || !path_in(a, s->img)) continue;
    // (a thumbnail-small copy WordPress made, "-248x320.jpg": not a page; "-768x960" is one)
    const char *dot = strrchr(a, '.'), *x = dot;
    while (x && x > a && (isdigit((unsigned char)x[-1]) || x[-1] == 'x')) x--;
    if (x && dot && x > a && x[-1] == '-' && strchr(x, 'x') && strchr(x, 'x') < dot && atoi(x) < 400) continue;
    if (strstr(a, "logo") || strstr(a, "avatar") || strstr(a, "banner")) continue;
    char full[700];
    lv_ms_url(src, a, full, sizeof full);
    int dup = 0;
    size_t fl = strlen(full);
    for (int k = 0; k < n && !dup; k++) dup = !strncmp(u[k], full, fl) && (!u[k][fl] || u[k][fl] == '#');
    if (dup) continue;
    // (a webtoon's strip, many screens tall, with its size given: bands of about a page's shape,
    //  as even as they can be, each read as a page of its own)
    char ws[16] = "", hs[16] = "";
    attr(p, "width", ws, sizeof ws);
    attr(p, "height", hs, sizeof hs);
    int iw = atoi(ws), ih = atoi(hs), nb = 1, rows = 0;
    if (iw > 0 && ih > iw * 2.5f) { nb = (int)((ih + iw * 1.5f - 1) / (iw * 1.5f)); rows = (ih + nb - 1) / nb; }
    for (int k = 0; k < nb; k++) {
      if (n == cap) { char **nu = realloc(u, (cap *= 2) * sizeof *u); if (!nu) break; u = nu; }
      char one[760];
      if (rows) snprintf(one, sizeof one, "%s#lvband=%d,%d,%d", full, k, rows, ih);
      else snprintf(one, sizeof one, "%s", full);
      u[n++] = strdup(one);
    }
  }
  if (!n) { free(u); return -1; }
  *urls = u;
  return n;
}

int lv_hh_parse_list(const char *json, size_t n, LvBook *out, int cap) {
  Json j;
  if (json_parse(&j, json, n) != 0) return -1;
  int data = json_get(&j, 0, "data"), got = 0;
  for (int k = 0, c = data >= 0 ? data + 1 : -1; data >= 0 && j.t[data].type == J_ARR && k < j.t[data].size && got < cap; k++, c = j.t[c].next) {
    LvBook b;
    memset(&b, 0, sizeof b);
    char slug[160] = "";
    json_gs(&j, c, "slug", slug, sizeof slug);
    json_gs(&j, c, "title", b.title, sizeof b.title);
    json_gs(&j, c, "thumb_url", b.cover, sizeof b.cover);
    b.pages = (int)json_gn(&j, c, "pages");
    if (!b.pages) { char pg[16] = ""; json_gs(&j, c, "pages", pg, sizeof pg); b.pages = atoi(pg); }
    // its tags, for the never-shown words
    int tg = json_get(&j, c, "tags");
    size_t tl = 0;
    for (int m = 0, t = tg >= 0 ? tg + 1 : -1; tg >= 0 && j.t[tg].type == J_ARR && m < j.t[tg].size && tl + 64 < sizeof b.tags; m++, t = j.t[t].next) {
      char name[64] = "";
      json_gs(&j, t, "name", name, sizeof name);
      tl += snprintf(b.tags + tl, sizeof b.tags - tl, "%s, ", name);
    }
    if (!slug[0] || lv_manga_never(b.tags) || lv_manga_never(b.title)) continue;
    snprintf(b.id, sizeof b.id, "%s", slug);
    out[got++] = b;
  }
  json_free(&j);
  return got;
}
int lv_hh_parse_images(const char *json, size_t n, char ***urls) {
  *urls = NULL;
  Json j;
  if (json_parse(&j, json, n) != 0) return -1;
  int im = json_get(&j, 0, "images"), cnt = im >= 0 && j.t[im].type == J_ARR ? j.t[im].size : 0, got = 0;
  char **u = cnt ? urls_new(cnt) : NULL;
  for (int k = 0, c = im + 1; u && k < cnt; k++, c = j.t[c].next) {
    char a[600] = "";
    json_gs(&j, c, "source_url", a, sizeof a);
    if (a[0]) u[got++] = strdup(a);
  }
  json_free(&j);
  if (!got) { free(u); return -1; }
  *urls = u;
  return got;
}

int lv_band_of(const char *url, char *base, size_t cap, int *k, int *rows, int *total) {
  const char *f = strstr(url, "#lvband=");
  *total = 0;
  if (!f || sscanf(f + 8, "%d,%d,%d", k, rows, total) < 2 || *k < 0 || *rows < 16) return 0;
  snprintf(base, cap, "%.*s", (int)(f - url), url);
  return 1;
}
