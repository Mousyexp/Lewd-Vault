// Untraceable vaults on the Vita: the same format as src-tauri/src/crypt.rs, written again in C.
// A file is "LVX1", a 12-byte nonce, then the bytes XORed with ChaCha20 (RFC 8439, counter from 0).
// The key is PBKDF2-HMAC-SHA256(password, "lewd-vault/untraceable/v1", 310000). A plain file (no
// "LVX1") reads as it is. Portable C: tests/core_test.c checks it against the golden vaults.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

void lv_sha256(const uint8_t *data, size_t n, uint8_t out[32]);
void lv_derive(const char *password, uint8_t key[32]);
void lv_chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint64_t pos, uint8_t *restrict buf, size_t n);
int lv_check_ok(const uint8_t key[32], const uint8_t *bytes, size_t n);
void lv_random(uint8_t *out, size_t n);

// A file opened for reading: decrypted as read, any byte range (videos seek, thumbnails are slices).
typedef struct {
  FILE *f;
  int enc;
  uint8_t key[32], nonce[12];
  uint64_t len; // the plain length
} LvFile;

// key: NULL for a normal vault. 0 on success.
int lv_open(LvFile *lf, const char *path, const uint8_t *key);
size_t lv_read_at(LvFile *lf, uint64_t off, void *buf, size_t n);
void lv_close(LvFile *lf);
// The whole file, decrypted (malloc'd, NUL-terminated; *n without the NUL). NULL if it can't.
uint8_t *lv_read_all(const char *path, const uint8_t *key, size_t *n);
// Written next to it, then moved over it; encrypted with a fresh nonce when key is set. 0 on success.
int lv_write_all(const char *path, const uint8_t *key, const void *data, size_t n);
