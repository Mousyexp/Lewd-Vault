// The Vita's vault core against the golden vaults the PC and phone tests use
// (src-tauri/tests/fixtures): run by tests/run.sh with the PC's C compiler.
#include "../src/lvcrypt.h"
#include "../src/vault.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *slurp(const char *p, size_t *n) {
  FILE *f = fopen(p, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long l = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc(l + 1);
  if (fread(b, 1, l, f) != (size_t)l) l = 0;
  b[l] = 0;
  fclose(f);
  if (n) *n = l;
  return b;
}

static void hex(const uint8_t *b, size_t n, char *out) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }

int main(int argc, char **argv) {
  const char *fx = argc > 1 ? argv[1] : "../src-tauri/tests/fixtures";
  char p[1024], h[200];
  uint8_t d[32];

  // SHA-256 and ChaCha20 against their published vectors
  lv_sha256((const uint8_t *)"abc", 3, d);
  hex(d, 32, h);
  CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256(abc) = %s", h);
  {
    uint8_t key[32], nonce[12] = {0, 0, 0, 0x09, 0, 0, 0, 0x4a, 0, 0, 0, 0}, z[64] = {0};
    for (int i = 0; i < 32; i++) key[i] = i;
    lv_chacha20_xor(key, nonce, 64, z, 64); // RFC 8439 2.3.2: block counter 1
    hex(z, 16, h);
    CHECK(!strcmp(h, "10f1e7e4d13b5915500fdd1fa32071c4"), "chacha20 block 1 = %s", h);
    // reading from the middle of a block gives the same bytes as from its start
    uint8_t a[100] = {0}, b[30] = {0};
    lv_chacha20_xor(key, nonce, 0, a, 100);
    lv_chacha20_xor(key, nonce, 50, b, 30);
    CHECK(!memcmp(a + 50, b, 30), "chacha20 seek");
  }

  // ---- the plain golden vault ----
  snprintf(p, sizeof p, "%s/golden-vault", fx);
  LvVault v;
  lv_vault_init(&v, p);
  CHECK(lv_is_vault(p) && !v.untraceable, "the plain vault");
  const char *why = NULL;
  CHECK(lv_vault_load(&v, &why) == 0, "load: %s", why ? why : "");
  CHECK(v.n == 3, "3 items, got %d", v.n);
  if (v.n == 3) {
    CHECK(!strcmp(v.items[0].name, "Cover, front \"final\".webp"), "quoted name: %s", v.items[0].name);
    CHECK(!strcmp(v.items[2].name, "line1\nline2.gif"), "a name over two lines");
    CHECK(!strcmp(v.items[2].category, "日本語"), "unicode category");
    CHECK(v.items[1].duration == 95.5 && v.items[1].nsfw == 1 && v.items[1].quality == 3, "numbers");
    CHECK(!strcmp(v.items[0].tags, "artist:someone blue_hair smile"), "tags");
    // found by its number in Photos/, though its category is "Test- Cat"
    CHECK(v.items[0].path && strstr(v.items[0].path, "Photos/000123_"), "item 123's file: %s", v.items[0].path ? v.items[0].path : "(none)");
    CHECK(v.items[1].path && strstr(v.items[1].path, "Clips/000124_"), "item 124's file");
    CHECK(!v.items[2].path, "item 125 has no file");
    // the thumbnail is a slice of a pack (the fixture's own id names a path, which lib.rs refuses
    // too; a pack the app makes is thumbs-<base64url of its name>)
    size_t tn = 0;
    uint8_t *t = lv_thumb_bytes(&v, &v.items[1], &tn);
    CHECK(!t, "a pack id with a path in it is refused");
    v.items[1].thumb = strdup("thumbs-cGFjay0yNjEwMTYtMS5iaW4:8:100");
    t = lv_thumb_bytes(&v, &v.items[1], &tn);
    {
      char pp[1024]; size_t pn = 0;
      snprintf(pp, sizeof pp, "%s/golden-vault/_app/thumbs/pack-261016-1.bin", fx);
      char *pack = slurp(pp, &pn);
      CHECK(t && tn == 100 && pack && !memcmp(t, pack + 8, 100), "pack slice: %zu", tn);
      free(pack);
    }
    free(t);
    CHECK(v.items[0].view == 0 && lv_views_of(&v, &v.items[0]) == 12 && lv_likes_of(&v, &v.items[0]) == 3, "views of 123");
    CHECK(v.views[0].seconds == 42.6, "seconds");
    CHECK(v.ncats == 3, "categories: %d", v.ncats);
    CHECK(v.hot[0].added == 20261016090000LL && v.hot[1].kind == LV_VIDEO && v.hot[2].kind == LV_GIF && v.hot[0].quality == 7, "hot fields");
    CHECK(!strcmp(v.cats[v.hot[2].cat], "日本語"), "hot category");
    CHECK(!strcmp(v.theme, "rose"), "theme: %s", v.theme);
  }
  // views.csv comes back byte for byte
  {
    size_t on = 0;
    snprintf(p, sizeof p, "%s/golden-vault/views.csv", fx);
    char *orig = slurp(p, &on);
    char *again = lv_views_csv(&v);
    CHECK(orig && strlen(again) == on && !memcmp(orig, again, on), "views.csv round trip\n--- want\n%s\n--- got\n%s", orig, again);
    free(orig); free(again);
  }
  // index.csv parses and writes back the same (the parser and writer agree with lib.rs)
  {
    size_t on = 0;
    snprintf(p, sizeof p, "%s/golden-vault/index.csv", fx);
    char *orig = slurp(p, &on);
    LvCsv c;
    lv_csv_parse(orig, &c);
    char *again = lv_csv_text(c.rows, c.ncells, c.nrows);
    CHECK(strlen(again) == on && !memcmp(orig, again, on), "index.csv round trip");
    lv_csv_free(&c);
    free(orig); free(again);
  }
  // a view, a heart and time, as the PC writes them
  {
    LvItem *it = &v.items[2];
    lv_viewed(&v, it, "2026-11-16T10:00:00.000Z");
    lv_liked(&v, it, "2026-11-16T10:00:01.000Z");
    lv_add_seconds(&v, it, 3.04);
    lv_viewed(&v, &v.items[0], "2026-01-01T00:00:00.000Z"); // (older than its last view: kept)
    char *t = lv_views_csv(&v);
    CHECK(strstr(t, "\r\nitem-000125,000125,2,2026-11-16T10:00:00.000Z,3,2026-11-16T10:00:01.000Z,3\r\n"), "item 125 row:\n%s", t);
    CHECK(strstr(t, "item-000123,000123,13,2026-10-20T12:00:00Z,3,"), "item 123 row");
    free(t);
    // an item with no row yet gets one at the end
    v.items[1].id = strdup("item-000999"); v.items[1].uid = strdup("000999"); // (the old ones are in the arena)
    v.items[1].view = -1;
    lv_add_seconds(&v, &v.items[1], 12.25);
    t = lv_views_csv(&v);
    CHECK(strstr(t, "\r\nitem-000999,000999,0,,0,,12.3\r\n"), "new row:\n%s", t);
    free(t);
  }
  char n1[32];
  lv_num_or_empty(7, n1, sizeof n1); CHECK(!strcmp(n1, "7"), "7");
  lv_num_or_empty(0, n1, sizeof n1); CHECK(!strcmp(n1, ""), "0");
  lv_num_or_empty(0.1, n1, sizeof n1); CHECK(!strcmp(n1, "0.1"), "0.1");
  lv_vault_free(&v);

  // ---- the untraceable golden vault (password hunter2, made by an independent implementation) ----
  snprintf(p, sizeof p, "%s/golden-untraceable", fx);
  lv_vault_init(&v, p);
  CHECK(v.untraceable, "untraceable");
  CHECK(lv_vault_load(&v, &why) != 0, "locked before the password");
  CHECK(lv_vault_unlock(&v, "hunter3") == 0, "a wrong password");
  CHECK(lv_vault_unlock(&v, "hunter2") == 1, "the right password");
  CHECK(lv_vault_load(&v, &why) == 0, "load: %s", why ? why : "");
  CHECK(v.n == 3 && !strcmp(v.items[2].category, "日本語"), "decrypted index");
  CHECK(v.nviews == 3 && v.views[0].seconds == 42.6, "decrypted views");
  CHECK(!strcmp(v.theme, "berry"), "decrypted settings: %s", v.theme);
  // written back encrypted, and read back the same
  {
    char tmp[200];
    snprintf(tmp, sizeof tmp, "/tmp/lv-vita-test-%d", (int)getpid());
    char cmd[2400];
    snprintf(cmd, sizeof cmd, "rm -rf %s && cp -r '%s' %s", tmp, p, tmp);
    CHECK(system(cmd) == 0, "copy");
    char *before = lv_views_csv(&v);
    snprintf(v.root, sizeof v.root, "%s", tmp);
    v.views_dirty = 1;
    CHECK(lv_vault_save_views(&v) == 0, "save views");
    snprintf(cmd, sizeof cmd, "%s/views.csv", tmp);
    size_t rn = 0;
    char *raw = slurp(cmd, &rn);
    CHECK(raw && !memcmp(raw, "LVX1", 4), "views.csv stays encrypted");
    free(raw);
    uint8_t *plain = lv_read_all(cmd, v.key, &rn);
    CHECK(plain && rn == strlen(before) && !memcmp(plain, before, rn), "encrypted views round trip");
    free(plain); free(before);
    snprintf(cmd, sizeof cmd, "rm -rf %s", tmp);
    CHECK(system(cmd) == 0, "clean up");
  }
  lv_vault_free(&v);

  printf(fails ? "%d FAILED\n" : "core: all passed\n", fails);
  return fails != 0;
}
