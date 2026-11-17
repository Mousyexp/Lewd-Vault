// Finding more sites (discover.h).
#define _GNU_SOURCE // (strcasestr)
#include "discover.h"
#include "booru.h"
#include "json.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

const char *const LV_GRABBER_DIRS[3] = {"Danbooru%20(2.0)", "Gelbooru%20(0.2)", "Moebooru"};
const int LV_GRABBER_KINDS[3] = {BK_DANBOORU, BK_GELBOORU, BK_MOEBOORU};

// (minors; furry and pony boorus; real people; the mixed ones the app keeps hidden)
static const char *const NEVER[] = {"allthefallen", "atfbooru", "lolibooru", "loli", "shota", "cub",
                                    "e621", "e926", "e6ai", "furry", "fur", "derpibooru", "ponybooru", "twibooru", "pony",
                                    "realbooru", "3dbooru", "behoimi", "idol", "paheal", "rule34", "tbib", "borvar", NULL};
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
  }
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
