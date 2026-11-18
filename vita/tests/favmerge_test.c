// Two-way Favourites' merge (src/favmerge.c): additions and removals from either side.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/favmerge.h"

static int fails;
static void put(const char *path, const char *text) { FILE *f = fopen(path, "wb"); fputs(text, f); fclose(f); }
static void expect(const char *path, const char *want, const char *what) {
  static char got[4096];
  FILE *f = fopen(path, "rb");
  size_t n = f ? fread(got, 1, sizeof got - 1, f) : 0;
  if (f) fclose(f);
  got[n] = 0;
  if (strcmp(got, want)) { printf("FAIL %s\n--- got\n%s--- want\n%s", what, got, want); fails++; }
}
#define A "0\tsite\tA\thttps://x/a\tp\te\t60\n"
#define B "0\tsite\tB\thttps://x/b\tp\te\t60\n"
#define C "1\tsite\tC\thttps://x/c\tp\te\t12\n"
#define D "0\tsite\tD\thttps://x/d\tp\te\t60\n"
#define C_AS_VIDEO "0\tsite\tC\thttps://x/c\tp\te\t12\n"

int main(void) {
  const char *dir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  char m[512], t[512], b[512], o[512];
  snprintf(m, sizeof m, "%s/fm-mine.tsv", dir); snprintf(t, sizeof t, "%s/fm-theirs.tsv", dir);
  snprintf(b, sizeof b, "%s/fm-base.tsv", dir); snprintf(o, sizeof o, "%s/fm-out.tsv", dir);
  int ad, rm;

  // first time (no base): both sides' together, theirs first
  remove(b);
  put(m, A B); put(t, B C);
  lv_favs_merge(m, t, b, o, &ad, &rm);
  expect(o, C A B, "union");
  if (ad != 1 || rm != 0) { printf("FAIL union counts %d %d\n", ad, rm); fails++; }

  // removed there: gone here; added here: kept; removed here: not back
  put(b, A B C); put(m, D A C); put(t, A B);
  lv_favs_merge(m, t, b, o, &ad, &rm);
  expect(o, D A, "removals both ways");
  if (ad != 0 || rm != 1) { printf("FAIL removal counts %d %d\n", ad, rm); fails++; }

  // the same page as another kind is another favourite
  remove(b);
  put(m, C); put(t, C_AS_VIDEO);
  lv_favs_merge(m, t, b, o, NULL, NULL);
  expect(o, C_AS_VIDEO C, "kinds apart");

  // nothing there yet (no file): this side as it is
  remove(t); remove(b);
  put(m, A B);
  lv_favs_merge(m, t, b, o, NULL, NULL);
  expect(o, A B, "nothing there");

  remove(m); remove(t); remove(b); remove(o);
  if (fails) return 1;
  printf("favmerge: all passed\n");
  return 0;
}
