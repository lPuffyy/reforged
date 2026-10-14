// DES / 3DES (EDE) block cipher and the Xbox kernel exports built on it: XcKeyTable, XcBlockCrypt, XcBlockCryptCBC (XNet's secure packet encryption uses them).
// Written from the public DES specification (FIPS 46-3 tables). The "key table" is our own layout: byte 0 = cipher (0 = DES, otherwise 3DES-EDE), bytes 8.. = the key
// bytes (8 or 24); schedules are recomputed per call, which is plenty fast for game traffic. Operation: 0 = decrypt, otherwise encrypt.
#include "kernel.h"
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define KFN(name, ...) extern "C" uint32_t PLAT_STDCALL K_##name(__VA_ARGS__)

namespace {
const uint8_t IP[64] = {58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,64,56,48,40,32,24,16,8,57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7};
const uint8_t FP[64] = {40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,37,5,45,13,53,21,61,29,36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25};
const uint8_t E[48] = {32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,16,17,18,19,20,21,20,21,22,23,24,25,24,25,26,27,28,29,28,29,30,31,32,1};
const uint8_t P[32] = {16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25};
const uint8_t PC1[56] = {57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,60,52,44,36,63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,29,21,13,5,28,20,12,4};
const uint8_t PC2[48] = {14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,41,52,31,37,47,55,30,40,51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32};
const uint8_t SHIFTS[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};
const uint8_t S[8][64] = {
 {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
 {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
 {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
 {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
 {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
 {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
 {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
 {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}};

inline int Bit(const uint8_t* b, int n) { return (b[(n - 1) >> 3] >> (7 - ((n - 1) & 7))) & 1; }       // 1-based, MSB first
inline void SetBit(uint8_t* b, int n, int v) { if (v) b[(n - 1) >> 3] |= (uint8_t)(0x80 >> ((n - 1) & 7)); else b[(n - 1) >> 3] &= (uint8_t)~(0x80 >> ((n - 1) & 7)); }

struct Sched { uint8_t k[16][6]; };
void MakeSched(const uint8_t* key8, Sched* s) {
  uint8_t cd[7] = {}; for (int i = 0; i < 56; i++) SetBit(cd, i + 1, Bit(key8, PC1[i]));
  int c[28], d[28]; for (int i = 0; i < 28; i++) { c[i] = Bit(cd, i + 1); d[i] = Bit(cd, i + 29); }
  for (int r = 0; r < 16; r++) {
    for (int sh = 0; sh < SHIFTS[r]; sh++) { int c0 = c[0], d0 = d[0]; for (int i = 0; i < 27; i++) { c[i] = c[i + 1]; d[i] = d[i + 1]; } c[27] = c0; d[27] = d0; }
    uint8_t merged[7] = {}; for (int i = 0; i < 28; i++) { SetBit(merged, i + 1, c[i]); SetBit(merged, i + 29, d[i]); }
    memset(s->k[r], 0, 6); for (int i = 0; i < 48; i++) SetBit(s->k[r], i + 1, Bit(merged, PC2[i]));
  }
}
void Des(const uint8_t* in, uint8_t* out, const Sched& s, bool decrypt) {
  uint8_t ip[8] = {}; for (int i = 0; i < 64; i++) SetBit(ip, i + 1, Bit(in, IP[i]));
  uint32_t l = (uint32_t)ip[0] << 24 | ip[1] << 16 | ip[2] << 8 | ip[3], r = (uint32_t)ip[4] << 24 | ip[5] << 16 | ip[6] << 8 | ip[7];
  for (int round = 0; round < 16; round++) {
    const uint8_t* k = s.k[decrypt ? 15 - round : round];
    uint8_t rb[4] = {(uint8_t)(r >> 24), (uint8_t)(r >> 16), (uint8_t)(r >> 8), (uint8_t)r}, ex[6] = {};
    for (int i = 0; i < 48; i++) SetBit(ex, i + 1, Bit(rb, E[i]));
    for (int i = 0; i < 6; i++) ex[i] ^= k[i];
    uint8_t so[4] = {};
    for (int b = 0; b < 8; b++) {
      int v = 0; for (int j = 0; j < 6; j++) v = (v << 1) | Bit(ex, b * 6 + j + 1);
      int row = ((v >> 4) & 2) | (v & 1), col = (v >> 1) & 15; int sv = S[b][row * 16 + col];
      for (int j = 0; j < 4; j++) SetBit(so, b * 4 + j + 1, (sv >> (3 - j)) & 1);
    }
    uint8_t pf[4] = {}; for (int i = 0; i < 32; i++) SetBit(pf, i + 1, Bit(so, P[i]));
    uint32_t f = (uint32_t)pf[0] << 24 | pf[1] << 16 | pf[2] << 8 | pf[3];
    uint32_t nl = r; r = l ^ f; l = nl;
  }
  uint8_t pre[8] = {(uint8_t)(r >> 24), (uint8_t)(r >> 16), (uint8_t)(r >> 8), (uint8_t)r, (uint8_t)(l >> 24), (uint8_t)(l >> 16), (uint8_t)(l >> 8), (uint8_t)l};   // swap halves
  memset(out, 0, 8); for (int i = 0; i < 64; i++) SetBit(out, i + 1, Bit(pre, FP[i]));
}
void Block3(const uint8_t* table, const uint8_t* in, uint8_t* out, bool encrypt) {
  uint8_t tmp[8]; Sched a, b, c;
  if (table[0] == 0) { MakeSched(table + 8, &a); Des(in, out, a, !encrypt); return; }
  MakeSched(table + 8, &a); MakeSched(table + 16, &b); MakeSched(table + 24, &c);
  if (encrypt) { Des(in, tmp, a, false); Des(tmp, tmp, b, true); Des(tmp, out, c, false); }
  else { Des(in, tmp, c, true); Des(tmp, tmp, b, false); Des(tmp, out, a, true); }
}
}  // namespace

// XcKeyTable(cipher, keytable, key)
KFN(XcKeyTable, uint32_t cipher, uint8_t* table, const uint8_t* key) {
  { static const bool lg = getenv("XBOX_LOG_DES") != nullptr; static int n = 0; if (lg && key && n++ < 30) { char k[60] = ""; for (int i = 0; i < 24; i++) sprintf(k + 2 * i, "%02x", key[i]); Log("[des] KeyTable cipher=%u key=%s", cipher, k); } }
  if (!table || !key) return 0; memset(table, 0, 32); table[0] = cipher ? 1 : 0; memcpy(table + 8, key, cipher ? 24 : 8); return 0;   // only 32 bytes: callers keep the real (0x80 / 0x180 byte) key tables on the stack
}
// XcBlockCrypt(cipher, out, in, keytable, operation): one 8-byte block
KFN(XcBlockCrypt, uint32_t cipher, uint8_t* out, const uint8_t* in, const uint8_t* table, uint32_t op) { Block3(table, in, out, op != 0); return 0; }
// XcBlockCryptCBC(cipher, length, out, in, keytable, operation, feedback): CBC, the feedback block (IV) is updated in place
KFN(XcBlockCryptCBC, uint32_t cipher, uint32_t len, uint8_t* out, const uint8_t* in, const uint8_t* table, uint32_t op, uint8_t* iv) {
  { static const bool lg = getenv("XBOX_LOG_DES") != nullptr; static int n = 0; if (lg && n++ < 60) { char k[40] = ""; for (int i = 0; i < 16; i++) sprintf(k + 2 * i, "%02x", table[8 + i]); Log("[des] CBC cipher=%u len=%u op=%u key=%s iv=%02x%02x%02x%02x%02x%02x%02x%02x in=%02x%02x%02x%02x%02x%02x%02x%02x", cipher, len, op, k, iv[0], iv[1], iv[2], iv[3], iv[4], iv[5], iv[6], iv[7], in[0], in[1], in[2], in[3], in[4], in[5], in[6], in[7]); } }
  bool enc = op != 0; uint8_t chain[8]; memcpy(chain, iv, 8);
  for (uint32_t off = 0; off + 8 <= len; off += 8) {
    uint8_t blk[8];
    if (enc) { for (int i = 0; i < 8; i++) blk[i] = in[off + i] ^ chain[i]; Block3(table, blk, out + off, true); memcpy(chain, out + off, 8); }
    else { uint8_t save[8]; memcpy(save, in + off, 8); Block3(table, in + off, blk, false); for (int i = 0; i < 8; i++) out[off + i] = blk[i] ^ chain[i]; memcpy(chain, save, 8); }
  }
  memcpy(iv, chain, 8); return 0;
}

// Self test (XBOX_CRYPTO_TEST=1): the classic DES example key 133457799BBCDFF1, plaintext 0123456789ABCDEF -> 85E813540F0AB405
void DesSelfTest() {
  uint8_t key[8] = {0x13,0x34,0x57,0x79,0x9B,0xBC,0xDF,0xF1}, pt[8] = {0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF}, ct[8], back[8], table[0x200];
  K_XcKeyTable(0, table, key); K_XcBlockCrypt(0, ct, pt, table, 1); K_XcBlockCrypt(0, back, ct, table, 0);
  Log("[crypto] DES self-test: ct=%02x%02x%02x%02x%02x%02x%02x%02x (expect 85e813540f0ab405) roundtrip=%s", ct[0], ct[1], ct[2], ct[3], ct[4], ct[5], ct[6], ct[7], memcmp(back, pt, 8) == 0 ? "ok" : "FAIL");
}
