// Finding more sites (discover.h).
#define _GNU_SOURCE // (strcasestr)
#include "discover.h"
#include "booru.h"
#include "json.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

const char *const LV_GRABBER_DIRS[3] = {"Danbooru%20(2.0)", "Gelbooru%20(0.2)", "Moebooru"};
const int LV_GRABBER_KINDS[3] = {BK_DANBOORU, BK_GELBOORU, BK_MOEBOORU};

// (minors; furry and pony boorus; real people; AI-made pictures; the mixed ones the app keeps hidden)
static const char *const NEVER[] = {"allthefallen", "atfbooru", "lolibooru", "loli", "shota", "cub",
                                    "e621", "e926", "e6ai", "furry", "fur", "derpibooru", "ponybooru", "twibooru", "pony",
                                    "realbooru", "3dbooru", "aibooru", "ai", "aiart", "diffusion", "aigenerated", "behoimi", "idol", "paheal", "rule34", "tbib", "borvar", NULL};
int lv_disc_never(const char *name, const char *base) {
  const char *in[2] = {name, base};
  for (int s = 0; s < 2; s++) {
    if (!in[s]) continue;
    // word by word (letters and digits), so "furry" catches "furry.booru.org" but "fur" not "future"
    const char *p = in[s];
    while (*p) {
      while (*p && !isalnum((unsigned char)*p)) p++;
      const char *b = p;
      while (*p && isalnum((unsigned char)*p)) p++;
      size_t n = (size_t)(p - b);
      for (int k = 0; NEVER[k] && n; k++)
        if (strlen(NEVER[k]) == n && !strncasecmp(b, NEVER[k], n)) return 1;
    }
    // (and anywhere in the host, for names run together: allthefallen, lolibooru)
    for (int k = 0; NEVER[k]; k++) if (strlen(NEVER[k]) >= 5 && strcasestr(in[s], NEVER[k])) return 1;
  }
  return 0;
}
static const char *host_of(const char *u, size_t *len) {
  const char *h = strstr(u, "://");
  h = h ? h + 3 : u;
  *len = strcspn(h, "/:?");
  return h;
}
int lv_disc_same_site(const char *a, const char *b) {
  size_t la, lb;
  const char *ha = host_of(a, &la), *hb = host_of(b, &lb);
  // the last two parts of each
  int dots = 0;
  size_t ia = la, ib = lb;
  while (ia > 0) { if (ha[ia - 1] == '.' && ++dots == 2) break; ia--; }
  dots = 0;
  while (ib > 0) { if (hb[ib - 1] == '.' && ++dots == 2) break; ib--; }
  return la - ia == lb - ib && !strncasecmp(ha + ia, hb + ib, la - ia);
}
static int add(const char *name, const char *url, int kind, LvCand *out, int *n, int cap) {
  if (*n >= cap || !url || strncmp(url, "http", 4)) return 0;
  char base[160];
  size_t hl;
  const char *h = host_of(url, &hl);
  if (!hl || hl > 120) return 0;
  if (!strncmp(h, "api.", 4)) { h += 4; hl -= 4; } // (api.rule34.xxx: the site itself)
  snprintf(base, sizeof base, "https://%.*s", (int)hl, h);
  for (char *c = base; *c; c++) *c = (char)tolower((unsigned char)*c);
  if (lv_disc_never(name, base)) return 0;
  for (int k = 0; k < *n; k++) if (lv_disc_same_site(out[k].base, base)) return 0;
  LvCand *c = &out[(*n)++];
  memset(c, 0, sizeof *c);
  snprintf(c->base, sizeof c->base, "%s", base);
  if (name && name[0]) snprintf(c->name, sizeof c->name, "%s", name);
  else {
    const char *x = base + 8;
    if (!strncmp(x, "www.", 4)) x += 4;
    snprintf(c->name, sizeof c->name, "%s", x);
  }
  c->kind = kind;
  return 1;
}

static int gallerydl_md(const char *md, int kind, const char *from, const char *to, LvCand *out, int *n, int cap);
// gallery-dl's docs/supportedsites.md: a table; a row with colspan opens a group ("Danbooru
// Instances"), then a row per site: <td>Name</td> <td>https://address/</td> ...
int lv_disc_gallerydl(const char *md, size_t len, LvCand *out, int *n, int cap) {
  static const struct { const char *head; int kind; } groups[] = {
    {"Danbooru Instances", BK_DANBOORU}, {"Gelbooru Beta 0.2", BK_GELBOORU}, {"Moebooru and MyImouto", BK_MOEBOORU}};
  int added = 0;
  (void)len;
  for (int g = 0; g < 3; g++) {
    const char *p = strstr(md, groups[g].head);
    if (!p) continue;
    const char *end = strstr(p + strlen(groups[g].head), "colspan");
    char prev[64] = "";
    for (const char *td = strstr(p, "<td>"); td && (!end || td < end); td = strstr(td + 4, "<td>")) {
      const char *v = td + 4, *e = strstr(v, "</td>");
      if (!e || (end && e > end)) break;
      char cell[200];
      snprintf(cell, sizeof cell, "%.*s", (int)(e - v < 199 ? e - v : 199), v);
      if (!strncmp(cell, "https://", 8) || !strncmp(cell, "http://", 7)) added += add(prev, cell, groups[g].kind, out, n, cap);
      else snprintf(prev, sizeof prev, "%.47s", cell);
    }
    // (no HTML cells: the group as a markdown table, up to the next heading)
    if (!strstr(p, "<td>") || (end && strstr(p, "<td>") > end)) {
      const char *to = strstr(p + 1, "\n#");
      added += gallerydl_md(md, groups[g].kind, p, to, out, n, cap);
    }
  }
  return added;
}

// (a markdown table instead, should the page ever be written that way: | Name | https://... | ...)
static int gallerydl_md(const char *md, int kind, const char *from, const char *to, LvCand *out, int *n, int cap) {
  int added = 0;
  for (const char *l = from; l && (!to || l < to); ) {
    const char *e = strchr(l, '\n');
    if (!e) e = l + strlen(l);
    if (*l == '|') {
      char name[64] = "", url[200] = "";
      const char *c1 = l + 1, *b1 = strchr(c1, '|');
      if (b1 && b1 < e) {
        const char *c2 = b1 + 1, *b2 = strchr(c2, '|');
        if (b2 && b2 < e) {
          while (*c1 == ' ') c1++;
          while (*c2 == ' ') c2++;
          snprintf(name, sizeof name, "%.*s", (int)(b1 - c1), c1);
          snprintf(url, sizeof url, "%.*s", (int)(b2 - c2), c2);
          for (int k = (int)strlen(name) - 1; k >= 0 && name[k] == ' '; k--) name[k] = 0;
          for (int k = (int)strlen(url) - 1; k >= 0 && url[k] == ' '; k--) url[k] = 0;
          if (!strncmp(url, "http", 4)) added += add(name, url, kind, out, n, cap);
        }
      }
    }
    l = *e ? e + 1 : NULL;
  }
  (void)md;
  return added;
}

// the "booru" library's src/sites.json: { "host": { "domain", "api": { "search": ... } } }; the kind
// from the search's address
int lv_disc_boorujs(const char *text, size_t len, LvCand *out, int *n, int cap) {
  Json j;
  if (json_parse(&j, text, len) != 0) return 0;
  int added = 0;
  if (j.n > 0 && j.t[0].type == J_OBJ) {
    for (int k = 0; k < j.t[0].size; k++) {
      int site = json_at(&j, 0, k);
      char dom[128] = "", search[128] = "";
      json_str(&j, json_get(&j, site, "domain"), dom, sizeof dom);
      json_str(&j, json_get(&j, json_get(&j, site, "api"), "search"), search, sizeof search);
      int kind = strstr(search, "page=dapi") ? BK_GELBOORU : !strncmp(search, "/posts.json", 11) ? BK_DANBOORU
               : !strncmp(search, "/post.json", 10) ? BK_MOEBOORU : -1;
      if (kind < 0 || !dom[0]) continue;
      char url[160];
      snprintf(url, sizeof url, "https://%s", dom);
      added += add(NULL, url, kind, out, n, cap);
    }
  }
  json_free(&j);
  return added;
}

// Grabber's sites.txt: a host a line
int lv_disc_lines(const char *txt, size_t len, int kind, LvCand *out, int *n, int cap) {
  int added = 0;
  const char *p = txt, *e = txt + len;
  while (p < e) {
    const char *nl = memchr(p, '\n', (size_t)(e - p));
    if (!nl) nl = e;
    int l = (int)(nl - p);
    while (l > 0 && (p[l - 1] == '\r' || p[l - 1] == ' ')) l--;
    if (l > 0 && l < 150 && *p != '#') {
      char url[160];
      snprintf(url, sizeof url, "https://%.*s", l, p);
      added += add(NULL, url, kind, out, n, cap);
    }
    p = nl + 1;
  }
  return added;
}

int lv_disc_pybooru(const char *py, size_t len, LvCand *out, int *n, int cap) {
  (void)len;
  int added = 0;
  const char *list = strstr(py, "SITE_LIST = {");
  if (!list) return 0;
  const char *end = strstr(list, "\n    }\n");
  for (const char *u = strstr(list, "'url':"); u && (!end || u < end); u = strstr(u + 6, "'url':")) {
    const char *q = strchr(u + 6, '"');
    if (!q) break;
    const char *e = strchr(q + 1, '"');
    if (!e) break;
    char url[200];
    snprintf(url, sizeof url, "%.*s", (int)(e - q - 1), q + 1);
    // (its entry: up to the closing brace; an api_version is Moebooru's)
    const char *close = strchr(e, '}');
    const char *api = strstr(e, "'api_version'");
    int kind = api && close && api < close ? BK_MOEBOORU : BK_DANBOORU;
    added += add(NULL, url, kind, out, n, cap);
  }
  return added;
}
void lv_disc_mirror(const char *raw, char *out, size_t cap) {
  // https://raw.githubusercontent.com/<user>/<repo>/<branch>/<path> -> https://cdn.jsdelivr.net/gh/<user>/<repo>@<branch>/<path>
  const char *p = strstr(raw, "raw.githubusercontent.com/");
  out[0] = 0;
  if (!p) return;
  p += 26;
  const char *s1 = strchr(p, '/'), *s2 = s1 ? strchr(s1 + 1, '/') : NULL, *s3 = s2 ? strchr(s2 + 1, '/') : NULL;
  if (!s3) return;
  snprintf(out, cap, "https://cdn.jsdelivr.net/gh/%.*s@%.*s%s", (int)(s2 - p), p, (int)(s3 - s2 - 1), s2 + 1, s3);
}
size_t lv_disc_save(const LvCand *c, int n, char *out, size_t cap) {
  size_t o = 0;
  if (cap) out[0] = 0;
  for (int k = 0; k < n && o < cap; k++) {
    int w = snprintf(out + o, cap - o, "%d\t%s\t%s\n", c[k].kind, c[k].name, c[k].base);
    if (w < 0 || (size_t)w >= cap - o) break;
    o += (size_t)w;
  }
  return o;
}
int lv_disc_load(const char *txt, LvCand *out, int *n, int cap) {
  int added = 0;
  for (const char *l = txt; l && *l; ) {
    const char *e = strchr(l, '\n');
    char line[260];
    snprintf(line, sizeof line, "%.*s", (int)(e ? e - l : (long)strlen(l)), l);
    char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
    if (t1 && t2) {
      *t1 = 0; *t2 = 0;
      int kind = atoi(line);
      if (kind >= BK_DANBOORU && kind <= BK_MOEBOORU) added += add(t1 + 1, t2 + 1, kind, out, n, cap);
    }
    l = e ? e + 1 : NULL;
  }
  return added;
}
