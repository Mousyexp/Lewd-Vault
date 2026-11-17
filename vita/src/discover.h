// Finding more sites: the lists other booru tools keep (gallery-dl's supported sites, the "booru"
// library's sites.json, Grabber's per-engine site lists), read for the boorus of the kinds the
// Browser speaks (Danbooru, Gelbooru 0.2, Moebooru), merged, and the ones never offered left out
// (sites for sexual content involving minors, furry and real-people boorus). Portable C:
// tests/discover_test.c runs it against saved copies of the lists.
#pragma once
#include <stddef.h>

typedef struct {
  char name[48], base[160]; // base: https://host (no slash at the end)
  int kind;                 // BK_DANBOORU, BK_GELBOORU or BK_MOEBOORU
} LvCand;

#define LV_INDEX_GALLERYDL "https://raw.githubusercontent.com/mikf/gallery-dl/master/docs/supportedsites.md"
#define LV_INDEX_BOORUJS "https://raw.githubusercontent.com/AtoraSuunva/booru/master/src/sites.json"
#define LV_INDEX_GRABBER "https://raw.githubusercontent.com/Bionus/imgbrd-grabber/master/src/sites/"
// Grabber's lists: LV_INDEX_GRABBER + one of these + "/sites.txt", each of one kind
extern const char *const LV_GRABBER_DIRS[3];
extern const int LV_GRABBER_KINDS[3];

// Each adds what it finds to out (from *n, up to cap), skipping what's there already. Return the
// number added.
int lv_disc_gallerydl(const char *md, size_t len, LvCand *out, int *n, int cap);
int lv_disc_boorujs(const char *json, size_t len, LvCand *out, int *n, int cap);
int lv_disc_lines(const char *txt, size_t len, int kind, LvCand *out, int *n, int cap);
// 1: never offered (its name or address carries one of the words left out)
int lv_disc_never(const char *name, const char *base);
// 1 when two addresses are the same site (the same last two parts of the host: danbooru.donmai.us
// and sonohara.donmai.us)
int lv_disc_same_site(const char *a, const char *b);
