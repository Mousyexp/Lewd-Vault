#include "vault.h"
#include "lvcrypt.h"
#include <ctype.h>
#include <dirent.h>
#include <strings.h>
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
// room for n pointers (an item's cells), aligned
static char **arena_cells(LvArena **a, int n) {
  size_t need = (size_t)n * sizeof(char *) + sizeof(char *);
  if (!*a || (*a)->used + need > (*a)->cap) {
    size_t cap = need > 256 * 1024 ? need : 256 * 1024;
    LvArena *b = malloc(sizeof *b + cap);
    if (!b) { lv_oom = 1; return NULL; }
    b->next = *a; b->used = 0; b->cap = cap; *a = b;
  }
  size_t at = ((*a)->used + sizeof(char *) - 1) & ~(sizeof(char *) - 1);
  if (at + (size_t)n * sizeof(char *) > (*a)->cap) { (*a)->used = (*a)->cap; return arena_cells(a, n); }
  char **p = (char **)((*a)->data + at);
  (*a)->used = at + (size_t)n * sizeof(char *);
  return p;
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
static void backup_today(LvVault *v);
void lv_local_tm(time_t t, struct tm *out);
static void backup_daily(LvVault *v, const char *today) {
  char dir[700], from[700], to[1800];
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
void lv_backup_today_root(const char *root) {
  // (the day's backup for the vault at root: a writer thread's way in, with no loaded vault to hand)
  static LvVault tmp;
  memset(&tmp, 0, sizeof tmp);
  snprintf(tmp.root, sizeof tmp.root, "%s", root);
  backup_today(&tmp);
}
static void backup_today(LvVault *v) {
  time_t t = time(NULL);
  struct tm l;
  lv_local_tm(t, &l);
  char d[16];
  strftime(d, sizeof d, "%Y-%m-%d", &l);
  backup_daily(v, d);
}

const uint8_t *lv_key(const LvVault *v) { return v->has_key ? v->key : NULL; }

// The local time; where the C library can't tell it (Vita3K's gives 1970), the UTC one instead.
void lv_local_tm(time_t t, struct tm *out) {
  if (!localtime_r(&t, out) || (out->tm_year < 100 && t > 946684800)) gmtime_r(&t, out);
}

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
  if (!m->keys || !m->vals) return; // (no memory for the table: nothing found later, nothing crashes)
  size_t i = hash(k) & (m->cap - 1), probes = 0;
  while (m->keys[i]) {
    if (!strcmp(m->keys[i], k)) return; // (the first one found wins)
    i = (i + 1) & (m->cap - 1);
    if (++probes >= m->cap) return; // (full: never round and round)
  }
  m->keys[i] = sdup(k);
  m->vals[i] = sdup(val);
}
static const char *map_get(const Map *m, const char *k) {
  if (!m->keys || !m->vals) return NULL;
  size_t i = hash(k) & (m->cap - 1), probes = 0;
  while (m->keys[i]) {
    if (!strcmp(m->keys[i], k)) return m->vals[i];
    i = (i + 1) & (m->cap - 1);
    if (++probes >= m->cap) break;
  }
  return NULL;
}
static void map_free(Map *m) {
  for (size_t i = 0; m->keys && i < m->cap; i++) { sfree(m->keys[i]); sfree(m->vals[i]); }
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

static int grow_cats(LvVault *v, const char *name); // (below)
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
  v->head = arena_cells(&v->arena, hn);
  v->nhead = v->head ? hn : 0;
  for (int k = 0; k < v->nhead; k++) v->head[k] = arena_dup(&v->arena, h[k]);

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
    // every cell kept (a rewrite gives back the same row); the fields are those same strings
    it->ncells = n;
    it->cells = arena_cells(&v->arena, n);
    if (!it->cells) { it->ncells = 0; v->n--; continue; }
    for (int k = 0; k < n; k++) it->cells[k] = arena_dup(&v->arena, row[k]);
    row = it->cells;
#define F(k) ((ci[k] >= 0 && ci[k] < n) ? row[ci[k]] : empty_)
    it->id = F(0); it->uid = F(1); it->file = F(2); it->name = F(3); it->category = F(4); it->kind = F(5);
    it->nsfw = (int)int_of(F(6));
    it->quality = (int)int_of(F(7));
    it->added = F(8);
    it->width = (int)int_of(F(9));
    it->height = (int)int_of(F(10));
    it->duration = strtod(F(11), NULL);
    it->preview_id = F(12); it->thumb = F(13); it->collection = F(14); it->tags = F(15); it->source = F(16);
#undef F
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
    if (k == v->ncats && !grow_cats(v, v->items[i].category)) { lv_oom = 1; continue; }
    v->cat_counts[k]++;
  }
  if (v->ncats > 1) {
    int *order = malloc(v->ncats * sizeof *order);
    char **cats = malloc(v->ncats * sizeof *cats);
    int *counts = malloc(v->ncats * sizeof *counts);
    if (order && cats && counts) { // (no memory: they stay in the order found)
      for (int k = 0; k < v->ncats; k++) order[k] = k;
      sort_ctx = v;
      qsort(order, v->ncats, sizeof *order, cmp_cats);
      for (int k = 0; k < v->ncats; k++) { cats[k] = v->cats[order[k]]; counts[k] = v->cat_counts[order[k]]; }
      free(v->cats); free(v->cat_counts);
      v->cats = cats; v->cat_counts = counts;
      cats = NULL; counts = NULL;
    }
    free(order); free(cats); free(counts);
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
      if (!v->views) { v->capviews = 0; lv_oom = 1; }
      for (int r = 1; r < c.nrows && v->views; r++) {
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

// One more category (its name copied, its count 0): 1, or 0 with nothing changed when there's no memory.
static int grow_cats(LvVault *v, const char *name) {
  char **nc = realloc(v->cats, (v->ncats + 1) * sizeof *nc);
  if (!nc) return 0;
  v->cats = nc;
  int *nn = realloc(v->cat_counts, (v->ncats + 1) * sizeof *nn);
  if (!nn) return 0;
  v->cat_counts = nn;
  char *d = sdup(name);
  if (!d) return 0;
  v->cats[v->ncats] = d;
  v->cat_counts[v->ncats] = 0;
  v->ncats++;
  return 1;
}

// ---------- views ----------
static LvView scratch_view; // (out of memory: a row that's counted in nowhere, rather than a crash)
static LvView *row_for(LvVault *v, LvItem *it) {
  if (it->view >= 0) return &v->views[it->view];
  if (v->nviews == v->capviews) {
    int cap = v->capviews ? v->capviews * 2 : 64;
    LvView *nv = realloc(v->views, cap * sizeof *nv);
    if (!nv) { lv_oom = 1; memset(&scratch_view, 0, sizeof scratch_view); scratch_view.id = scratch_view.uid = scratch_view.last = scratch_view.last_like = sdup(""); return &scratch_view; }
    v->views = nv; v->capviews = cap;
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
  char (*nums)[3][32] = malloc(v->nviews * sizeof *nums + 1);
  char **cells = malloc((size_t)v->nviews * 7 * sizeof *cells + 1);
  if (!rows || !ncells || !nums || !cells) { free(rows); free(ncells); free(nums); free(cells); lv_oom = 1; return NULL; }
  rows[0] = head;
  ncells[0] = 7;
  for (int k = 0; k < v->nviews; k++) {
    const LvView *w = &v->views[k];
    snprintf(nums[k][0], 32, "%lld", w->views);
    snprintf(nums[k][1], 32, "%lld", w->likes);
    // (the seconds to a tenth, as JavaScript prints them: whole tenths written straight, not found
    //  by printing and reading back as lv_num_or_empty does for any number)
    double t = round(w->seconds * 10.0);
    if (isfinite(t) && fabs(t) < 9e15) {
      long long tt = (long long)t;
      if (!tt) nums[k][2][0] = 0;
      else if (tt % 10 == 0) snprintf(nums[k][2], 32, "%lld", tt / 10);
      else snprintf(nums[k][2], 32, "%s%lld.%lld", tt < 0 ? "-" : "", llabs(tt) / 10, llabs(tt) % 10);
    } else lv_num_or_empty(t / 10.0, nums[k][2], 32);
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
  if (!text) return -1;
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
  const char *cells[21] = {id, uid, name, n->name, n->category, n->kind, nsfw, q, n->added, w, h, d, "", tid, "", "", tsize, n->collection ? n->collection : "", "", n->tags, n->source};
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
  snprintf(out->tsize, sizeof out->tsize, "%s", tsize);
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
  it->collection = arena_dup(&v->arena, n->collection ? n->collection : ""); it->tags = arena_dup(&v->arena, n->tags); it->source = arena_dup(&v->arena, n->source);
  it->path = arena_dup(&v->arena, path);
  {
    // its cells, in the file's own column order (as the row appended was, by name)
    static const char *std[21] = {"id", "uid", "file", "original_name", "category", "kind", "nsfw", "quality", "added", "width", "height",
                                  "duration", "preview_id", "thumb", "thumbhash", "stream_id", "thumb_size", "collection", "collection_cover", "tags", "source"};
    char nsfw[8], q[8], w[16], h[16], d[32];
    snprintf(nsfw, sizeof nsfw, "%d", n->nsfw);
    snprintf(q, sizeof q, "%d", n->quality);
    lv_num_or_empty(n->width, w, sizeof w);
    lv_num_or_empty(n->height, h, sizeof h);
    lv_num_or_empty(n->duration, d, sizeof d);
    const char *val[21] = {it->id, it->uid, it->file, it->name, it->category, it->kind, nsfw, q, it->added, w, h, d, "", it->thumb, "", "", a->tsize, it->collection, "", it->tags, it->source};
    int nh = v->nhead ? v->nhead : 21;
    it->cells = arena_cells(&v->arena, nh);
    it->ncells = it->cells ? nh : 0;
    for (int c = 0; c < it->ncells; c++) {
      const char *name = v->nhead ? v->head[c] : std[c], *x = "";
      for (int k = 0; k < 21; k++) if (!strcmp(name, std[k])) { x = val[k]; break; }
      it->cells[c] = arena_dup(&v->arena, x);
    }
  }
  it->nsfw = n->nsfw; it->quality = n->quality; it->width = n->width; it->height = n->height; it->duration = n->duration;
  it->view = -1;
  int c;
  for (c = 0; c < v->ncats; c++) if (!strcmp(v->cats[c], n->category)) break;
  if (c == v->ncats && !grow_cats(v, n->category)) c = 0; // (no memory for one more: counted with the first)
  if (c < v->ncats) v->cat_counts[c]++;
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

// ---------- editing ----------

static int head_col(const LvVault *v, const char *name) {
  for (int k = 0; k < v->nhead; k++) if (!strcmp(v->head[k], name)) return k;
  return -1;
}

int lv_item_set(LvVault *v, int i, const char *column, const char *value) {
  if (i < 0 || i >= v->n) return -1;
  int k = head_col(v, column);
  LvItem *it = &v->items[i];
  if (k < 0 || k >= it->ncells) return -1;
  char *x = arena_dup(&v->arena, value);
  it->cells[k] = x;
  // the fields that are this cell
  if (!strcmp(column, "category")) {
    it->category = x;
    int c;
    for (c = 0; c < v->ncats; c++) if (!strcmp(v->cats[c], x)) break;
    if (c == v->ncats) {
      char **cats = realloc(v->cats, (v->ncats + 1) * sizeof *cats);
      int *counts = realloc(v->cat_counts, (v->ncats + 1) * sizeof *counts);
      if (cats) v->cats = cats;
      if (counts) v->cat_counts = counts;
      if (!cats || !counts) return -1;
      v->cats[c] = sdup(x);
      v->cat_counts[c] = 0;
      v->ncats++;
    }
    if (v->hot[i].cat >= 0 && v->hot[i].cat < v->ncats) v->cat_counts[v->hot[i].cat]--;
    v->cat_counts[c]++;
    v->hot[i].cat = (int16_t)c;
  } else if (!strcmp(column, "tags")) it->tags = x;
  else if (!strcmp(column, "collection")) it->collection = x;
  else if (!strcmp(column, "original_name")) it->name = x;
  else if (!strcmp(column, "nsfw")) it->nsfw = (int)int_of(x);
  else if (!strcmp(column, "quality")) { it->quality = (int)int_of(x); v->hot[i].quality = (uint8_t)(it->quality < 0 ? 0 : it->quality); }
  v->index_dirty = 1;
  return 0;
}

char *lv_index_csv(const LvVault *v) {
  int nrows = v->n + 1;
  char ***rows = malloc(nrows * sizeof *rows);
  int *nc = malloc(nrows * sizeof *nc);
  if (!rows || !nc) { free(rows); free(nc); return NULL; }
  rows[0] = v->head;
  nc[0] = v->nhead;
  for (int i = 0; i < v->n; i++) { rows[i + 1] = v->items[i].cells; nc[i + 1] = v->items[i].ncells; }
  char *t = lv_csv_text(rows, nc, nrows);
  free(rows);
  free(nc);
  return t;
}

int lv_vault_save_index(LvVault *v) {
  if (!v->index_dirty) return 0;
  if (!v->nhead) return -1; // (never write a file without its columns)
  backup_today(v);
  char *t = lv_index_csv(v);
  if (!t) return -1;
  char p[700];
  snprintf(p, sizeof p, "%s/index.csv", v->root);
  int r = lv_write_all(p, lv_key(v), t, strlen(t));
  free(t);
  if (r == 0) v->index_dirty = 0;
  return r;
}

static void to_trash(LvVault *v, const char *path) {
  if (!path || !path[0]) return;
  struct stat st;
  if (stat(path, &st) != 0) return;
  char bin[700], to[1400];
  snprintf(bin, sizeof bin, "%s/_trash", v->root);
  mkdir(bin, 0777);
  const char *name = strrchr(path, '/');
  snprintf(to, sizeof to, "%s/%s", bin, name ? name + 1 : path);
  remove(to);
  if (rename(path, to) != 0 && lv_copy_file(path, to, NULL) == 0) remove(path);
}

int lv_vault_delete(LvVault *v, int i, int save) {
  if (i < 0 || i >= v->n) return -1;
  LvItem *it = &v->items[i];
  // its file, its clip, and its thumbnail when that's a file of its own (not a slice of a pack)
  to_trash(v, it->path);
  char p[1100];
  if (lv_clip_path(v, it, p, sizeof p)) to_trash(v, p);
  if (it->thumb[0] && !strchr(it->thumb, ':') && app_file(v, it->thumb, p, sizeof p)) to_trash(v, p);
  if (v->hot[i].cat >= 0 && v->hot[i].cat < v->ncats) v->cat_counts[v->hot[i].cat]--;
  memmove(&v->items[i], &v->items[i + 1], (size_t)(v->n - i - 1) * sizeof *v->items);
  memmove(&v->hot[i], &v->hot[i + 1], (size_t)(v->n - i - 1) * sizeof *v->hot);
  v->n--;
  v->index_dirty = 1;
  return save ? lv_vault_save_index(v) : 0;
}

// ---------- searching ----------

void lv_search_compile(const char *q, LvSearch *s) {
  memset(s, 0, sizeof *s);
  const char *p = q;
  while (*p && s->n < 24) {
    while (*p == ' ') p++;
    const char *e = p;
    while (*e && *e != ' ') e++;
    if (e > p) {
      char mode = '+';
      if (*p == '-' || *p == '~') { mode = *p; p++; }
      size_t n = (size_t)(e - p);
      if (n > 0 && n < 64) {
        for (size_t k = 0; k < n; k++) s->words[s->n][k] = (char)((p[k] >= 'A' && p[k] <= 'Z') ? p[k] + 32 : p[k]);
        s->words[s->n][n] = 0;
        s->mode[s->n++] = mode;
      }
    }
    p = e;
  }
}

static int ci_contains(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  for (; *hay; hay++) {
    size_t k = 0;
    while (k < n && hay[k] && ((hay[k] >= 'A' && hay[k] <= 'Z') ? hay[k] + 32 : hay[k]) == needle[k]) k++;
    if (k == n) return 1;
  }
  return 0;
}
// a tag in the item's tags (exactly, or up to a * at the end; case doesn't matter)
static int has_tag(const char *tags, const char *w) {
  size_t wl = strlen(w);
  int star = wl && w[wl - 1] == '*';
  if (star) wl--;
  const char *p = tags;
  while (*p) {
    while (*p == ' ') p++;
    const char *e = p;
    while (*e && *e != ' ') e++;
    size_t n = (size_t)(e - p);
    if (n && (star ? n >= wl : n == wl)) {
      size_t k = 0;
      while (k < wl && ((p[k] >= 'A' && p[k] <= 'Z') ? p[k] + 32 : p[k]) == w[k]) k++;
      if (k == wl) return 1;
    }
    p = e;
  }
  return 0;
}
static int word_matches(const char *w, const LvItem *it) {
  if (has_tag(it->tags, w)) return 1;
  if (strchr(w, '*') || strchr(w, ':')) return 0;
  return ci_contains(it->name, w) || ci_contains(it->category, w) || ci_contains(it->collection, w);
}
int lv_search_match(const LvSearch *s, const LvItem *it) {
  int any = 0, any_ok = 0;
  for (int k = 0; k < s->n; k++) {
    int m = word_matches(s->words[k], it);
    if (s->mode[k] == '+' && !m) return 0;
    if (s->mode[k] == '-' && m) return 0;
    if (s->mode[k] == '~') { any = 1; any_ok |= m; }
  }
  return !any || any_ok;
}

// ---------- copies from a Drive sync ----------

// "index (from Drive).csv" is a copy of index.csv (is_conflict_copy in lib.rs): the name starts with
// the base, then something that isn't a letter, digit or dot, and ends in .csv.
static int is_conflict_copy(const char *name, const char *base) {
  size_t n = strlen(name), b = strlen(base);
  if (n <= b + 4 || strncasecmp(name, base, b) || strcasecmp(name + n - 4, ".csv")) return 0;
  unsigned char c = (unsigned char)name[b];
  return !(isalnum(c) || c == '.' || c >= 0x80);
}
static Map map_new(int n) {
  size_t cap = 64;
  while (cap < (size_t)n * 2 + 2) cap *= 2;
  Map m = {calloc(cap, sizeof(char *)), calloc(cap, sizeof(char *)), cap};
  if (!m.keys || !m.vals) { free(m.keys); free(m.vals); m.keys = m.vals = NULL; m.cap = 0; lv_oom = 1; }
  return m;
}
static double num_of(const char *s) { return s && *s ? atof(s) : 0; }

// Our rows plus theirs that we haven't (by id, and by uid), in our columns. 1: changed.
static int merge_index(char **ours_text, const char *theirs_text, int *added) {
  LvCsv a, b;
  lv_csv_parse(*ours_text, &a);
  lv_csv_parse(theirs_text, &b);
  int changed = 0;
  if (a.nrows && b.nrows) {
    char **ha = a.rows[0], **hb = b.rows[0];
    int na = a.ncells[0], nb = b.ncells[0];
    int ida = col(ha, na, "id"), uida = col(ha, na, "uid"), idb = col(hb, nb, "id"), uidb = col(hb, nb, "uid");
    Map ids = map_new(a.nrows * 2);
    if (ida >= 0 && idb >= 0 && ids.cap) {
      for (int r = 1; r < a.nrows; r++) {
        const char *id = cell(a.rows[r], a.ncells[r], ida), *u = cell(a.rows[r], a.ncells[r], uida);
        if (*id) map_put(&ids, id, "");
        if (*u) { char k[80]; snprintf(k, sizeof k, "\x01%s", u); map_put(&ids, k, ""); }
      }
      int cap = a.nrows + b.nrows;
      char ***rows = malloc(cap * sizeof *rows);
      int *nc = malloc(cap * sizeof *nc);
      int *map = malloc((na + 1) * sizeof *map);
      int n = 0;
      if (rows && nc && map) {
        for (int k = 0; k < na; k++) map[k] = col(hb, nb, ha[k]);
        for (int r = 0; r < a.nrows; r++) { rows[n] = a.rows[r]; nc[n++] = a.ncells[r]; }
        int first_new = n;
        for (int r = 1; r < b.nrows; r++) {
          const char *id = cell(b.rows[r], b.ncells[r], idb), *u = cell(b.rows[r], b.ncells[r], uidb);
          char k[80];
          snprintf(k, sizeof k, "\x01%s", u);
          if (!*id || map_get(&ids, id) || (*u && map_get(&ids, k))) continue;
          char **row = malloc((na + 1) * sizeof *row);
          if (!row) { lv_oom = 1; break; }
          for (int c = 0; c < na; c++) row[c] = (char *)cell(b.rows[r], b.ncells[r], map[c]);
          rows[n] = row;
          nc[n++] = na;
          map_put(&ids, id, "");
          if (*u) map_put(&ids, k, "");
        }
        if (n > first_new) {
          char *t = lv_csv_text(rows, nc, n);
          if (t) { sfree(*ours_text); *ours_text = t; changed = 1; *added += n - first_new; }
        }
        for (int r = first_new; r < n; r++) free(rows[r]);
      }
      free(rows); free(nc); free(map);
    }
    if (ids.cap) map_free(&ids);
  }
  lv_csv_free(&a);
  lv_csv_free(&b);
  return changed;
}

// The counts: the larger of the two, the later times; their rows we haven't, added. 1: changed.
static int merge_views(char **ours_text, const char *theirs_text, int *counts) {
  LvCsv a, b;
  lv_csv_parse(*ours_text, &a);
  lv_csv_parse(theirs_text, &b);
  int changed = 0;
  if (a.nrows && b.nrows && a.ncells[0] >= 7 && b.ncells[0] >= 7) {
    Map at = map_new(a.nrows + b.nrows);
    int cap = a.nrows + b.nrows;
    char ***rows = malloc(cap * sizeof *rows);
    int *nc = malloc(cap * sizeof *nc);
    char **mine = malloc((size_t)cap * 7 * sizeof *mine); // (rows rewritten: row k at mine + 7k)
    char (*nums)[3][32] = malloc((size_t)cap * sizeof *nums);
    if (at.cap && rows && nc && mine && nums) {
      int n = 0;
      for (int r = 0; r < a.nrows; r++) {
        rows[n] = a.rows[r];
        nc[n] = a.ncells[r];
        if (r && a.ncells[r] >= 7 && *a.rows[r][0]) { char ix[16]; snprintf(ix, sizeof ix, "%d", n); map_put(&at, a.rows[r][0], ix); }
        n++;
      }
      for (int r = 1; r < b.nrows; r++) {
        if (b.ncells[r] < 7 || !*b.rows[r][0]) continue;
        char **t = b.rows[r];
        const char *ix = map_get(&at, t[0]);
        if (!ix) { // (a row only they have)
          char nx[16];
          snprintf(nx, sizeof nx, "%d", n);
          map_put(&at, t[0], nx);
          rows[n] = t;
          nc[n++] = 7;
          changed++;
          (*counts)++;
          continue;
        }
        int k = atoi(ix);
        if (nc[k] < 7) continue;
        char **o = rows[k];
        long long v0 = (long long)num_of(o[2]), l0 = (long long)num_of(o[4]), v1 = (long long)num_of(t[2]), l1 = (long long)num_of(t[4]);
        double s0 = num_of(o[6]), s1 = num_of(t[6]);
        int more = v1 > v0 || l1 > l0 || s1 > s0, later = strcmp(t[3], o[3]) > 0 || strcmp(t[5], o[5]) > 0;
        if (!more && !later) continue;
        char **row = mine + (size_t)k * 7;
        if (row != o) for (int c = 0; c < 7; c++) row[c] = o[c];
        if (more) {
          snprintf(nums[k][0], 32, "%lld", v1 > v0 ? v1 : v0);
          snprintf(nums[k][1], 32, "%lld", l1 > l0 ? l1 : l0);
          lv_num_or_empty(round((s1 > s0 ? s1 : s0) * 10.0) / 10.0, nums[k][2], 32);
          row[2] = nums[k][0];
          row[4] = nums[k][1];
          row[6] = nums[k][2];
          (*counts)++;
        }
        if (strcmp(t[3], row[3]) > 0) row[3] = t[3];
        if (strcmp(t[5], row[5]) > 0) row[5] = t[5];
        rows[k] = row;
        nc[k] = 7;
        changed++;
      }
      if (changed) {
        char *text = lv_csv_text(rows, nc, n);
        if (text) { sfree(*ours_text); *ours_text = text; }
        else changed = 0;
      }
    }
    free(rows); free(nc); free(mine); free(nums);
    if (at.cap) map_free(&at);
  }
  lv_csv_free(&a);
  lv_csv_free(&b);
  return changed != 0;
}

int lv_vault_merge_conflicts(LvVault *v, int *added, int *counts) {
  int a0 = 0, c0 = 0;
  if (!added) added = &a0;
  if (!counts) counts = &c0;
  DIR *d = opendir(v->root);
  if (!d) return -1;
  char names[16][256];
  int kinds[16], n = 0;
  struct dirent *e;
  while ((e = readdir(d)) && n < 16) {
    int k = is_conflict_copy(e->d_name, "index") ? 1 : is_conflict_copy(e->d_name, "views") ? 2 : 0;
    if (!k || strlen(e->d_name) >= sizeof names[0]) continue;
    snprintf(names[n], sizeof names[n], "%s", e->d_name);
    kinds[n++] = k;
  }
  closedir(d);
  if (!n) return 0;
  int done = 0;
  for (int k = 0; k < n; k++) {
    char p[900];
    snprintf(p, sizeof p, "%s/%s", v->root, names[k]);
    char *theirs = (char *)lv_read_all(p, lv_key(v), NULL);
    if (!theirs) continue; // (unreadable: tried again next time)
    const char *base = kinds[k] == 1 ? "index.csv" : "views.csv";
    char *ours = read_text(v, base);
    if (!ours) ours = sdup("");
    int changed = kinds[k] == 1 ? merge_index(&ours, theirs, added) : merge_views(&ours, theirs, counts);
    int ok = 1;
    if (changed) {
      backup_today(v);
      char to[700];
      snprintf(to, sizeof to, "%s/%s", v->root, base);
      ok = lv_write_all(to, lv_key(v), ours, strlen(ours)) == 0;
    }
    sfree(ours);
    free(theirs);
    if (!ok) continue;
    // the copy is kept, out of the way, as the PC keeps it
    char dir[700], to[1300];
    snprintf(dir, sizeof dir, "%s/_app/backups", v->root);
    mkdir(dir, 0777);
    snprintf(dir, sizeof dir, "%s/_app/backups/conflicts", v->root);
    mkdir(dir, 0777);
    time_t t = time(NULL);
    struct tm l;
    lv_local_tm(t, &l);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &l);
    snprintf(to, sizeof to, "%s/%s_%s", dir, stamp, names[k]);
    if (rename(p, to) != 0) remove(p);
    done++;
  }
  return done;
}
