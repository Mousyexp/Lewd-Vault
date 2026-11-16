#include "vault.h"
#include "lvcrypt.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Out of memory (the Vita's heap is fixed): nothing crashes on a failed allocation. Strings come back
// as a shared empty one (never freed: sfree knows it), buffers stop growing, and lv_oom is set so a
// load can say so instead of showing half a vault.
int lv_oom;
static char empty_[1];
static void sfree(char *p) { if (p && p != empty_) free(p); }
static char *dupn(const char *s, size_t n) {
  char *d = malloc(n + 1);
  if (!d) { lv_oom = 1; return empty_; }
  memcpy(d, s, n);
  d[n] = 0;
  return d;
}
static char *sdup(const char *s) { return dupn(s ? s : "", strlen(s ? s : "")); }

// The items' strings: thousands of small ones, made once and dropped together, so they go into a few
// big blocks instead of one malloc each (less time, no fragmentation of the Vita's fixed heap).
typedef struct LvArena { struct LvArena *next; size_t used, cap; char data[]; } LvArena;
static char *arena_dup(LvArena **a, const char *s) {
  size_t n = strlen(s) + 1;
  if (!*a || (*a)->used + n > (*a)->cap) {
    size_t cap = n > 256 * 1024 ? n : 256 * 1024;
    LvArena *b = malloc(sizeof *b + cap);
    if (!b) { lv_oom = 1; return empty_; }
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
    size_t cap = (b->n + n + 1) * 2;
    char *p = realloc(b->s, cap);
    if (!p) { lv_oom = 1; return; }
    b->s = p;
    b->cap = cap;
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
  if (!cell.s) return;
  int quoted = 0;
  // (out of memory: cells and rows stop being added, and lv_oom says so)
#define PUSH_CELL() do { if (ncells == capcells) { int c_ = capcells ? capcells * 2 : 16; char **r_ = realloc(row, c_ * sizeof *row); \
    if (r_) { row = r_; capcells = c_; } else lv_oom = 1; } if (ncells < capcells) row[ncells++] = sdup(cell.s); cell.n = 0; cell.s[0] = 0; } while (0)
#define PUSH_ROW() do { if (out->nrows == caprows) { int c_ = caprows ? caprows * 2 : 64; char ***r_ = realloc(out->rows, c_ * sizeof *out->rows); \
    if (r_) { out->rows = r_; } int *n_ = realloc(out->ncells, c_ * sizeof *out->ncells); if (n_) { out->ncells = n_; } \
    if (r_ && n_) caprows = c_; else lv_oom = 1; } if (out->nrows < caprows) { out->rows[out->nrows] = row; out->ncells[out->nrows++] = ncells; } \
    else { for (int k_ = 0; k_ < ncells; k_++) sfree(row[k_]); free(row); } row = NULL; ncells = 0; capcells = 0; } while (0)
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
    for (int k = 0; k < c->ncells[r]; k++) sfree(c->rows[r][k]);
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

void lv_b64url_encode(const char *in, char *out, size_t cap) {
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t o = 0, n = strlen(in);
  for (size_t i = 0; i < n && o + 5 < cap; i += 3) {
    unsigned v = (unsigned char)in[i] << 16 | (i + 1 < n ? (unsigned char)in[i + 1] << 8 : 0) | (i + 2 < n ? (unsigned char)in[i + 2] : 0);
    out[o++] = A[v >> 18 & 63];
    out[o++] = A[v >> 12 & 63];
    if (i + 1 < n) out[o++] = A[v >> 6 & 63];
    if (i + 2 < n) out[o++] = A[v & 63];
  }
  out[o] = 0;
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
  // a write cut short (the battery, a crash): lv_write_all moves the old file aside as .old, the
  // new one in from .part, then drops .old. Whatever step it stopped at, this finishes or undoes it.
  static const char *files[] = {"index.csv", "views.csv", "_app/state.json", "_app/settings.json"};
  for (size_t k = 0; k < sizeof files / sizeof *files; k++) {
    char f[700], old[720], part[720];
    snprintf(f, sizeof f, "%s/%s", v->root, files[k]);
    snprintf(old, sizeof old, "%s.old", f);
    snprintf(part, sizeof part, "%s.part", f);
    struct stat st;
    if (stat(f, &st) != 0 && stat(old, &st) == 0) rename(old, f);
    else if (stat(old, &st) == 0) remove(old);
    remove(part);
  }
}

// A copy of index.csv, views.csv and settings.json in _app/backups/<today>, before the first change
// of the day, the 14 newest kept (backup_daily in lib.rs: the same folders, so the PC's restore finds
// them). Copied as they are, still encrypted in an Untraceable vault.
int lv_backup_days = 14;
static void backup_daily(LvVault *v, const char *today) {
  char dir[700], from[700], to[1100];
  snprintf(dir, sizeof dir, "%s/_app/backups", v->root);
  mkdir(dir, 0777);
  snprintf(dir, sizeof dir, "%s/_app/backups/%.10s", v->root, today);
  struct stat st;
  if (stat(dir, &st) == 0 || mkdir(dir, 0777) != 0) return;
  static const char *names[][2] = {{"index.csv", "index.csv"}, {"views.csv", "views.csv"}, {"_app/settings.json", "settings.json"}};
  for (int k = 0; k < 3; k++) {
    snprintf(from, sizeof from, "%s/%s", v->root, names[k][0]);
    snprintf(to, sizeof to, "%s/%s", dir, names[k][1]);
    if (stat(from, &st) != 0 || st.st_size == 0) continue;
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
    char buf[16384];
    size_t n;
    while (a && b && (n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    if (a) fclose(a);
    if (b) fclose(b);
  }
  // the oldest go
  char days[64][16];
  int nd = 0;
  snprintf(dir, sizeof dir, "%s/_app/backups", v->root);
  DIR *d = opendir(dir);
  struct dirent *e;
  while (d && (e = readdir(d)) && nd < 64)
    if (!strncmp(e->d_name, "20", 2) && strlen(e->d_name) == 10) snprintf(days[nd++], 16, "%s", e->d_name);
  if (d) closedir(d);
  while (nd > lv_backup_days) {
    int oldest = 0;
    for (int k = 1; k < nd; k++) if (strcmp(days[k], days[oldest]) < 0) oldest = k;
    for (int k = 0; k < 3; k++) {
      snprintf(to, sizeof to, "%s/%s/%s", dir, days[oldest], names[k][1]);
      remove(to);
    }
    snprintf(to, sizeof to, "%s/%s", dir, days[oldest]);
    rmdir(to);
    memcpy(days[oldest], days[--nd], 16);
  }
}
static void backup_today(LvVault *v) {
  time_t t = time(NULL);
  struct tm l;
  localtime_r(&t, &l);
  char d[16];
  strftime(d, sizeof d, "%Y-%m-%d", &l);
  backup_daily(v, d);
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
  m->keys[i] = sdup(k);
  m->vals[i] = sdup(val);
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
  lv_oom = 0;
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
      v->cats[k] = sdup(v->items[i].category);
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
        w->id = sdup(cell(row, n, k_id));
        w->uid = sdup(cell(row, n, k_uid));
        w->views = int_of(cell(row, n, k_views));
        w->last = sdup(cell(row, n, k_last));
        w->likes = int_of(cell(row, n, k_likes));
        w->last_like = sdup(cell(row, n, k_ll));
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
  if (!v->hot || !v->items || lv_oom) {
    lv_vault_free(v);
    lv_oom = 0;
    msg = "Not enough memory for this vault";
    if (why) *why = msg;
    return -1;
  }
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
    sfree(v->views[k].id); sfree(v->views[k].uid); sfree(v->views[k].last); sfree(v->views[k].last_like);
  }
  free(v->views);
  for (int k = 0; k < v->ncats; k++) sfree(v->cats[k]);
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
  w->id = sdup(it->id);
  w->uid = sdup(it->uid);
  w->last = sdup("");
  w->last_like = sdup("");
  it->view = v->nviews++;
  if (v->hot) v->hot[it - v->items].view = it->view;
  return w;
}
void lv_viewed(LvVault *v, LvItem *it, const char *now) {
  LvView *w = row_for(v, it);
  w->views++;
  if (strcmp(now, w->last) > 0) { sfree(w->last); w->last = sdup(now); }
  v->views_dirty = 1;
}
void lv_liked(LvVault *v, LvItem *it, const char *now) {
  LvView *w = row_for(v, it);
  w->likes++;
  if (strcmp(now, w->last_like) > 0) { sfree(w->last_like); w->last_like = sdup(now); }
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
  backup_today(v);
  char *text = lv_views_csv(v);
  char p[700];
  snprintf(p, sizeof p, "%s/views.csv", v->root);
  int r = lv_write_all(p, lv_key(v), text, strlen(text));
  free(text);
  if (r == 0) v->views_dirty = 0;
  return r;
}

// ---------- adding an item (the Browser's saves) ----------

static void csv_cell(Buf *b, const char *s) {
  if (strpbrk(s, "\",\r\n")) {
    bstr(b, "\"");
    for (const char *p = s; *p; p++) { bput(b, p, 1); if (*p == '"') bput(b, "\"", 1); }
    bstr(b, "\"");
  } else bstr(b, s);
}

// The next item number: state.json's nextUid, and never one already in the index.
static long next_uid(LvVault *v) {
  long next = 1;
  char *st = read_text(v, "_app/state.json");
  if (st) {
    char *k = strstr(st, "\"nextUid\"");
    if (k && (k = strchr(k, ':'))) next = strtol(k + 1, NULL, 10);
    free(st);
  }
  for (int i = 0; i < v->n; i++) {
    long u = strtol(v->items[i].uid, NULL, 10);
    if (u >= next) next = u + 1;
  }
  return next < 1 ? 1 : next;
}
static void save_next_uid(LvVault *v, long next) {
  char p[700], text[64];
  snprintf(p, sizeof p, "%s/_app/state.json", v->root);
  char *st = read_text(v, "_app/state.json");
  // (keeps whatever else is in it: only the number changes)
  char *k = st ? strstr(st, "\"nextUid\"") : NULL;
  if (k && (k = strchr(k, ':'))) {
    char *e = k + 1;
    while (*e == ' ') e++;
    while (*e >= '0' && *e <= '9') e++;
    Buf b = {0};
    bput(&b, st, (size_t)(k + 1 - st));
    snprintf(text, sizeof text, "%ld", next);
    bstr(&b, text);
    bstr(&b, e);
    lv_write_all(p, lv_key(v), b.s, b.n);
    free(b.s);
  } else {
    snprintf(text, sizeof text, "{\"nextUid\":%ld}", next);
    lv_write_all(p, lv_key(v), text, strlen(text));
  }
  free(st);
}

int lv_vault_add_files(LvVault *v, const LvNew *n, const uint8_t *data, size_t len, const char *ext,
                       const uint8_t *thumb, size_t tlen, int tw, int th, LvAdded *out, const char **why) {
  static const char *msg;
  if (v->untraceable && !v->has_key) { msg = "The vault is locked"; *why = msg; return -1; }
  backup_today(v);
  long next = next_uid(v);
  char uid[16], dir[300], name[400], path[1300], tname[64], tpath[800], tid[128], tb[96];
  snprintf(uid, sizeof uid, "%06ld", next);
  // where the PC would put it: Media/ with Discreet names, else the category's folder
  char kind_letter = !strcmp(n->kind, "video") ? 'v' : !strcmp(n->kind, "gif") ? 'g' : 'i';
  if (v->discreet) {
    snprintf(dir, sizeof dir, "Media");
    snprintf(name, sizeof name, "%s_file.%s", uid, ext);
  } else {
    safe_name(n->category, dir, sizeof dir);
    char ymd[8] = "000000";
    if (strlen(n->added) >= 10) snprintf(ymd, sizeof ymd, "%.2s%.2s%.2s", n->added + 2, n->added + 5, n->added + 8);
    snprintf(name, sizeof name, "%s_%s_%c%02d%d_%s.%s", uid, ymd, kind_letter, n->nsfw, n->quality > 9 ? 9 : n->quality, dir, ext);
  }
  snprintf(path, sizeof path, "%s/%s", v->root, dir);
  mkdir(path, 0777);
  snprintf(path, sizeof path, "%s/%s/%s", v->root, dir, name);
  // (data NULL: the file is at n->from, copied in pieces: a big video never has to fit in memory)
  if (data ? lv_write_all(path, lv_key(v), data, len) != 0 : lv_copy_file(n->from, path, lv_key(v)) != 0) { msg = "The file could not be written (is the memory card full?)"; *why = msg; return -1; }
  // the thumbnail, made card-sized, in _app/thumbs (named the way the PC's are)
  tid[0] = 0;
  if (thumb && tlen) {
    snprintf(tpath, sizeof tpath, "%s/_app", v->root);
    mkdir(tpath, 0777);
    snprintf(tpath, sizeof tpath, "%s/_app/thumbs", v->root);
    mkdir(tpath, 0777);
    snprintf(tname, sizeof tname, "%s.jpg", uid);
    snprintf(tpath, sizeof tpath, "%s/_app/thumbs/%s", v->root, tname);
    if (lv_write_all(tpath, lv_key(v), thumb, tlen) == 0) {
      lv_b64url_encode(tname, tb, sizeof tb);
      snprintf(tid, sizeof tid, "thumbs-%s", tb);
    }
  }
  // the row, in index.csv's columns (HEADERS in lib.rs), appended
  char id[24], nsfw[8], q[8], w[16], h[16], d[32], tsize[32];
  snprintf(id, sizeof id, "item-%s", uid);
  snprintf(nsfw, sizeof nsfw, "%d", n->nsfw);
  snprintf(q, sizeof q, "%d", n->quality);
  lv_num_or_empty(n->width, w, sizeof w);
  lv_num_or_empty(n->height, h, sizeof h);
  lv_num_or_empty(n->duration, d, sizeof d);
  if (tw && th) snprintf(tsize, sizeof tsize, "%dx%d", tw, th); else tsize[0] = 0;
  const char *cells[21] = {id, uid, name, n->name, n->category, n->kind, nsfw, q, n->added, w, h, d, "", tid, "", "", tsize, "", "", n->tags, n->source};
  Buf row = {0};
  for (int k = 0; k < 21; k++) { if (k) bstr(&row, ","); csv_cell(&row, cells[k]); }
  bstr(&row, "\r\n");
  char ip[700];
  snprintf(ip, sizeof ip, "%s/index.csv", v->root);
  int ok = lv_append(ip, lv_key(v), row.s, row.n) == 0;
  free(row.s);
  if (!ok) { remove(path); msg = "index.csv could not be written"; *why = msg; return -1; }
  save_next_uid(v, next + 1);
  snprintf(out->uid, sizeof out->uid, "%s", uid);
  snprintf(out->name, sizeof out->name, "%s", name);
  snprintf(out->path, sizeof out->path, "%s", path);
  snprintf(out->thumb, sizeof out->thumb, "%s", tid);
  return 0;
}

// The new item into the loaded vault, without reading it all again.
int lv_vault_add_memory(LvVault *v, const LvNew *n, const LvAdded *a) {
  char id[24];
  snprintf(id, sizeof id, "item-%s", a->uid);
  const char *uid = a->uid, *name = a->name, *path = a->path, *tid = a->thumb;
  char kind_letter = !strcmp(n->kind, "video") ? 'v' : !strcmp(n->kind, "gif") ? 'g' : 'i';
  LvItem *items = realloc(v->items, (v->n + 1) * sizeof *items);
  LvHot *hot = realloc(v->hot, (v->n + 2) * sizeof *hot);
  if (!items || !hot) { if (items) v->items = items; if (hot) v->hot = hot; return -1; }
  v->items = items;
  v->hot = hot;
  LvItem *it = &v->items[v->n];
  memset(it, 0, sizeof *it);
  it->id = arena_dup(&v->arena, id); it->uid = arena_dup(&v->arena, uid); it->file = arena_dup(&v->arena, name);
  it->name = arena_dup(&v->arena, n->name); it->category = arena_dup(&v->arena, n->category); it->kind = arena_dup(&v->arena, n->kind);
  it->added = arena_dup(&v->arena, n->added); it->preview_id = arena_dup(&v->arena, ""); it->thumb = arena_dup(&v->arena, tid);
  it->collection = arena_dup(&v->arena, ""); it->tags = arena_dup(&v->arena, n->tags); it->source = arena_dup(&v->arena, n->source);
  it->path = arena_dup(&v->arena, path);
  it->nsfw = n->nsfw; it->quality = n->quality; it->width = n->width; it->height = n->height; it->duration = n->duration;
  it->view = -1;
  int c;
  for (c = 0; c < v->ncats; c++) if (!strcmp(v->cats[c], n->category)) break;
  if (c == v->ncats) {
    v->cats = realloc(v->cats, (v->ncats + 1) * sizeof *v->cats);
    v->cat_counts = realloc(v->cat_counts, (v->ncats + 1) * sizeof *v->cat_counts);
    v->cats[c] = sdup(n->category);
    v->cat_counts[c] = 0;
    v->ncats++;
  }
  v->cat_counts[c]++;
  LvHot *hh = &v->hot[v->n];
  hh->added = time_key(n->added);
  hh->cat = (int16_t)c;
  hh->kind = kind_letter == 'v' ? LV_VIDEO : kind_letter == 'g' ? LV_GIF : LV_IMAGE;
  hh->quality = (uint8_t)n->quality;
  hh->view = -1;
  v->n++;
  return 0;
}
int lv_vault_add(LvVault *v, const LvNew *n, const uint8_t *data, size_t len, const char *ext,
                 const uint8_t *thumb, size_t tlen, int tw, int th, const char **why) {
  LvAdded a;
  if (lv_vault_add_files(v, n, data, len, ext, thumb, tlen, tw, th, &a, why) != 0) return -1;
  lv_vault_add_memory(v, n, &a); // (out of memory: it's on the card all the same, and shows next time)
  return 0;
}
