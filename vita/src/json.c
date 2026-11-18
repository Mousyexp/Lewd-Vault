#include "json.h"
#include <stdlib.h>
#include <string.h>

static int ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

typedef struct { const char *s; size_t len, at; Json *j; int cap; } P;

static int push(P *p, int type, int start) {
  if (p->j->n == p->cap) {
    p->cap = p->cap ? p->cap * 2 : 256;
    JTok *t = realloc(p->j->t, p->cap * sizeof *t);
    if (!t) return -1;
    p->j->t = t;
  }
  JTok *t = &p->j->t[p->j->n];
  t->type = (unsigned char)type; t->start = start; t->end = start; t->size = 0; t->next = 0;
  return p->j->n++;
}

static int value(P *p, int depth);
static void skip(P *p) { while (p->at < p->len && ws(p->s[p->at])) p->at++; }

static int string(P *p) {
  size_t i = ++p->at; // (after the quote)
  while (i < p->len && p->s[i] != '"') i += p->s[i] == '\\' ? 2 : 1;
  if (i >= p->len) return -1;
  int k = push(p, J_STR, (int)p->at);
  if (k < 0) return -1;
  p->j->t[k].end = (int)i;
  p->at = i + 1;
  p->j->t[k].next = p->j->n;
  return k;
}

static int value(P *p, int depth) {
  if (depth > 64) return -1;
  skip(p);
  if (p->at >= p->len) return -1;
  char c = p->s[p->at];
  if (c == '"') return string(p);
  if (c == '{' || c == '[') {
    int k = push(p, c == '{' ? J_OBJ : J_ARR, (int)p->at);
    if (k < 0) return -1;
    char close = c == '{' ? '}' : ']';
    p->at++;
    int size = 0;
    for (;;) {
      skip(p);
      if (p->at >= p->len) return -1;
      if (p->s[p->at] == close) { p->at++; break; }
      if (size) {
        if (p->s[p->at] != ',') return -1;
        p->at++;
        skip(p);
      }
      if (c == '{') {
        if (p->at >= p->len || p->s[p->at] != '"' || string(p) < 0) return -1;
        skip(p);
        if (p->at >= p->len || p->s[p->at] != ':') return -1;
        p->at++;
      }
      if (value(p, depth + 1) < 0) return -1;
      size++;
    }
    p->j->t[k].size = size;
    p->j->t[k].end = (int)p->at;
    p->j->t[k].next = p->j->n;
    return k;
  }
  // a number, true, false or null: up to the next delimiter
  size_t e = p->at;
  while (e < p->len && !ws(p->s[e]) && p->s[e] != ',' && p->s[e] != '}' && p->s[e] != ']') e++;
  int type = c == 't' ? J_TRUE : c == 'f' ? J_FALSE : c == 'n' ? J_NULL : J_NUM;
  // (only real JSON: an error page from a site isn't read as a value)
  size_t len = e - p->at;
  const char *w = p->s + p->at;
  if (type == J_TRUE && !(len == 4 && !memcmp(w, "true", 4))) return -1;
  if (type == J_FALSE && !(len == 5 && !memcmp(w, "false", 5))) return -1;
  if (type == J_NULL && !(len == 4 && !memcmp(w, "null", 4))) return -1;
  if (type == J_NUM) {
    if (!(c == '-' || (c >= '0' && c <= '9'))) return -1;
    for (size_t i = 0; i < len; i++) if (!strchr("0123456789+-.eE", w[i])) return -1;
  }
  int k = push(p, type, (int)p->at);
  if (k < 0) return -1;
  p->j->t[k].end = (int)e;
  p->j->t[k].next = p->j->n;
  p->at = e;
  return k;
}

int json_parse(Json *j, const char *text, size_t len) {
  memset(j, 0, sizeof *j);
  j->text = text;
  P p = {text, len, 0, j, 0};
  // (room for the tokens guessed from its size, about one per 12 bytes of a booru's answer: a few
  //  doublings and copies of a large array saved)
  int guess = (int)(len / 12) + 64;
  if (guess > 256) { j->t = malloc(guess * sizeof *j->t); if (j->t) p.cap = guess; }
  if (value(&p, 0) < 0) { json_free(j); return -1; }
  skip(&p);
  if (p.at != len) { json_free(j); return -1; }
  return 0;
}
void json_free(Json *j) { free(j->t); j->t = NULL; j->n = 0; }

static int key_is(const Json *j, int k, const char *key) {
  const JTok *t = &j->t[k];
  size_t n = strlen(key);
  return t->type == J_STR && (size_t)(t->end - t->start) == n && !memcmp(j->text + t->start, key, n);
}

int json_get(const Json *j, int obj, const char *key) {
  if (obj < 0 || obj >= j->n || j->t[obj].type != J_OBJ) return -1;
  int k = obj + 1;
  for (int i = 0; i < j->t[obj].size; i++) {
    int v = k + 1; // (the value after its key)
    if (key_is(j, k, key)) return v;
    k = j->t[v].next;
  }
  return -1;
}

int json_at(const Json *j, int arr, int idx) {
  if (arr < 0 || arr >= j->n || (j->t[arr].type != J_ARR && j->t[arr].type != J_OBJ) || idx >= j->t[arr].size) return -1;
  int k = arr + 1;
  for (int i = 0; i < idx; i++) k = j->t[j->t[arr].type == J_OBJ ? k + 1 : k].next;
  return j->t[arr].type == J_OBJ ? k + 1 : k;
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

const char *json_str(const Json *j, int tok, char *out, size_t cap) {
  if (!cap) return out;
  out[0] = 0;
  if (tok < 0 || tok >= j->n) return out;
  const JTok *t = &j->t[tok];
  if (t->type == J_OBJ || t->type == J_ARR || t->type == J_NULL) return out;
  size_t o = 0;
  for (int i = t->start; i < t->end && o + 4 < cap; i++) {
    char c = j->text[i];
    if (c != '\\' || t->type != J_STR) { out[o++] = c; continue; }
    c = j->text[++i];
    switch (c) {
      case 'n': out[o++] = '\n'; break;
      case 't': out[o++] = '\t'; break;
      case 'r': out[o++] = '\r'; break;
      case 'b': case 'f': break;
      case 'u': {
        unsigned u = 0;
        for (int h = 0; h < 4 && i + 1 < t->end; h++) { int v = hexv(j->text[++i]); u = u * 16 + (v < 0 ? 0 : v); }
        // (a surrogate pair: the low half follows)
        if (u >= 0xD800 && u < 0xDC00 && i + 6 < t->end && j->text[i + 1] == '\\' && j->text[i + 2] == 'u') {
          unsigned lo = 0;
          for (int h = 0; h < 4; h++) { int v = hexv(j->text[i + 3 + h]); lo = lo * 16 + (v < 0 ? 0 : v); }
          if (lo >= 0xDC00 && lo < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
        }
        if (u < 0x80) out[o++] = (char)u;
        else if (u < 0x800) { out[o++] = 0xC0 | u >> 6; out[o++] = 0x80 | (u & 0x3F); }
        else if (u < 0x10000) { out[o++] = 0xE0 | u >> 12; out[o++] = 0x80 | ((u >> 6) & 0x3F); out[o++] = 0x80 | (u & 0x3F); }
        else { out[o++] = 0xF0 | u >> 18; out[o++] = 0x80 | ((u >> 12) & 0x3F); out[o++] = 0x80 | ((u >> 6) & 0x3F); out[o++] = 0x80 | (u & 0x3F); }
        break;
      }
      default: out[o++] = c;
    }
  }
  out[o] = 0;
  return out;
}

double json_num(const Json *j, int tok) {
  if (tok < 0 || tok >= j->n) return 0;
  const JTok *t = &j->t[tok];
  if (t->type == J_TRUE) return 1;
  if (t->type != J_NUM && t->type != J_STR) return 0;
  char b[40];
  int n = t->end - t->start;
  if (n > 39) n = 39;
  memcpy(b, j->text + t->start, n);
  b[n] = 0;
  return strtod(b, NULL);
}

const char *json_gs(const Json *j, int obj, const char *key, char *out, size_t cap) { return json_str(j, json_get(j, obj, key), out, cap); }
double json_gn(const Json *j, int obj, const char *key) { return json_num(j, json_get(j, obj, key)); }
