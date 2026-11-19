// The Browser's site readers against answers saved from the real sites (tests/booru/*.json, from
// each kind's all-ages site), and the JSON reader on its own.
#include "../src/booru.h"
#include "../src/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  *n = l;
  return b;
}

// (an e621-kind site, as one added by hand would be: the kind is still read)
static LvSite E6 = {"e621", BK_E621, "https://e621.net", NULL, 1, 0, NULL, NULL, NULL};
static const LvSite *site(const char *name) {
  if (!strcmp(name, "e621")) return &E6;
  for (int k = 0; k < LV_NSITES; k++) if (!strcmp(LV_SITES[k].name, name)) return &LV_SITES[k];
  return NULL;
}

static void check_kind(const char *file, const LvSite *s, int want) {
  char p[256];
  snprintf(p, sizeof p, "tests/booru/%s.json", file);
  size_t n = 0;
  char *t = slurp(p, &n);
  CHECK(t, "%s missing", p);
  if (!t) return;
  int count = 0;
  LvPost *posts = lv_booru_parse(s, t, n, LV_R_ALL, &count);
  CHECK(posts && count == want, "%s: %d posts, not %d", file, count, want);
  for (int i = 0; i < count; i++) {
    LvPost *q = &posts[i];
    CHECK(q->id > 0, "%s: id", file);
    CHECK(!strncmp(q->preview, "https://", 8) && !strncmp(q->sample, "https://", 8) && !strncmp(q->file, "https://", 8), "%s: urls %s | %s | %s", file, q->preview, q->sample, q->file);
    CHECK(q->ext[0], "%s: ext", file);
    CHECK(q->tags[0], "%s: tags", file);
    CHECK(q->rating & LV_R_ALL, "%s: rating", file);
  }
  printf("%-9s %d posts, first: %lld %s %dx%d score %d\n  preview %s\n  sample  %s\n", file, count, posts[0].id, posts[0].ext, posts[0].width, posts[0].height, posts[0].score, posts[0].preview, posts[0].sample);
  // only the ratings asked for
  int none = 0;
  LvPost *x = lv_booru_parse(s, t, n, LV_R_EXPLICIT, &none);
  CHECK(none == 0, "%s: an explicit-only filter on all-ages posts keeps %d", file, none);
  lv_posts_free(x, none);
  lv_posts_free(posts, count);
  free(t);
}

int main(void) {
  // HentaiCity: eight minutes and longer, its counter links undone, each episode's tags and MP4
  {
    LvSite hy = {"HentaiCity", BK_HCITY, "https://www.hentaicity.com", NULL, 1, 0, NULL, NULL, NULL, 1};
    char u[600];
    lv_booru_url_ex(&hy, "", LV_R_ALL, LV_SORT_HOT, 0, NULL, 2, u, sizeof u);
    CHECK(!strcmp(u, "https://www.hentaicity.com/videos/straight/all-popular-2.html?min_duration=480"), "hcity url %s", u);
    size_t n;
    char *t = slurp("tests/booru/hcity_list.html", &n);
    int c = 0;
    LvPost *p = lv_booru_parse(&hy, t, n, LV_R_ALL, &c);
    CHECK(p && c >= 20, "hcity cards %d", c);
    for (int k = 0; p && k < c; k++) CHECK(!strncmp(p[k].page, "https://www.hentaicity.com/video/", 33) && !strstr(p[k].page, "click") && strstr(p[k].preview, "main.jpg") && p[k].duration >= 480, "hcity card %d %s %d", k, p[k].page, p[k].duration);
    lv_posts_free(p, c);
    free(t);
    t = slurp("tests/booru/hcity_episode.html", &n);
    LvPost q = {0};
    q.file = strdup("x");
    CHECK(lv_item_check(&hy, &q, t) == 1 && !strcmp(q.file, "https://www.hentaicity.com/flv/0807/38331/480p.mp4"), "hcity mp4 %s", q.file);
    char fb[600];
    CHECK(lv_video_fallback(q.file, fb, sizeof fb) == 0 && !strcmp(fb, "https://www.hentaicity.com/flv/0807/38331/mobile.mp4"), "hcity fallback %s", fb);
    char *bad = malloc(n + 100);
    snprintf(bad, n + 100, "<a href=\"https://www.hentaicity.com/tags/video/loli\">x</a>%s", t);
    CHECK(lv_item_check(&hy, &q, bad) == 0, "hcity never-shown tag");
    free(bad); free(q.file); free(t);
  }
  // the episode sites: HentaiWorld and HentaiCloud (their lists, then each episode's page)
  {
    LvSite hw = {"HentaiWorld", BK_HWORLD, "https://hentaiworld.tv", NULL, 1, 0, NULL, NULL, NULL, 1};
    LvSite hc = {"HentaiCloud", BK_HCLOUD, "https://www.hentaicloud.com", NULL, 1, 0, NULL, NULL, NULL, 1};
    CHECK(lv_site_checks_items(&hw) && lv_site_checks_items(&hc) && !lv_site_is_booru(&hw), "episode sites check each item");
    char u[600];
    lv_booru_url_ex(&hw, "", LV_R_ALL, LV_SORT_HOT, 0, NULL, 2, u, sizeof u);
    CHECK(!strcmp(u, "https://hentaiworld.tv/trending?page=2"), "hw url %s", u);
    lv_booru_url_ex(&hc, "", LV_R_ALL, LV_SORT_LATEST, 0, NULL, 3, u, sizeof u);
    CHECK(!strcmp(u, "https://www.hentaicloud.com/videos?o=mr&page=3"), "hc url %s", u);
    size_t n;
    char *t = slurp("tests/booru/hworld_list.html", &n);
    int c = 0;
    LvPost *p = lv_booru_parse(&hw, t, n, LV_R_ALL, &c);
    CHECK(p && c >= 20, "hw cards %d", c);
    for (int k = 0; p && k < c; k++) CHECK(strstr(p[k].preview, "cover-268.webp") && !strncmp(p[k].page, "https://hentaiworld.tv/videos/", 30) && p[k].duration > 60 && lv_is_long(&hw, &p[k]), "hw card %d %s %d", k, p[k].preview, p[k].duration);
    lv_posts_free(p, c);
    free(t);
    // (the site's newer cards, 2026-11: the title on the cover's alt, more after the duration's class)
    t = slurp("tests/booru/hworld_list2.html", &n);
    c = 0;
    p = lv_booru_parse(&hw, t, n, LV_R_ALL, &c);
    CHECK(p && c >= 8, "hw2 cards %d", c);
    for (int k = 0; p && k < c; k++) CHECK(strstr(p[k].preview, "cover-268.webp") && !strncmp(p[k].page, "https://hentaiworld.tv/videos/", 30) && p[k].duration > 60, "hw2 card %d %s %d", k, p[k].preview, p[k].duration);
    lv_posts_free(p, c);
    free(t);
    t = slurp("tests/booru/hworld_episode.html", &n);
    LvPost q = {0};
    q.file = strdup("x");
    CHECK(lv_item_check(&hw, &q, t) == 1 && strstr(q.file, "cdn.hentaiworld.tv/") && strstr(q.file, "video.mp4"), "hw episode mp4 %s", q.file);
    free(q.file);
    free(t);
    t = slurp("tests/booru/hcloud_list.html", &n);
    p = lv_booru_parse(&hc, t, n, LV_R_ALL, &c);
    CHECK(p && c >= 20, "hc cards %d", c);
    for (int k = 0; p && k < c; k++) CHECK(strstr(p[k].file, "/media/videos/hd/") && strstr(p[k].preview, "/media/videos/tmb/") && p[k].duration >= 60, "hc card %d %s %d", k, p[k].file, p[k].duration);
    lv_posts_free(p, c);
    free(t);
    t = slurp("tests/booru/hcloud_episode.html", &n);
    q.file = strdup("keep");
    CHECK(lv_item_check(&hc, &q, t) == 1 && !strcmp(q.file, "keep"), "hc episode ok");
    // (a category of the never-shown words: not shown)
    char *bad = malloc(n + 200);
    const char *cat = strstr(t, "class=\"category\"");
    snprintf(bad, n + 200, "%.*s<a href=\"/videos/x\">Loli</a>%s", (int)(strstr(cat, "<ul>") + 4 - t), t, strstr(cat, "<ul>") + 4);
    CHECK(lv_item_check(&hc, &q, bad) == 0, "hc never-shown category");
    free(bad); free(q.file); free(t);
  }
  // AI-made: by tag (the boorus) or by title (the video sites)
  CHECK(lv_ai_made("1girl ai-generated solo") && lv_ai_made("cum by snu snu island ai generated") && lv_ai_made("x stable diffusion y"), "ai tags and titles");
  CHECK(!lv_ai_made("1girl aiko solo") && !lv_ai_made("air guitar") && !lv_ai_made("ai"), "not ai: other words");
  // xvideos (its hentai, ten minutes and longer): the search page and a video page
  {
    LvSite xv = {"XVideos Hentai", BK_XV, "https://www.xvideos.com", NULL, 1, 0, NULL, NULL, "hentai", 1};
    char u[600];
    lv_booru_url_ex(&xv, "succubus", LV_R_ALL, LV_SORT_LATEST, 0, NULL, 2, u, sizeof u);
    CHECK(!strcmp(u, "https://www.xvideos.com/?k=hentai+succubus&durf=10min_more&sort=uploaddate&p=1"), "xv url %s", u);
    size_t n;
    char *t = slurp("tests/booru/xv_list.html", &n);
    int c = 0;
    LvPost *p = lv_booru_parse(&xv, t, n, LV_R_ALL, &c);
    CHECK(p && c >= 15, "xv cards %d", c);
    int longs = 0;
    for (int k = 0; p && k < c; k++) {
      CHECK(!strncmp(p[k].page, "https://www.xvideos.com/video.", 30) && strstr(p[k].preview, ".jpg") && lv_post_is_video(&p[k]), "xv card %d %s", k, p[k].page);
      longs += p[k].duration >= 600 && lv_is_long(&xv, &p[k]);
    }
    CHECK(longs >= c / 2, "xv durations: %d of %d long", longs, c);
    lv_posts_free(p, c);
    free(t);
    t = slurp("tests/booru/xv_video.html", &n);
    char mp4[1024];
    int nev = 0;
    CHECK(lv_xv_video(t, mp4, sizeof mp4, &nev) == 0 && strstr(mp4, "mp4_sd.mp4") && !nev, "xv mp4 %s", mp4);
    free(t);
    // (Rule34Video's longest: sorted by length)
    LvSite kv = {"Rule34Video", BK_KVS, "https://rule34video.com", NULL, 1, 0, NULL, NULL, "hentai", 1};
    lv_booru_url_ex(&kv, "longest", LV_R_ALL, LV_SORT_LATEST, 0, NULL, 1, u, sizeof u);
    CHECK(strstr(u, "/tags/hentai/") && strstr(u, "sort_by=duration"), "kvs longest %s", u);
  }
  // the image sites: e-shuushuu (its API, a tag by its number) and booru.io
  {
    LvSite es = {"E-Shuushuu", BK_ESHUU, "https://e-shuushuu.net", NULL, 1, 0, NULL, NULL};
    LvSite bi = {"booru.io", BK_BOORUIO, "https://booru.io", NULL, 0, 0, NULL, NULL};
    CHECK(!lv_site_is_booru(&es) && !lv_site_is_booru(&bi), "image sites aren't boorus");
    char u[600];
    lv_booru_url_ex(&es, "#169 smile", LV_R_ALL, LV_SORT_TOP, 0, NULL, 2, u, sizeof u);
    CHECK(!strcmp(u, "https://e-shuushuu.net/api/v1/images?page=2&per_page=40&sort_by=favorites&tags=169"), "eshuu url %s", u);
    lv_booru_url_ex(&bi, "blonde_hair", LV_R_ALL, LV_SORT_LATEST, 0, NULL, 1, u, sizeof u);
    CHECK(!strcmp(u, "https://booru.io/api/legacy/query/entity?query=blonde_hair"), "booruio url %s", u);
    lv_booru_url_ex(&bi, "blonde_hair", LV_R_ALL, LV_SORT_LATEST, 0, NULL, 2, u, sizeof u);
    CHECK(!u[0], "booru.io has one page");
    size_t n;
    char *t = slurp("tests/booru/eshuushuu_tags.json", &n);
    char id[16] = "";
    CHECK(lv_esh_tag_id(t, n, "blonde_hair", id, sizeof id) == 0 && !strcmp(id, "#169"), "eshuu tag id %s", id);
    free(t);
    t = slurp("tests/booru/eshuushuu.json", &n);
    int c = 0;
    LvPost *p = lv_booru_parse(&es, t, n, LV_R_ALL, &c);
    CHECK(p && c >= 30, "eshuu posts %d", c);
    for (int k = 0; p && k < c; k++) {
      CHECK(strstr(p[k].preview, "cdn.e-shuushuu.net/thumbs/") && p[k].width > 0 && p[k].tags && p[k].id > 0, "eshuu post %d", k);
      CHECK(!strchr(p[k].tags, '#') && !strstr(p[k].tags, "  "), "eshuu tags %d: %s", k, p[k].tags);
    }
    lv_posts_free(p, c);
    free(t);
    t = slurp("tests/booru/booruio.json", &n);
    p = lv_booru_parse(&bi, t, n, LV_R_ALL, &c);
    CHECK(p && c >= 40, "booruio posts %d", c);
    for (int k = 0; p && k < c; k++) {
      CHECK(strstr(p[k].preview, "/api/legacy/data/") && strstr(p[k].preview, "300x.jpg") && p[k].tags[0] && !strchr(p[k].tags, '#'), "booruio post %d: %s", k, p[k].preview);
    }
    lv_posts_free(p, c);
    // (only general: the explicit ones left out by their tags)
    p = lv_booru_parse(&bi, t, n, LV_R_GENERAL, &c);
    int gen = c;
    lv_posts_free(p, c);
    CHECK(gen < 40, "booruio general only: %d", gen);
    free(t);
  }
  // a KVS video site (rule34video.com): its list page's cards, and a video page's MP4
  {
    LvSite kv = {"Rule34Video", BK_KVS, "https://rule34video.com", NULL, 1, 0, NULL, NULL, "hentai", 1};
    char u[600];
    lv_booru_url_ex(&kv, "", LV_R_ALL, LV_SORT_LATEST, 0, NULL, 2, u, sizeof u);
    CHECK(!strcmp(u, "https://rule34video.com/tags/hentai/?mode=async&function=get_block&block_id=custom_list_videos_common_videos&sort_by=post_date&from=2"), "kvs url %s", u);
    lv_booru_url_ex(&kv, "big_breasts", LV_R_ALL, LV_SORT_TOP, LV_SPAN_MONTH, NULL, 1, u, sizeof u);
    CHECK(strstr(u, "/tags/big-breasts/") && strstr(u, "sort_by=rating_month"), "kvs search %s", u);
    size_t n;
    char *html = slurp("tests/booru/kvs_list.html", &n);
    int c = 0;
    LvPost *p = lv_booru_parse(&kv, html, n, LV_R_ALL, &c);
    CHECK(p && c >= 15, "kvs cards: %d", c);
    for (int k = 0; p && k < c; k++) {
      CHECK(lv_post_is_video(&p[k]) && !strncmp(p[k].file, "https://rule34video.com/video/", 30), "card %d page %s", k, p[k].file);
      CHECK(!strncmp(p[k].preview, "https://", 8) && strstr(p[k].preview, ".jpg"), "card %d preview %s", k, p[k].preview);
      CHECK(p[k].tags[0] && p[k].id > 0, "card %d tags", k);
    }
    lv_posts_free(p, c);
    free(html);
    html = slurp("tests/booru/kvs_video.html", &n);
    char mp4[1024];
    int nev = 0;
    CHECK(lv_kvs_video(html, mp4, sizeof mp4, &nev) == 0 && strstr(mp4, "_480p.mp4") && !nev, "kvs mp4 (480p) %s", mp4);
    // a page tagged with one of the never-shown words: no video
    char *bad = malloc(n + 64);
    snprintf(bad, n + 64, "<a href=\"/tags/loli-elf/\">x</a>%s", html);
    CHECK(lv_kvs_video(bad, mp4, sizeof mp4, &nev) != 0 && nev, "never-shown tags block the video");
    free(bad);
    free(html);
  }
  // working out what kind a site added by hand is, from its answers
  {
    size_t n1, n2, n3;
    char *e6 = slurp("tests/booru/e621.json", &n1), *dan = slurp("tests/booru/danbooru.json", &n2), *moe = slurp("tests/booru/moebooru.json", &n3);
    CHECK(lv_booru_probe(BK_E621, e6, n1) && !lv_booru_probe(BK_DANBOORU, e6, n1), "e621's answer is e621's kind only");
    CHECK(lv_booru_probe(BK_DANBOORU, dan, n2) && !lv_booru_probe(BK_E621, dan, n2), "danbooru's answer is danbooru's kind");
    CHECK(lv_booru_probe(BK_MOEBOORU, moe, n3), "moebooru's");
    CHECK(!lv_booru_probe(BK_GELBOORU, "<html>", 6), "a web page is no kind");
    free(e6); free(dan); free(moe);
    char u[256];
    lv_booru_probe_url(BK_GELBOORU, "https://example.org", u, sizeof u);
    CHECK(!strcmp(u, "https://example.org/index.php?page=dapi&s=post&q=index&json=1&limit=1"), "probe url: %s", u);
    int before = LV_NSITES;
    CHECK(lv_booru_add_site("My booru", BK_MOEBOORU, "https://my.example/") == 0 && LV_NSITES == before + 1, "added");
    CHECK(!strcmp(LV_SITES[before].base, "https://my.example") && !strcmp(LV_SITES[before].name, "My booru"), "its address, trimmed");
    lv_booru_url(&LV_SITES[before], "x", LV_R_ALL, LV_SORT_LATEST, 1, u, sizeof u);
    CHECK(!strncmp(u, "https://my.example/post.json?", 29), "its searches: %s", u);
    LV_NSITES = before;
  }
  // Top's spans and an account's key
  {
    char u[1024];
    lv_booru_url_ex(site("Danbooru"), "blue_hair", LV_R_ALL, LV_SORT_TOP, LV_SPAN_WEEK, NULL, 1, u, sizeof u);
    CHECK(strstr(u, "order%3Ascore+age%3A%3C1w"), "danbooru's week: %s", u);
    lv_booru_url_ex(site("e621"), "", LV_R_ALL, LV_SORT_TOP, LV_SPAN_DAY, NULL, 1, u, sizeof u);
    CHECK(strstr(u, "date%3Aday"), "e621's day: %s", u);
    lv_booru_url_ex(site("Yande.re"), "", LV_R_ALL, LV_SORT_TOP, LV_SPAN_MONTH, NULL, 1, u, sizeof u);
    CHECK(strstr(u, "date%3A%3E%3D20"), "moebooru's month: %s", u);
    lv_booru_url_ex(site("Danbooru"), "x", LV_R_ALL, LV_SORT_LATEST, LV_SPAN_WEEK, NULL, 1, u, sizeof u);
    CHECK(!strstr(u, "age%3A"), "a span only for Top: %s", u);
    lv_booru_url_ex(site("Gelbooru"), "x", LV_R_ALL, LV_SORT_LATEST, 0, "12345 abc+def", 2, u, sizeof u);
    CHECK(strstr(u, "https://gelbooru.com/index.php?page=dapi") && strstr(u, "&pid=1") && strstr(u, "&user_id=12345&api_key=abc%2Bdef"), "gelbooru's key: %s", u);
    CHECK(site("Rule34") && site("Rule34")->needs_key && !site("Danbooru")->needs_key, "which need a key");
    // a site that's a slice of another: its preset tags first
    lv_booru_url_ex(site("Danbooru Comics"), "blue_hair", LV_R_ALL, LV_SORT_LATEST, 0, NULL, 1, u, sizeof u);
    CHECK(strstr(u, "danbooru.donmai.us/posts.json") && strstr(u, "tags=comic+blue_hair"), "danbooru comics: %s", u);
    lv_booru_url_ex(site("Xbooru Comics"), "", LV_R_ALL, LV_SORT_LATEST, 0, NULL, 1, u, sizeof u);
    CHECK(strstr(u, "xbooru.com") && strstr(u, "tags=comic"), "xbooru comics: %s", u);
  }
  // the JSON reader
  const char *js = "{\"a\":[1,2,{\"b\":\"x\\u00e9\\\"y\"}],\"c\":-3.5,\"d\":true,\"e\":null,\"f\":\"\\ud83d\\ude00\"}";
  Json j;
  CHECK(json_parse(&j, js, strlen(js)) == 0, "parse");
  char b[64];
  CHECK(json_gn(&j, 0, "c") == -3.5, "number");
  CHECK(json_gn(&j, 0, "d") == 1, "true");
  int a = json_get(&j, 0, "a");
  CHECK(j.t[a].size == 3 && json_num(&j, json_at(&j, a, 1)) == 2, "array");
  CHECK(!strcmp(json_gs(&j, json_at(&j, a, 2), "b", b, sizeof b), "x\xc3\xa9\"y"), "escapes: %s", b);
  CHECK(!strcmp(json_gs(&j, 0, "f", b, sizeof b), "\xf0\x9f\x98\x80"), "surrogates");
  CHECK(json_get(&j, 0, "zz") < 0 && !json_gs(&j, 0, "e", b, sizeof b)[0], "missing and null");
  json_free(&j);
  CHECK(json_parse(&j, "[1,2", 4) != 0, "truncated is an error");
  CHECK(json_parse(&j, "<!DOCTYPE html>", 15) != 0, "a web page is an error");

  check_kind("danbooru", site("Danbooru"), 5);
  check_kind("moebooru", site("Konachan"), 5);
  // (four of these five are tagged "young": never shown)
  check_kind("e621", site("e621"), 1);
  check_kind("gelbooru", site("Safebooru"), 5);
  check_kind("sakugabooru", site("Sakugabooru"), 5);

  // searches: what's never searched for is dropped; ratings and orders become the site's words
  char q[256], url[2048];
  CHECK(!strcmp(lv_booru_clean("blue_hair Loli -shota smile ~cub", q, sizeof q), "blue_hair smile"), "clean: %s", q);
  lv_booru_url(site("Danbooru"), "blue_hair loli", LV_R_GENERAL | LV_R_SENSITIVE, LV_SORT_TOP, 2, url, sizeof url);
  CHECK(!strcmp(url, "https://danbooru.donmai.us/posts.json?limit=40&page=2&tags=blue_hair+rating%3Ag%2Cs+order%3Ascore"), "danbooru url: %s", url);
  lv_booru_url(site("Yande.re"), "", LV_R_GENERAL | LV_R_SENSITIVE, LV_SORT_RANDOM, 1, url, sizeof url);
  CHECK(!strcmp(url, "https://yande.re/post.json?limit=40&page=1&tags=-rating%3Aq+-rating%3Ae+order%3Arandom"), "moebooru url: %s", url);
  lv_booru_url(site("Xbooru"), "a b", LV_R_ALL, LV_SORT_TOP, 3, url, sizeof url);
  CHECK(!strcmp(url, "https://xbooru.com/index.php?page=dapi&s=post&q=index&json=1&limit=40&pid=2&tags=a+b+sort%3Ascore"), "gelbooru url: %s", url);
  lv_booru_url(site("e621"), "", LV_R_EXPLICIT, LV_SORT_HOT, 1, url, sizeof url);
  CHECK(!strcmp(url, "https://e621.net/posts.json?limit=40&page=1&tags=-rating%3As+-rating%3Aq+order%3Ahot"), "e621 url: %s", url);

  printf(fails ? "%d FAILED\n" : "booru: all passed\n", fails);
  return fails != 0;
}
