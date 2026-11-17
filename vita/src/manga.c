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
void lv_md_feed_url(const char *manga_id, int adult, int offset, char *out, size_t cap) {
  snprintf(out, cap, "https://api.mangadex.org/manga/%s/feed?translatedLanguage[]=en&order[volume]=asc&order[chapter]=asc&limit=100&offset=%d%s",
           manga_id, offset, adult ? RATINGS_ALL : RATINGS_SAFE);
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
