// The Browser's sites: boorus read through their JSON APIs (the same four kinds the desktop app's
// Discover knows, ui/discover.js), so no web page is ever loaded. Portable C: tests/booru_test.c
// runs it on a PC against answers saved from the real sites.
#pragma once
#include <stddef.h>

enum { BK_DANBOORU, BK_GELBOORU, BK_MOEBOORU, BK_E621 };
typedef struct {
  const char *name;
  int kind;
  const char *base;
  const char *page_base; // (posts' own pages, when not on base)
  int hot;               // has a "Hot" order
} LvSite;
extern const LvSite LV_SITES[];
extern const int LV_NSITES;

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
// The posts in a page of the site's answer (malloc'd array, *n of them); NULL with *n = 0 when it
// isn't one. Posts tagged as sexual content involving minors are left out, and so are those whose
// rating isn't in `ratings` (some sites ignore the rating in the search).
LvPost *lv_booru_parse(const LvSite *s, const char *json, size_t len, int ratings, int *n);
void lv_posts_free(LvPost *p, int n);
// Cleans a search: drops the tags that are never searched for. Returns out.
char *lv_booru_clean(const char *query, char *out, size_t cap);
int lv_post_is_video(const LvPost *p);
