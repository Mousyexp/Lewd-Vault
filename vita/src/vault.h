// A Lewd Vault vault on the Vita: index.csv and views.csv read the way src-tauri/src/lib.rs reads
// them, files found where the PC and the phones put them, and views.csv written back byte for byte
// in the same format (UTF-8 with a BOM, CRLF, RFC 4180 quoting, numbers as JavaScript prints them).
// Portable C: tests/core_test.c runs it on a PC against src-tauri/tests/fixtures.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char *id, *uid, *file, *name, *category, *kind, *added, *preview_id, *thumb, *collection, *tags, *source;
  int nsfw, quality, width, height;
  double duration;
  char *path; // its file (NULL: not in the vault folder)
  int view;   // its row in views (-1: none yet)
  char **cells; // every cell of its row, in index.csv's column order (the strings above are these)
  int ncells;
} LvItem;

// What sorting and filtering read for every item, kept apart from the strings (the guide's hot/cold
// split): 16 bytes, two to a Cortex-A9 cache line, so a sort of 10,000 items stays in cache.
typedef struct {
  int64_t added;   // "2026-10-16T09:00:00Z" as a number (yyyymmddhhmmss), for sorting
  int16_t cat;     // its category's place in cats
  uint8_t kind;    // LV_IMAGE, LV_GIF, LV_VIDEO
  uint8_t quality;
  int32_t view;    // = items[i].view
} LvHot;
enum { LV_IMAGE, LV_GIF, LV_VIDEO };

typedef struct {
  char *id, *uid, *last, *last_like;
  long long views, likes;
  double seconds;
} LvView;

typedef struct {
  char root[512];
  int untraceable; // _app/key.check is there: every file is encrypted
  int has_key;
  uint8_t key[32];
  int discreet; // every file in Media/ (Settings → Storage → Discreet names)
  char theme[32];
  LvItem *items;
  LvHot *hot; // (one per item, the same order)
  int n;
  struct LvArena *arena; // (the items' strings, freed at once)
  LvView *views;
  int nviews, capviews;
  int views_dirty;
  char **head; // index.csv's columns, as the file has them (a rewrite keeps every one)
  int nhead;
  int index_dirty;
  char **cats; // the categories, most items first
  int *cat_counts;
  int ncats;
} LvVault;

int lv_is_vault(const char *dir);
int lv_is_untraceable(const char *dir);
// 1: the password is right (the key is kept), 0: wrong, -1: no check file.
int lv_vault_unlock(LvVault *v, const char *password);
void lv_vault_init(LvVault *v, const char *root);
// 0 on success; *why gets a message otherwise.
int lv_vault_load(LvVault *v, const char **why);
void lv_vault_free(LvVault *v);

const uint8_t *lv_key(const LvVault *v);
// The thumbnail's bytes (WebP, malloc'd), or NULL.
uint8_t *lv_thumb_bytes(LvVault *v, const LvItem *it, size_t *n);
// The short preview clip (_app/clips) for videos and GIFs: 1 and the path, or 0.
int lv_clip_path(LvVault *v, const LvItem *it, char *out, size_t cap);

// Views, as the PC records them: "now" is an ISO time like 2026-10-20T12:00:00.000Z.
void lv_viewed(LvVault *v, LvItem *it, const char *now);
void lv_liked(LvVault *v, LvItem *it, const char *now);
void lv_add_seconds(LvVault *v, LvItem *it, double secs);
long long lv_views_of(const LvVault *v, const LvItem *it);
long long lv_likes_of(const LvVault *v, const LvItem *it);
int lv_vault_save_views(LvVault *v);

// A new item from the Browser: its file and thumbnail written into the vault (encrypted for an
// Untraceable one), its row appended to index.csv (not the whole file rewritten), and the item added
// to the loaded vault. 0 on success; *why says what went wrong.
typedef struct {
  const char *name, *category, *kind, *tags, *source, *added; // kind: image, gif or video
  const char *from; // (with no data: a file to copy in, piece by piece)
  int nsfw, quality, width, height;
  double duration;
} LvNew;
// The same in two steps, for a thread that does the slow part: the files and the row (reads the
// loaded items only for the next number), then the loaded vault (on the thread that draws it).
typedef struct { char uid[16], name[400], path[1300], thumb[128], tsize[32]; } LvAdded;
int lv_vault_add_files(LvVault *v, const LvNew *n, const uint8_t *data, size_t len, const char *ext,
                       const uint8_t *thumb, size_t tlen, int tw, int th, LvAdded *out, const char **why);
int lv_vault_add_memory(LvVault *v, const LvNew *n, const LvAdded *a);
int lv_vault_add(LvVault *v, const LvNew *n, const uint8_t *data, size_t len, const char *ext,
                 const uint8_t *thumb, size_t tlen, int tw, int th, const char **why);

// Editing (index.csv is then written whole, after the day's backup).
// A column's value for an item (nsfw, quality, category, tags, collection, ...): 0, or -1 when the
// file has no such column.
int lv_item_set(LvVault *v, int i, const char *column, const char *value);
int lv_vault_save_index(LvVault *v);
// The day's backup of the vault at root (index.csv, views.csv, settings.json), for a writer thread.
void lv_backup_today_root(const char *root);
// The item's file (and its own thumbnail and clip) into the vault's _trash folder, as the PC does
// for an Untraceable vault, and its row out of index.csv (saved). 0 on success.
int lv_vault_delete(LvVault *v, int i, int save); // (save 0: several in a row, then lv_vault_save_index)
char *lv_index_csv(const LvVault *v); // (malloc'd)

// After a sync with Google Drive: "index (from Drive).csv" and "views (from Drive).csv" folded in, as
// the PC's merge_conflicts does (their rows we haven't; the larger counts and later times), then kept
// in _app/backups/conflicts. Before the vault is loaded. Returns the copies handled (-1: no folder).
int lv_vault_merge_conflicts(LvVault *v, int *added, int *counts);

// Searching, as the PC does: blue_hair (has it), -sketch (hasn't), ~cat ~dog (any of them),
// artist:* or blue* (a wildcard at the end). A word matches a tag exactly, or (with no * and no
// prefix) is found in the name, category or collection.
typedef struct { char words[24][64]; char mode[24]; int n; } LvSearch; // mode: '+', '-' or '~'
void lv_search_compile(const char *query, LvSearch *s);
int lv_search_match(const LvSearch *s, const LvItem *it);

// The CSV pieces (exposed for the tests).
typedef struct { char ***rows; int *ncells; int nrows; } LvCsv;
void lv_csv_parse(const char *text, LvCsv *out);
void lv_csv_free(LvCsv *c);
char *lv_csv_text(char ***rows, const int *ncells, int nrows); // malloc'd
char *lv_views_csv(const LvVault *v);
#include <time.h>
void lv_local_tm(time_t t, struct tm *out); // (localtime_r, or UTC where the library can't tell)
extern int lv_oom; // set when an allocation failed (a load then fails cleanly)
int lv_b64url_decode(const char *in, char *out, size_t cap);
void lv_b64url_encode(const char *in, char *out, size_t cap);
void lv_num_or_empty(double f, char *out, size_t cap);
