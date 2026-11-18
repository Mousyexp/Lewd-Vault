// Two-way Favourites: two lists of favourites (favorites.tsv's lines: kind, site, title, page, ...)
// made one, knowing the list both last agreed on (the base). A favourite is the same one when its
// kind and page are. One added on either side is kept; one removed on either side (in the base, gone
// from that side) is removed; the rest stay as this side has them. The other side's new ones come
// first (the list is newest first), then this side's, in its order. Plain C, tested on the PC too
// (tests/favmerge_test.c).
#include "favmerge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FM_MAX 300

typedef struct { char *line; char key[640]; } FmLine;
typedef struct { FmLine *v; int n; } FmList;

// (a line's key: its first field and its fourth, the page)
static int fm_key(const char *line, char *key, size_t cap) {
  const char *t[4] = {line, NULL, NULL, NULL};
  for (int k = 1; k < 4; k++) { const char *p = strchr(t[k - 1], '\t'); if (!p) return -1; t[k] = p + 1; }
  const char *end = strchr(t[3], '\t');
  size_t kn = (size_t)(strchr(line, '\t') - line), pn = end ? (size_t)(end - t[3]) : strlen(t[3]);
  if (!pn || kn + pn + 2 > cap) return -1;
  memcpy(key, line, kn);
  key[kn] = '|';
  memcpy(key + kn + 1, t[3], pn);
  key[kn + 1 + pn] = 0;
  return 0;
}
static void fm_free(FmList *l) { for (int k = 0; k < l->n; k++) free(l->v[k].line); free(l->v); l->v = NULL; l->n = 0; }
// (a list from a file: none, or not there, is an empty list)
static int fm_read(const char *path, FmList *l) {
  l->v = calloc(FM_MAX, sizeof *l->v);
  l->n = 0;
  if (!l->v) return -1;
  FILE *f = path ? fopen(path, "rb") : NULL;
  if (!f) return 0;
  static char ln[2400];
  while (l->n < FM_MAX && fgets(ln, sizeof ln, f)) {
    ln[strcspn(ln, "\r\n")] = 0;
    FmLine *x = &l->v[l->n];
    if (fm_key(ln, x->key, sizeof x->key) != 0) continue;
    x->line = strdup(ln);
    if (x->line) l->n++;
  }
  fclose(f);
  return 0;
}
static int fm_has(const FmList *l, const char *key) {
  for (int k = 0; k < l->n; k++) if (!strcmp(l->v[k].key, key)) return 1;
  return 0;
}

int lv_favs_merge(const char *mine, const char *theirs, const char *base, const char *out, int *added, int *removed) {
  FmList m = {0}, t = {0}, b = {0};
  int r = -1, na = 0, nr = 0, nout = 0;
  if (fm_read(mine, &m) || fm_read(theirs, &t) || fm_read(base, &b)) goto done;
  char part[600];
  snprintf(part, sizeof part, "%s.part", out);
  FILE *f = fopen(part, "wb");
  if (!f) goto done;
  // their new ones (not in the base, not here), first
  for (int k = 0; k < t.n && nout < FM_MAX; k++) {
    if (fm_has(&m, t.v[k].key) || fm_has(&b, t.v[k].key)) continue;
    fprintf(f, "%s\n", t.v[k].line); nout++; na++;
  }
  // then this side's: kept unless the other side removed it
  for (int k = 0; k < m.n; k++) {
    if (fm_has(&b, m.v[k].key) && !fm_has(&t, m.v[k].key)) { nr++; continue; }
    if (nout < FM_MAX) { fprintf(f, "%s\n", m.v[k].line); nout++; }
  }
  r = fclose(f) == 0 ? 0 : -1;
  if (r == 0) { remove(out); r = rename(part, out) == 0 ? 0 : -1; }
  else remove(part);
done:
  fm_free(&m); fm_free(&t); fm_free(&b);
  if (added) *added = na;
  if (removed) *removed = nr;
  return r;
}
