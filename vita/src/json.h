// A small JSON reader for the boorus' answers: one pass over the text makes a flat list of tokens
// (no allocation per value), then values are looked up by key where needed.
#pragma once
#include <stddef.h>

enum { J_NONE, J_OBJ, J_ARR, J_STR, J_NUM, J_TRUE, J_FALSE, J_NULL };
typedef struct {
  unsigned char type;
  int start, end; // the value's text (a string's without its quotes)
  int size;       // an object's keys or an array's items
  int next;       // the token after this value and everything in it
} JTok;
typedef struct {
  const char *text;
  JTok *t;
  int n;
} Json;

// 0 on success (free with json_free).
int json_parse(Json *j, const char *text, size_t len);
void json_free(Json *j);
// The value under `key` in the object at token `obj`, or -1.
int json_get(const Json *j, int obj, const char *key);
// The k-th item of the array (or k-th value of the object) at token `arr`, or -1.
int json_at(const Json *j, int arr, int k);
// A string's text, unescaped into out (cap bytes); numbers and booleans as written. "" if missing.
const char *json_str(const Json *j, int tok, char *out, size_t cap);
double json_num(const Json *j, int tok);
// Shortcuts: json_str / json_num of json_get.
const char *json_gs(const Json *j, int obj, const char *key, char *out, size_t cap);
double json_gn(const Json *j, int obj, const char *key);
