#include "lvcrypt.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __vita__
#include <psp2/kernel/rng.h>
#endif

static const char MAGIC[4] = {'L', 'V', 'X', '1'};
static const char SALT[] = "lewd-vault/untraceable/v1";
#define ROUNDS 310000u
static const char CHECK[] = "lewd vault: the password is right";

// ---------- SHA-256 ----------
typedef struct { uint32_t h[8]; uint64_t n; uint8_t buf[64]; size_t k; } Sha;
static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t h[8], const uint8_t *p) {
  uint32_t w[64], a, b, c, d, e, f, g, hh;
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static void sha_init(Sha *s) {
  static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, H0, sizeof H0); s->n = 0; s->k = 0;
}
static void sha_add(Sha *s, const uint8_t *p, size_t n) {
  s->n += n;
  while (n) {
    size_t t = 64 - s->k < n ? 64 - s->k : n;
    memcpy(s->buf + s->k, p, t); s->k += t; p += t; n -= t;
    if (s->k == 64) { sha_block(s->h, s->buf); s->k = 0; }
  }
}
static void sha_end(Sha *s, uint8_t out[32]) {
  uint64_t bits = s->n * 8;
  uint8_t pad = 0x80, z = 0;
  sha_add(s, &pad, 1);
  while (s->k != 56) sha_add(s, &z, 1);
  uint8_t l[8];
  for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha_add(s, l, 8);
  for (int i = 0; i < 8; i++) { out[4 * i] = s->h[i] >> 24; out[4 * i + 1] = s->h[i] >> 16; out[4 * i + 2] = s->h[i] >> 8; out[4 * i + 3] = s->h[i]; }
}
void lv_sha256(const uint8_t *data, size_t n, uint8_t out[32]) { Sha s; sha_init(&s); sha_add(&s, data, n); sha_end(&s, out); }

// PBKDF2-HMAC-SHA256 for one 32-byte block. The inner and outer pad states are hashed once, so
// each round is two compressions (about two seconds on a Vita).
void lv_derive(const char *password, uint8_t key[32]) {
  uint8_t k[64] = {0}, ipad[64], opad[64];
  size_t pl = strlen(password);
  if (pl > 64) lv_sha256((const uint8_t *)password, pl, k); else memcpy(k, password, pl);
  for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
  Sha si, so;
  sha_init(&si); sha_add(&si, ipad, 64);
  sha_init(&so); sha_add(&so, opad, 64);
  // U1 = HMAC(salt || INT(1))
  Sha t = si;
  sha_add(&t, (const uint8_t *)SALT, sizeof SALT - 1);
  const uint8_t one[4] = {0, 0, 0, 1};
  sha_add(&t, one, 4);
  uint8_t u[32], acc[32];
  sha_end(&t, u);
  t = so; sha_add(&t, u, 32); sha_end(&t, u);
  memcpy(acc, u, 32);
  // later rounds: a 32-byte message after a 64-byte prefix, so one padded block each
  uint8_t blk[64];
  memset(blk, 0, 64);
  blk[32] = 0x80; blk[62] = 0x03; blk[63] = 0x00; // length (64 + 32) * 8 = 768 bits
  for (uint32_t r = 1; r < ROUNDS; r++) {
    uint32_t h[8];
    memcpy(blk, u, 32);
    memcpy(h, si.h, 32); sha_block(h, blk);
    for (int i = 0; i < 8; i++) { blk[4 * i] = h[i] >> 24; blk[4 * i + 1] = h[i] >> 16; blk[4 * i + 2] = h[i] >> 8; blk[4 * i + 3] = h[i]; }
    memcpy(h, so.h, 32); sha_block(h, blk);
    for (int i = 0; i < 8; i++) { u[4 * i] = h[i] >> 24; u[4 * i + 1] = h[i] >> 16; u[4 * i + 2] = h[i] >> 8; u[4 * i + 3] = h[i]; }
    for (int i = 0; i < 32; i++) acc[i] ^= u[i];
  }
  memcpy(key, acc, 32);
}

// ---------- ChaCha20 ----------
#define QR(a, b, c, d) a += b; d ^= a; d = ROL(d, 16); c += d; b ^= c; b = ROL(b, 12); a += b; d ^= a; d = ROL(d, 8); c += d; b ^= c; b = ROL(b, 7);
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void chacha_block(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t out[64]) {
  uint32_t s[16], x[16];
  s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
  for (int i = 0; i < 8; i++) s[4 + i] = le32(key + 4 * i);
  s[12] = counter; s[13] = le32(nonce); s[14] = le32(nonce + 4); s[15] = le32(nonce + 8);
  memcpy(x, s, sizeof s);
  for (int i = 0; i < 10; i++) {
    QR(x[0], x[4], x[8], x[12]) QR(x[1], x[5], x[9], x[13]) QR(x[2], x[6], x[10], x[14]) QR(x[3], x[7], x[11], x[15])
    QR(x[0], x[5], x[10], x[15]) QR(x[1], x[6], x[11], x[12]) QR(x[2], x[7], x[8], x[13]) QR(x[3], x[4], x[9], x[14])
  }
  for (int i = 0; i < 16; i++) { uint32_t v = x[i] + s[i]; out[4 * i] = v; out[4 * i + 1] = v >> 8; out[4 * i + 2] = v >> 16; out[4 * i + 3] = v >> 24; }
}
void lv_chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint64_t pos, uint8_t *restrict buf, size_t n) {
  uint8_t ks[64] __attribute__((aligned(32)));
  uint32_t counter = (uint32_t)(pos / 64);
  size_t skip = pos % 64;
  while (n) {
    chacha_block(key, nonce, counter++, ks);
    size_t t = 64 - skip < n ? 64 - skip : n;
    const uint8_t *restrict k = ks + skip;
    for (size_t i = 0; i < t; i++) buf[i] ^= k[i]; // (vectorised: NEON, 16 bytes at a time)
    buf += t; n -= t; skip = 0;
  }
}

int lv_check_ok(const uint8_t key[32], const uint8_t *bytes, size_t n) {
  size_t cl = sizeof CHECK - 1;
  if (n != 16 + cl || memcmp(bytes, MAGIC, 4) != 0) return 0;
  uint8_t body[64];
  memcpy(body, bytes + 16, cl);
  lv_chacha20_xor(key, bytes + 4, 0, body, cl);
  return memcmp(body, CHECK, cl) == 0;
}

void lv_random(uint8_t *out, size_t n) {
#ifdef __vita__
  // (the kernel's generator: at most 64 bytes a call)
  while (n) { size_t t = n > 64 ? 64 : n; sceKernelGetRandomNumber(out, t); out += t; n -= t; }
#else
  FILE *f = fopen("/dev/urandom", "rb");
  if (f && fread(out, 1, n, f) == n) { fclose(f); return; }
  if (f) fclose(f);
  srand((unsigned)time(NULL));
  for (size_t i = 0; i < n; i++) out[i] = (uint8_t)rand();
#endif
}

// ---------- files ----------
int lv_open(LvFile *lf, const char *path, const uint8_t *key) {
  memset(lf, 0, sizeof *lf);
  lf->f = fopen(path, "rb");
  if (!lf->f) return -1;
  fseek(lf->f, 0, SEEK_END);
  long size = ftell(lf->f);
  fseek(lf->f, 0, SEEK_SET);
  uint8_t h[16];
  if (size >= 16 && fread(h, 1, 16, lf->f) == 16 && memcmp(h, MAGIC, 4) == 0) {
    if (!key) { fclose(lf->f); lf->f = NULL; return -2; } // encrypted, and no password given
    lf->enc = 1;
    memcpy(lf->nonce, h + 4, 12);
    memcpy(lf->key, key, 32);
    lf->len = (uint64_t)size - 16;
  } else {
    lf->len = (uint64_t)size;
  }
  return 0;
}
size_t lv_read_at(LvFile *lf, uint64_t off, void *buf, size_t n) {
  if (!lf->f || off >= lf->len) return 0;
  if (off + n > lf->len) n = (size_t)(lf->len - off);
  if (fseek(lf->f, (long)(off + (lf->enc ? 16 : 0)), SEEK_SET) != 0) return 0;
  size_t got = fread(buf, 1, n, lf->f);
  if (lf->enc) lv_chacha20_xor(lf->key, lf->nonce, off, buf, got);
  return got;
}
void lv_close(LvFile *lf) { if (lf->f) fclose(lf->f); lf->f = NULL; }

uint8_t *lv_read_all(const char *path, const uint8_t *key, size_t *n) {
  LvFile lf;
  if (lv_open(&lf, path, key) != 0) return NULL;
  uint8_t *b = malloc((size_t)lf.len + 1);
  if (!b) { lv_close(&lf); return NULL; }
  size_t got = lv_read_at(&lf, 0, b, (size_t)lf.len);
  lv_close(&lf);
  if (got != lf.len) { free(b); return NULL; }
  b[got] = 0;
  if (n) *n = got;
  return b;
}

int lv_write_all(const char *path, const uint8_t *key, const void *data, size_t n) {
  char tmp[1024];
  snprintf(tmp, sizeof tmp, "%s.part", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return -1;
  int ok = 1;
  if (key) {
    uint8_t nonce[12];
    lv_random(nonce, 12);
    ok &= fwrite(MAGIC, 1, 4, f) == 4 && fwrite(nonce, 1, 12, f) == 12;
    uint8_t buf[16384];
    for (size_t at = 0; ok && at < n; at += sizeof buf) {
      size_t t = n - at < sizeof buf ? n - at : sizeof buf;
      memcpy(buf, (const uint8_t *)data + at, t);
      lv_chacha20_xor(key, nonce, at, buf, t);
      ok &= fwrite(buf, 1, t, f) == t;
    }
  } else {
    ok &= fwrite(data, 1, n, f) == n;
  }
  ok &= fclose(f) == 0;
  if (!ok) { remove(tmp); return -1; }
  // (the Vita's rename won't replace a file: the old one steps aside first, and comes back if the
  // new one can't take its place)
  char old[1040];
  snprintf(old, sizeof old, "%s.old", path);
  remove(old);
  int had = rename(path, old) == 0;
  if (rename(tmp, path) != 0) { if (had) rename(old, path); remove(tmp); return -1; }
  if (had) remove(old);
  return 0;
}
