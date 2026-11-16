#include "vault.h"
#include "lvcrypt.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *dupn(const char *s, size_t n) {
  char *d = malloc(n + 1);
  memcpy(d, s, n);
  d[n] = 0;
  return d;
}
static char *dup(const char *s) { return dupn(s ? s : "", strlen(s ? s : "")); }

// The items' strings: thousands of small ones, made once and dropped together, so they go into a few
// big blocks instead of one malloc each (less time, no fragmentation of the Vita's fixed heap).
typedef struct LvArena { struct LvArena *next; size_t used, cap; char data[]; } LvArena;
static char *arena_dup(LvArena **a, const char *s) {
  size_t n = strlen(s) + 1;
  if (!*a || (*a)->used + n > (*a)->cap) {
    size_t cap = n > 256 * 1024 ? n : 256 * 1024;
    LvArena *b = malloc(sizeof *b + cap);
    b->next = *a;
    b->used = 0;
    b->cap = cap;
    *a = b;
  }
  char *d = (*a)->data + (*a)->used;
  memcpy(d, s, n);
  (*a)->used += n;
  return d;
}
static void arena_free(LvArena *a) {
  while (a) { LvArena *n = a->next; free(a); a = n; }
}
static int64_t time_key(const char *s) {
  int64_t k = 0;
  int digits = 0;
  for (; *s && digits < 14; s++)
    if (*s >= '0' && *s <= '9') { k = k * 10 + (*s - '0'); digits++; }
  while (digits++ < 14) k *= 10;
  return k;
}

// ---------- CSV (the same rules as parse_csv / csv_text in lib.rs) ----------

typedef struct { char *s; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    b->cap = (b->n + n + 1) * 2;
    b->s = realloc(b->s, b->cap);
  }
  memcpy(b->s + b->n, s, n);
  b->n += n;
  b->s[b->n] = 0;
}
static void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }

void lv_csv_parse(const char *text, LvCsv *out) {
  memset(out, 0, sizeof *out);
  if (!strncmp(text, "\xEF\xBB\xBF", 3)) text += 3;
  int caprows = 0, capcells = 0, ncells = 0;
  char **row = NULL;
  Buf cell = {0};
  bput(&cell, "", 0);
  int quoted = 0;
#define PUSH_CELL() do { if (ncells == capcells) { capcells = capcells ? capcells * 2 : 16; row = realloc(row, capcells * sizeof *row); } \
    row[ncells++] = dup(cell.s); cell.n = 0; cell.s[0] = 0; } while (0)
#define PUSH_ROW() do { if (out->nrows == caprows) { caprows = caprows ? caprows * 2 : 64; out->rows = realloc(out->rows, caprows * sizeof *out->rows); \
    out->ncells = realloc(out->ncells, caprows * sizeof *out->ncells); } out->rows[out->nrows] = row; out->ncells[out->nrows++] = ncells; \
    row = NULL; ncells = 0; capcells = 0; } while (0)
  for (const char *p = text; *p; p++) {
    char ch = *p;
    if (quoted) {
      if (ch == '"') {
        if (p[1] == '"') { bput(&cell, "\"", 1); p++; }
        else quoted = 0;
      } else {
        size_t run = strcspn(p, "\"");
        bput(&cell, p, run);
        p += run - 1;
      }
    } else if (ch == '"') quoted = 1;
    else if (ch == ',') PUSH_CELL();
    else if (ch == '\r' || ch == '\n') {
      if (ch == '\r' && p[1] == '\n') p++;
      PUSH_CELL();
      PUSH_ROW();
    } else {
      // (a run of plain characters at once)
      size_t run = strcspn(p, "\",\r\n");
      bput(&cell, p, run);
      p += run - 1;
    }
  }
  if (cell.n || ncells) { PUSH_CELL(); PUSH_ROW(); }
  free(cell.s);
  free(row);
}
void lv_csv_free(LvCsv *c) {
  for (int r = 0; r < c->nrows; r++) {
    for (int k = 0; k < c->ncells[r]; k++) free(c->rows[r][k]);
    free(c->rows[r]);
  }
  free(c->rows);
  free(c->ncells);
  memset(c, 0, sizeof *c);
}

char *lv_csv_text(char ***rows, const int *ncells, int nrows) {
  Buf b = {0};
  bstr(&b, "\xEF\xBB\xBF");
  for (int r = 0; r < nrows; r++) {
    if (r) bstr(&b, "\r\n");
    for (int k = 0; k < ncells[r]; k++) {
      if (k) bstr(&b, ",");
      const char *s = rows[r][k];
      if (strpbrk(s, "\",\r\n")) {
        bstr(&b, "\"");
        for (const char *p = s; *p; p++) {
          bput(&b, p, 1);
          if (*p == '"') bput(&b, "\"", 1);
        }
        bstr(&b, "\"");
      } else bstr(&b, s);
    }
  }
  bstr(&b, "\r\n");
  return b.s;
}

// Numbers the way JavaScript prints them (12 not 12.0), empty for 0 (num_or_empty in lib.rs).
void lv_num_or_empty(double f, char *out, size_t cap) {
  if (f == 0.0 || !isfinite(f)) { out[0] = 0; return; }
  if (f == floor(f) && fabs(f) < 9e15) { snprintf(out, cap, "%lld", (long long)f); return; }
  // shortest digits that read back the same, like Rust's Display
  for (int prec = 1; prec <= 17; prec++) {
    snprintf(out, cap, "%.*f", prec, f);
    if (strtod(out, NULL) == f) return;
  }
}

static long long int_of(const char *s) {
  char *end;
  double f = strtod(s, &end);
  if (end == s || !isfinite(f)) return 0;
  return (long long)f;
}

// ---------- base64url (the ids of files in _app: thumbs-<name>, clips-<name>) ----------
int lv_b64url_decode(const char *in, char *out, size_t cap) {
  size_t o = 0;
  unsigned acc = 0;
  int bits = 0;
  for (const char *p = in; *p; p++) {
    int v;
    char c = *p;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '-' || c == '+') v = 62;
    else if (c == '_' || c == '/') v = 63;
    else if (c == '=') break;
    else return -1;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o + 1 >= cap) return -1;
      out[o++] = (char)((acc >> bits) & 0xff);
    }
  }
  out[o] = 0;
  return (int)o;
}

// A file name from an id must stay a plain name (plain_name in lib.rs).
static int plain_name(const char *s) {
  if (!*s || !strcmp(s, ".") || !strcmp(s, "..")) return 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    if (*p == '/' || *p == '\\' || *p == ':' || *p < 0x20) return 0;
  return 1;
}

// A category as a folder name: what Windows doesn't allow becomes "-" (safe_name in lib.rs).
static void safe_name(const char *s, char *out, size_t cap) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p && o + 1 < cap; p++)
    out[o++] = (strchr("<>:\"/\\|?*", *p) && *p) || *p < 0x20 ? '-' : (char)*p;
  while (o && (out[o - 1] == '.' || out[o - 1] == ' ')) o--;
  out[o] = 0;
}

static int exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

int lv_is_vault(const char *dir) {
  char p[600];
  snprintf(p, sizeof p, "%s/index.csv", dir);
  return exists(p);
}
int lv_is_untraceable(const char *dir) {
  char p[600];
  snprintf(p, sizeof p, "%s/_app/key.check", dir);
  return exists(p);
}

void lv_vault_init(LvVault *v, const char *root) {
  memset(v, 0, sizeof *v);
  snprintf(v->root, sizeof v->root, "%s", root);
  size_t n = strlen(v->root);
  while (n > 1 && v->root[n - 1] == '/') v->root[--n] = 0;
  v->untraceable = lv_is_untraceable(v->root);
}

const uint8_t *lv_key(const LvVault *v) { return v->has_key ? v->key : NULL; }

int lv_vault_unlock(LvVault *v, const char *password) {
  char p[600];
  snprintf(p, sizeof p, "%s/_app/key.check", v->root);
  FILE *f = fopen(p, "rb");
  if (!f) return -1;
  uint8_t b[128];
  size_t n = fread(b, 1, sizeof b, f);
  fclose(f);
  uint8_t key[32];
  lv_derive(password, key);
  if (!lv_check_ok(key, b, n)) return 0;
  memcpy(v->key, key, 32);
  v->has_key = 1;
  return 1;
}

// ---------- finding files ----------

// Every file in the vault's own folders (not _app), by "<folder>/<name>" and by number ("000123"),
// listed once: an item is found where the index says, or by its number in any folder, as item_path
// does on the PC (a move cut short, a Discreet switch half done, a hand-made copy).
typedef struct { char **keys; char **vals; size_t cap; } Map;
static unsigned long hash(const char *s) {
  unsigned long h = 5381;
  while (*s) h = h * 33 ^ (unsigned char)*s++;
  return h;
}
static void map_put(Map *m, const char *k, const char *val) {
  size_t i = hash(k) & (m->cap - 1);
  while (m->keys[i]) {
    if (!strcmp(m->keys[i], k)) return; // (the first one found wins)
    i = (i + 1) & (m->cap - 1);
  }
  m->keys[i] = dup(k);
  m->vals[i] = dup(val);
}
static const char *map_get(const Map *m, const char *k) {
  size_t i = hash(k) & (m->cap - 1);
  while (m->keys[i]) {
    if (!strcmp(m->keys[i], k)) return m->vals[i];
    i = (i + 1) & (m->cap - 1);
  }
  return NULL;
}
static void map_free(Map *m) {
  for (size_t i = 0; i < m->cap; i++) { free(m->keys[i]); free(m->vals[i]); }
  free(m->keys);
  free(m->vals);
}

static void list_files(const char *root, Map *by_path, Map *by_uid) {
  DIR *d = opendir(root);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '_' || e->d_name[0] == '.') continue;
    char sub[1024];
    snprintf(sub, sizeof sub, "%s/%s", root, e->d_name);
    DIR *s = opendir(sub);
    if (!s) continue;
    struct dirent *f;
    while ((f = readdir(s))) {
      if (f->d_name[0] == '.') continue;
      char rel[1024], full[1600];
      snprintf(rel, sizeof rel, "%s/%s", e->d_name, f->d_name);
      snprintf(full, sizeof full, "%s/%s", root, rel);
      map_put(by_path, rel, full);
      const char *us = strchr(f->d_name, '_');
      if (us && us != f->d_name) {
        char uid[64];
        size_t n = (size_t)(us - f->d_name);
        if (n < sizeof uid) { memcpy(uid, f->d_name, n); uid[n] = 0; map_put(by_uid, uid, full); }
      }
    }
    closedir(s);
  }
  closedir(d);
}

static char *read_text(LvVault *v, const char *name) {
  char p[700];
  snprintf(p, sizeof p, "%s/%s", v->root, name);
  return (char *)lv_read_all(p, lv_key(v), NULL);
}

static int col(char **head, int n, const char *name) {
  for (int k = 0; k < n; k++) if (!strcmp(head[k], name)) return k;
  return -1;
}
static const char *cell(char **row, int n, int k) { return k >= 0 && k < n ? row[k] : ""; }

static LvVault *sort_ctx;
static int cmp_cats(const void *a, const void *b) {
  int ia = *(const int *)a, ib = *(const int *)b;
  int d = sort_ctx->cat_counts[ib] - sort_ctx->cat_counts[ia];
  return d ? d : strcmp(sort_ctx->cats[ia], sort_ctx->cats[ib]);
}

int lv_vault_load(LvVault *v, const char **why) {
  static const char *msg;
  if (v->untraceable && !v->has_key) { msg = "This vault is locked"; if (why) *why = msg; return -1; }
  char *text = read_text(v, "index.csv");
  if (!text) { msg = "index.csv could not be read"; if (why) *why = msg; return -1; }
  // Discreet names (_app/kv/storage.json {"discreet":true}) and the theme (_app/settings.json)
  char *kv = read_text(v, "_app/kv/storage.json");
  v->discreet = kv && strstr(kv, "\"discreet\":true");
  free(kv);
  char *settings = read_text(v, "_app/settings.json");
  v->theme[0] = 0;
  if (settings) {
    char *t = strstr(settings, "\"theme\"");
    if (t && (t = strchr(t + 7, '"'))) {
      char *e = strchr(t + 1, '"');
      if (e && e - t - 1 < (long)sizeof v->theme) { memcpy(v->theme, t + 1, e - t - 1); v->theme[e - t - 1] = 0; }
    }
    free(settings);
  }

  LvCsv c;
  lv_csv_parse(text, &c);
  free(text);
  if (c.nrows == 0) { lv_csv_free(&c); msg = "index.csv is empty"; if (why) *why = msg; return -1; }
  char **h = c.rows[0];
  int hn = c.ncells[0];
  int ci[19];
  const char *names[19] = {"id", "uid", "file", "original_name", "category", "kind", "nsfw", "quality", "added",
                           "width", "height", "duration", "preview_id", "thumb", "collection", "tags", "source", "", ""};
  for (int k = 0; k < 17; k++) ci[k] = col(h, hn, names[k]);

  Map by_path = {calloc(8192, sizeof(char *)), calloc(8192, sizeof(char *)), 8192};
  Map by_uid = {calloc(8192, sizeof(char *)), calloc(8192, sizeof(char *)), 8192};
  // (big vaults: room for the files at half load)
  if (c.nrows * 2 > 8192) {
    size_t cap = 1;
    while (cap < (size_t)c.nrows * 4) cap <<= 1;
    map_free(&by_path); map_free(&by_uid);
    by_path = (Map){calloc(cap, sizeof(char *)), calloc(cap, sizeof(char *)), cap};
    by_uid = (Map){calloc(cap, sizeof(char *)), calloc(cap, sizeof(char *)), cap};
  }
  list_files(v->root, &by_path, &by_uid);

  v->items = calloc(c.nrows, sizeof(LvItem));
  v->n = 0;
  for (int r = 1; r < c.nrows; r++) {
    char **row = c.rows[r];
    int n = c.ncells[r];
    if (n == 0 || !row[0][0]) continue;
    LvItem *it = &v->items[v->n++];
    it->id = arena_dup(&v->arena, cell(row, n, ci[0]));
    it->uid = arena_dup(&v->arena, cell(row, n, ci[1]));
    it->file = arena_dup(&v->arena, cell(row, n, ci[2]));
    it->name = arena_dup(&v->arena, cell(row, n, ci[3]));
    it->category = arena_dup(&v->arena, cell(row, n, ci[4]));
    it->kind = arena_dup(&v->arena, cell(row, n, ci[5]));
    it->nsfw = (int)int_of(cell(row, n, ci[6]));
    it->quality = (int)int_of(cell(row, n, ci[7]));
    it->added = arena_dup(&v->arena, cell(row, n, ci[8]));
    it->width = (int)int_of(cell(row, n, ci[9]));
    it->height = (int)int_of(cell(row, n, ci[10]));
    it->duration = strtod(cell(row, n, ci[11]), NULL);
    it->preview_id = arena_dup(&v->arena, cell(row, n, ci[12]));
    it->thumb = arena_dup(&v->arena, cell(row, n, ci[13]));
    it->collection = arena_dup(&v->arena, cell(row, n, ci[14]));
    it->tags = arena_dup(&v->arena, cell(row, n, ci[15]));
    it->source = arena_dup(&v->arena, cell(row, n, ci[16]));
    it->view = -1;
    // its file: where the index says (Media/ with Discreet names), else by its number anywhere
    char dir[256], rel[1024];
    if (v->discreet) snprintf(dir, sizeof dir, "Media");
    else safe_name(it->category, dir, sizeof dir);
    snprintf(rel, sizeof rel, "%s/%s", dir, it->file);
    const char *found = map_get(&by_path, rel);
    if (!found && it->uid[0]) found = map_get(&by_uid, it->uid);
    it->path = found ? arena_dup(&v->arena, found) : NULL;
  }
  map_free(&by_path);
  map_free(&by_uid);
  lv_csv_free(&c);

  // categories, most items first
  v->cats = NULL; v->cat_counts = NULL; v->ncats = 0;
  for (int i = 0; i < v->n; i++) {
    int k;
    for (k = 0; k < v->ncats; k++) if (!strcmp(v->cats[k], v->items[i].category)) break;
    if (k == v->ncats) {
      v->cats = realloc(v->cats, (v->ncats + 1) * sizeof *v->cats);
      v->cat_counts = realloc(v->cat_counts, (v->ncats + 1) * sizeof *v->cat_counts);
      v->cats[k] = dup(v->items[i].category);
      v->cat_counts[k] = 0;
      v->ncats++;
    }
    v->cat_counts[k]++;
  }
  if (v->ncats > 1) {
    int *order = malloc(v->ncats * sizeof *order);
    for (int k = 0; k < v->ncats; k++) order[k] = k;
    sort_ctx = v;
    qsort(order, v->ncats, sizeof *order, cmp_cats);
    char **cats = malloc(v->ncats * sizeof *cats);
    int *counts = malloc(v->ncats * sizeof *counts);
    for (int k = 0; k < v->ncats; k++) { cats[k] = v->cats[order[k]]; counts[k] = v->cat_counts[order[k]]; }
    free(v->cats); free(v->cat_counts); free(order);
    v->cats = cats; v->cat_counts = counts;
  }

  // views.csv
  text = read_text(v, "views.csv");
  if (text) {
    lv_csv_parse(text, &c);
    free(text);
    if (c.nrows) {
      char **vh = c.rows[0];
      int vn = c.ncells[0];
      int k_id = col(vh, vn, "id"), k_uid = col(vh, vn, "uid"), k_views = col(vh, vn, "views"),
          k_last = col(vh, vn, "last_viewed"), k_likes = col(vh, vn, "appreciations"),
          k_ll = col(vh, vn, "last_appreciated"), k_secs = col(vh, vn, "seconds_viewed");
      v->capviews = c.nrows + 64;
      v->views = calloc(v->capviews, sizeof(LvView));
      for (int r = 1; r < c.nrows; r++) {
        char **row = c.rows[r];
        int n = c.ncells[r];
        if (n == 0 || !row[0][0]) continue;
        LvView *w = &v->views[v->nviews++];
        w->id = dup(cell(row, n, k_id));
        w->uid = dup(cell(row, n, k_uid));
        w->views = int_of(cell(row, n, k_views));
        w->last = dup(cell(row, n, k_last));
        w->likes = int_of(cell(row, n, k_likes));
        w->last_like = dup(cell(row, n, k_ll));
        const char *s = cell(row, n, k_secs);
        char *end;
        double sec = strtod(s, &end);
        w->seconds = end == s || !isfinite(sec) ? 0 : sec;
      }
    }
    lv_csv_free(&c);
  }
  // each item's row
  {
    Map at = {calloc(8192, sizeof(char *)), calloc(8192, sizeof(char *)), 8192};
    if (v->nviews * 2 > 8192) {
      size_t cap = 1;
      while (cap < (size_t)v->nviews * 4) cap <<= 1;
      map_free(&at);
      at = (Map){calloc(cap, sizeof(char *)), calloc(cap, sizeof(char *)), cap};
    }
    for (int k = 0; k < v->nviews; k++) {
      char num[16];
      snprintf(num, sizeof num, "%d", k);
      map_put(&at, v->views[k].id, num);
    }
    for (int i = 0; i < v->n; i++) {
      const char *k = map_get(&at, v->items[i].id);
      v->items[i].view = k ? atoi(k) : -1;
    }
    map_free(&at);
  }
  // the hot fields, side by side
  v->hot = malloc((v->n + 1) * sizeof *v->hot);
  for (int i = 0; i < v->n; i++) {
    LvItem *it = &v->items[i];
    LvHot *h = &v->hot[i];
    h->added = time_key(it->added);
    h->kind = !strcmp(it->kind, "video") ? LV_VIDEO : !strcmp(it->kind, "gif") ? LV_GIF : LV_IMAGE;
    h->quality = (uint8_t)(it->quality < 0 ? 0 : it->quality > 255 ? 255 : it->quality);
    h->view = it->view;
    h->cat = -1;
    for (int k = 0; k < v->ncats; k++) if (!strcmp(v->cats[k], it->category)) { h->cat = (int16_t)k; break; }
  }
  return 0;
}

void lv_vault_free(LvVault *v) {
  arena_free(v->arena);
  v->arena = NULL;
  free(v->items);
  free(v->hot);
  v->hot = NULL;
  for (int k = 0; k < v->nviews; k++) {
    free(v->views[k].id); free(v->views[k].uid); free(v->views[k].last); free(v->views[k].last_like);
  }
  free(v->views);
  for (int k = 0; k < v->ncats; k++) free(v->cats[k]);
  free(v->cats);
  free(v->cat_counts);
  v->items = NULL; v->views = NULL; v->cats = NULL; v->cat_counts = NULL;
  v->n = v->nviews = v->ncats = v->capviews = 0;
}

// ---------- file ids (resolve in lib.rs) ----------
static const char *uid_of(const LvItem *it) { return it->uid[0] ? it->uid : it->id; }

static int app_file(LvVault *v, const char *id, char *out, size_t cap) {
  static const char *folders[] = {"clips", "thumbs", "streams", "audio"};
  for (int k = 0; k < 4; k++) {
    size_t l = strlen(folders[k]);
    if (!strncmp(id, folders[k], l) && id[l] == '-') {
      char name[512];
      if (lv_b64url_decode(id + l + 1, name, sizeof name) <= 0 || !plain_name(name)) return 0;
      snprintf(out, cap, "%s/_app/%s/%s", v->root, folders[k], name);
      return 1;
    }
  }
  return 0;
}

uint8_t *lv_thumb_bytes(LvVault *v, const LvItem *it, size_t *n) {
  const char *t = it->thumb;
  if (!t[0]) return NULL;
  char path[1024];
  // "<pack id>:<offset>:<length>": a slice of a thumbnail pack
  const char *c2 = strrchr(t, ':');
  if (c2 && c2 != t) {
    const char *c1 = c2 - 1;
    while (c1 > t && *c1 != ':') c1--;
    if (*c1 == ':') {
      char pack[600];
      size_t pl = (size_t)(c1 - t);
      if (pl >= sizeof pack) return NULL;
      memcpy(pack, t, pl);
      pack[pl] = 0;
      unsigned long long off = strtoull(c1 + 1, NULL, 10), len = strtoull(c2 + 1, NULL, 10);
      if (!len || len > 16u * 1024 * 1024 || !app_file(v, pack, path, sizeof path)) return NULL;
      LvFile f;
      if (lv_open(&f, path, lv_key(v)) != 0) return NULL;
      uint8_t *b = malloc((size_t)len);
      size_t got = b ? lv_read_at(&f, off, b, (size_t)len) : 0;
      lv_close(&f);
      if (got != len) { free(b); return NULL; }
      if (n) *n = got;
      return b;
    }
  }
  // a file in _app/thumbs: its own id, or (a name like 000123.webp, a Drive id) by the item's number
  if (!app_file(v, t, path, sizeof path)) snprintf(path, sizeof path, "%s/_app/thumbs/%s.webp", v->root, uid_of(it));
  return lv_read_all(path, lv_key(v), n);
}

int lv_clip_path(LvVault *v, const LvItem *it, char *out, size_t cap) {
  if (!it->preview_id[0]) return 0;
  if (!app_file(v, it->preview_id, out, cap)) snprintf(out, cap, "%s/_app/clips/%s.mp4", v->root, uid_of(it));
  return exists(out);
}

// ---------- views ----------
static LvView *row_for(LvVault *v, LvItem *it) {
  if (it->view >= 0) return &v->views[it->view];
  if (v->nviews == v->capviews) {
    v->capviews = v->capviews ? v->capviews * 2 : 64;
    v->views = realloc(v->views, v->capviews * sizeof *v->views);
  }
  LvView *w = &v->views[v->nviews];
  memset(w, 0, sizeof *w);
  w->id = dup(it->id);
  w->uid = dup(it->uid);
  w->last = dup("");
  w->last_like = dup("");
  it->view = v->nviews++;
  if (v->hot) v->hot[it - v->items].view = it->view;
  return w;
}
void lv_viewed(LvVault *v, LvItem *it, const char *now) {
  LvView *w = row_for(v, it);
  w->views++;
  if (strcmp(now, w->last) > 0) { free(w->last); w->last = dup(now); }
  v->views_dirty = 1;
}
void lv_liked(LvVault *v, LvItem *it, const char *now) {
  LvView *w = row_for(v, it);
  w->likes++;
  if (strcmp(now, w->last_like) > 0) { free(w->last_like); w->last_like = dup(now); }
  v->views_dirty = 1;
}
void lv_add_seconds(LvVault *v, LvItem *it, double secs) {
  if (!(secs > 0) || !isfinite(secs)) return;
  row_for(v, it)->seconds += secs;
  v->views_dirty = 1;
}
long long lv_views_of(const LvVault *v, const LvItem *it) { return it->view >= 0 ? v->views[it->view].views : 0; }
long long lv_likes_of(const LvVault *v, const LvItem *it) { return it->view >= 0 ? v->views[it->view].likes : 0; }

char *lv_views_csv(const LvVault *v) {
  int nrows = v->nviews + 1;
  char ***rows = malloc(nrows * sizeof *rows);
  int *ncells = malloc(nrows * sizeof *ncells);
  static char *head[] = {"id", "uid", "views", "last_viewed", "appreciations", "last_appreciated", "seconds_viewed"};
  rows[0] = head;
  ncells[0] = 7;
  char (*nums)[3][32] = malloc(v->nviews * sizeof *nums + 1);
  char **cells = malloc((size_t)v->nviews * 7 * sizeof *cells + 1);
  for (int k = 0; k < v->nviews; k++) {
    const LvView *w = &v->views[k];
    snprintf(nums[k][0], 32, "%lld", w->views);
    snprintf(nums[k][1], 32, "%lld", w->likes);
    lv_num_or_empty(round(w->seconds * 10.0) / 10.0, nums[k][2], 32);
    char **r = cells + k * 7;
    r[0] = w->id; r[1] = w->uid; r[2] = nums[k][0]; r[3] = w->last; r[4] = nums[k][1]; r[5] = w->last_like; r[6] = nums[k][2];
    rows[k + 1] = r;
    ncells[k + 1] = 7;
  }
  char *out = lv_csv_text(rows, ncells, nrows);
  free(rows); free(ncells); free(nums); free(cells);
  return out;
}

int lv_vault_save_views(LvVault *v) {
  if (!v->views_dirty) return 0;
  char *text = lv_views_csv(v);
  char p[700];
  snprintf(p, sizeof p, "%s/views.csv", v->root);
  int r = lv_write_all(p, lv_key(v), text, strlen(text));
  free(text);
  if (r == 0) v->views_dirty = 0;
  return r;
}
