// The Browser's sites: boorus read through their JSON APIs (the same four kinds the desktop app's
// Discover knows, ui/discover.js), so no web page is ever loaded. Portable C: tests/booru_test.c
// runs it on a PC against answers saved from the real sites.
#pragma once
#include <stddef.h>

enum { BK_DANBOORU, BK_GELBOORU, BK_MOEBOORU, BK_E621, BK_KVS };
// (BK_KVS: video sites made with Kernel Video Sharing, like rule34video.com: their list pages are read
//  as HTML; a post's file is its video page, whose player names the MP4: lv_kvs_video)
typedef struct {
  const char *name;
  int kind;
  const char *base;
  const char *page_base; // (posts' own pages, when not on base)
  int hot;               // has a "Hot" order
  int needs_key;         // only answers with an account's API key (user ID and key)
  const char *icon;      // its tile in the site switcher: app0:sites/<icon>.png
  const char *about;     // a few words on what it has
  const char *preset;    // tags every search on it carries (a site that's a slice of another)
  int hidden;            // kept (saved settings point at sites by their place) but not offered
} LvSite;
// The built-in sites, then the ones added on the Vita (lv_booru_add_site), up to LV_MAX_SITES.
#define LV_MAX_SITES 24
extern LvSite LV_SITES[LV_MAX_SITES];
extern int LV_NSITES;
extern const int LV_NBUILTIN;
// A site added by hand: 0, or -1 when there's no room. name and base are copied.
int lv_booru_add_site(const char *name, int kind, const char *base);
// What kind of site answers at base, from a page of its answer to each kind's search (the caller
// fetches them: url_for(kind) gives the address to try). -1: none of them.
void lv_booru_probe_url(int kind, const char *base, char *out, size_t cap);
int lv_booru_probe(int kind, const char *json, size_t len);

enum { LV_SORT_LATEST, LV_SORT_HOT, LV_SORT_TOP, LV_SORT_RANDOM, LV_NSORTS };
extern const char *LV_SORT_NAMES[LV_NSORTS];
// ratings: a bit each, general, sensitive, questionable, explicit
enum { LV_R_GENERAL = 1, LV_R_SENSITIVE = 2, LV_R_QUESTIONABLE = 4, LV_R_EXPLICIT = 8, LV_R_ALL = 15 };

typedef struct {
  long long id;
  char *preview, *sample, *file, *page, *source, *tags; // (malloc'd)
  char ext[8];
  int width, height, score;
  int rating; // one of LV_R_*
} LvPost;

#define LV_PAGE 40
// The search's address. tags: what was typed (cleaned of what's never searched for).
void lv_booru_url(const LvSite *s, const char *tags, int ratings, int sort, int page, char *out, size_t cap);
// The same, with Top's span (LV_SPAN_*: the site's best of the day, week or month, where it can) and
// an account's key ("<user id> <api key>", or NULL).
enum { LV_SPAN_ALL, LV_SPAN_DAY, LV_SPAN_WEEK, LV_SPAN_MONTH, LV_NSPANS };
extern const char *LV_SPAN_NAMES[LV_NSPANS];
int lv_booru_has_spans(const LvSite *s);
void lv_booru_url_ex(const LvSite *s, const char *tags, int ratings, int sort, int span, const char *key, int page, char *out, size_t cap);
// The posts in a page of the site's answer (malloc'd array, *n of them); NULL with *n = 0 when it
// isn't one. Posts tagged as sexual content involving minors are left out, and so are those whose
// rating isn't in `ratings` (some sites ignore the rating in the search).
LvPost *lv_booru_parse(const LvSite *s, const char *json, size_t len, int ratings, int *n);
void lv_posts_free(LvPost *p, int n);
// Cleans a search: drops the tags that are never searched for. Returns out.
char *lv_booru_clean(const char *query, char *out, size_t cap);
int lv_post_is_video(const LvPost *p);
// A KVS video page: the address of its MP4 (the smallest, the first it names), and 1
// in *never when its tags carry one of the never-shown words. 0, or -1 when there's no video.
int lv_kvs_video(const char *html, char *out, size_t cap, int *never);
int lv_booru_never_tags(const char *tags); // (1: one of the never-shown words, as a whole tag)
