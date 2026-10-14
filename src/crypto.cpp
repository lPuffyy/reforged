// Xbox kernel crypto exports (XcSHA*, XcHMAC) implemented with a small SHA-1. Contexts live in guest memory (XC_SHA_CONTEXT is 116 bytes).
#include "kernel.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define KFN(name, ...) extern "C" uint32_t __stdcall K_##name(__VA_ARGS__)

namespace {
struct Sha1 { uint32_t h[5]; uint64_t bits; uint8_t buf[64]; uint32_t used; };
static_assert(sizeof(Sha1) <= 116, "XC_SHA_CONTEXT is 116 bytes");

uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
void Block(Sha1* s, const uint8_t* p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
  for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; } else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; } else { f = b ^ c ^ d; k = 0xCA62C1D6; }
    uint32_t t = rol(a, 5) + f + e + k + w[i]; e = d; d = c; c = rol(b, 30); b = a; a = t;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
void Init(Sha1* s) { s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE; s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0; s->bits = 0; s->used = 0; }
void Update(Sha1* s, const uint8_t* d, uint32_t n) {
  s->bits += (uint64_t)n * 8;
  while (n) {
    uint32_t take = 64 - s->used; if (take > n) take = n;
    memcpy(s->buf + s->used, d, take); s->used += take; d += take; n -= take;
    if (s->used == 64) { Block(s, s->buf); s->used = 0; }
  }
}
void Final(Sha1* s, uint8_t* out) {
  uint64_t bits = s->bits; uint8_t pad = 0x80; Update(s, &pad, 1); pad = 0;
  while (s->used != 56) Update(s, &pad, 1);
  uint8_t len[8]; for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
  Update(s, len, 8);
  for (int i = 0; i < 5; i++) { out[i * 4] = (uint8_t)(s->h[i] >> 24); out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16); out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8); out[i * 4 + 3] = (uint8_t)s->h[i]; }
}
}  // namespace

KFN(XcSHAInit, uint8_t* ctx) { Init((Sha1*)ctx); return 0; }
KFN(XcSHAUpdate, uint8_t* ctx, const uint8_t* data, uint32_t len) { Update((Sha1*)ctx, data, len); return 0; }
KFN(XcSHAFinal, uint8_t* ctx, uint8_t* digest) { Final((Sha1*)ctx, digest); return 0; }
// XcHMAC(key, keyLen, in1, in1Len, in2, in2Len, digest): HMAC-SHA1 over in1 || in2
KFN(XcHMAC, const uint8_t* key, uint32_t keyLen, const uint8_t* in1, uint32_t len1, const uint8_t* in2, uint32_t len2, uint8_t* digest) {
  uint8_t k[64] = {}, ipad[64], opad[64], inner[20];
  if (keyLen > 64) keyLen = 64;  // the Xbox variant truncates
  memcpy(k, key, keyLen);
  for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
  Sha1 s; Init(&s); Update(&s, ipad, 64); if (in1 && len1) Update(&s, in1, len1); if (in2 && len2) Update(&s, in2, len2); Final(&s, inner);
  Init(&s); Update(&s, opad, 64); Update(&s, inner, 20); Final(&s, digest);
  if (getenv("XBOX_LOG_HMAC")) { static int n = 0; if (n++ < 30) { char kh[40] = "", dh[48] = ""; for (uint32_t i = 0; i < keyLen && i < 16; i++) sprintf(kh + i * 2, "%02x", key[i]); for (int i = 0; i < 20; i++) sprintf(dh + i * 2, "%02x", digest[i]); { char ih[48] = ""; for (uint32_t i = 0; i < len1 && i < 20; i++) sprintf(ih + i * 2, "%02x", in1[i]); Log("[crypto] XcHMAC in=%s", ih); } Log("[crypto] XcHMAC keyLen=%u key=%s len1=%u len2=%u -> %s", keyLen, kh, len1, len2, dh); if (len1 >= 400) { char path[64]; sprintf(path, "work/hmac_%d_in1.bin", n); FILE* f = fopen(path, "wb"); if (f) { fwrite(in1, 1, len1, f); if (in2 && len2) fwrite(in2, 1, len2, f); fclose(f); } } } }
  return 0;
}

// RC4: XcRC4Key(ctx, keyLen, key) / XcRC4Crypt(ctx, len, data) in place. The context is 258 bytes (i, j, S[256]).
KFN(XcRC4Key, uint8_t* ctx, uint32_t keyLen, const uint8_t* key) {
  uint8_t* S = ctx + 2; ctx[0] = 0; ctx[1] = 0;
  for (int i = 0; i < 256; i++) S[i] = (uint8_t)i;
  uint8_t j = 0;
  for (int i = 0; i < 256; i++) { j = (uint8_t)(j + S[i] + key[i % keyLen]); uint8_t t = S[i]; S[i] = S[j]; S[j] = t; }
  return 0;
}
KFN(XcRC4Crypt, uint8_t* ctx, uint32_t len, uint8_t* data) {
  uint8_t* S = ctx + 2; uint8_t i = ctx[0], j = ctx[1];
  for (uint32_t n = 0; n < len; n++) { i = (uint8_t)(i + 1); j = (uint8_t)(j + S[i]); uint8_t t = S[i]; S[i] = S[j]; S[j] = t; data[n] ^= S[(uint8_t)(S[i] + S[j])]; }
  ctx[0] = i; ctx[1] = j; return 0;
}

// XcDESKeyParity(key, len): set odd parity on every key byte
KFN(XcDESKeyParity, uint8_t* key, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) { uint8_t b = key[i] & 0xFE; int ones = 0; for (int k = 1; k < 8; k++) ones += (b >> k) & 1; key[i] = b | ((ones & 1) ? 0 : 1); }
  return 0;
}

// XcModExp(result, base, exponent, modulus, numWords): result = base^exponent mod modulus. All four are arrays of numWords little-endian 32-bit words (XNet's key exchange uses
// 768-bit numbers: 24 words). Plain square-and-multiply over a shift-and-add modular multiply: slow-ish (tens of ms) but only a few calls per connection.
namespace {
typedef std::vector<uint32_t> Big;
bool GeqBig(const Big& a, const Big& m) { for (size_t i = a.size(); i-- > 0;) { if (a[i] != m[i]) return a[i] > m[i]; } return true; }
void SubBig(Big& a, const Big& m) { uint64_t borrow = 0; for (size_t i = 0; i < a.size(); i++) { uint64_t d = (uint64_t)a[i] - m[i] - borrow; a[i] = (uint32_t)d; borrow = (d >> 63) & 1; } }
void AddModBig(Big& a, const Big& b, const Big& m) {   // a = (a + b) mod m, a and b < m
  uint64_t carry = 0; for (size_t i = 0; i < a.size(); i++) { uint64_t s = (uint64_t)a[i] + b[i] + carry; a[i] = (uint32_t)s; carry = s >> 32; }
  if (carry || GeqBig(a, m)) SubBig(a, m);
}
Big MulModBig(const Big& a, const Big& b, const Big& m) {
  Big r(a.size(), 0);
  for (size_t w = b.size(); w-- > 0;) for (int bit = 31; bit >= 0; bit--) {
    AddModBig(r, r, m);
    if ((b[w] >> bit) & 1) AddModBig(r, a, m);
  }
  return r;
}
}
KFN(XcModExp, uint32_t* result, const uint32_t* base, const uint32_t* exp, const uint32_t* mod, uint32_t n) {
  if (!result || !base || !exp || !mod || n == 0 || n > 128) return 0;
  Big m(mod, mod + n), b(base, base + n), r(n, 0); r[0] = 1;
  if (m == Big(n, 0)) return 0;
  while (GeqBig(b, m)) SubBig(b, m);          // reduce the base
  int top = (int)n * 32 - 1; while (top >= 0 && !((exp[top >> 5] >> (top & 31)) & 1)) top--;
  if (GeqBig(r, m)) SubBig(r, m);              // 1 mod m (m == 1 gives 0)
  for (int i = top; i >= 0; i--) { r = MulModBig(r, r, m); if ((exp[i >> 5] >> (i & 31)) & 1) r = MulModBig(r, b, m); }
  memcpy(result, r.data(), n * 4); return 1;
}
