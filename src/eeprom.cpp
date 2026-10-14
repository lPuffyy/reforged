// Console identity: a persistent 256-byte EEPROM image (work/eeprom.bin, XBOX_EEPROM overrides the path) generated on first run with our own random
// HDD key, online key, serial number and MAC (Xbox OUI 00:50:F2). Layout (public hardware documentation): hash[20] confounder[8] hdd_key[16] region[4]
// checksum[4] serial[12] mac[6] pad[2] online_key[16] video_standard[4] pad[4] user_checksum[4] user_section[156].
// ExQueryNonVolatileSetting serves it: user settings 0..0x12 as DWORDs, 0x100 serial, 0x101 MAC, 0x102 online key, 0x103 video standard, 0xFFFF the whole image.
#define _CRT_RAND_S
#include "kernel.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace {
uint8_t g_ee[256]; bool g_init = false;   // saved user settings (REG_DWORD) live in the user section: dword per index at 0x64 + 4 * index, "is saved" bitmap in the last dword (0xFC)
std::string EePath() { const char* p = getenv("XBOX_EEPROM"); return p && *p ? p : "work/eeprom.bin"; }
void Rand(uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) { unsigned v = 0; rand_s(&v); p[i] = (uint8_t)(v >> 8); } }

// ---- EEPROM security-section encryption (public description of the retail EEPROM format; same scheme every Xbox kernel uses) ----
// The "key" is a pair of precomputed SHA-1 states (inner/outer HMAC pads) per kernel generation; hash = H_outer(H_inner(data)), RC4 key = hash of hash.
struct Sha1S { uint32_t h[5]; };
uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
void Sha1Block(uint32_t* h, const uint8_t* b) {
  uint32_t w[80]; for (int i = 0; i < 16; i++) w[i] = (b[4*i] << 24) | (b[4*i+1] << 16) | (b[4*i+2] << 8) | b[4*i+3];
  for (int i = 16; i < 80; i++) w[i] = Rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
  uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; i++) { uint32_t f, k; if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999; } else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1; } else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; } else { f = bb ^ c ^ d; k = 0xCA62C1D6; }
    uint32_t t = Rol(a, 5) + f + e + k + w[i]; e = d; d = c; c = Rol(bb, 30); bb = a; a = t; }
  h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
}
// SHA-1 continuing from state `st` that already consumed 64 bytes
void Sha1From(const uint32_t* st, const uint8_t* data, size_t len, uint8_t* out20) {
  uint32_t h[5]; memcpy(h, st, 20); std::vector<uint8_t> m(data, data + len); m.push_back(0x80); while (m.size() % 64 != 56) m.push_back(0);
  uint64_t bits = (uint64_t)(len + 64) * 8; for (int i = 7; i >= 0; i--) m.push_back((uint8_t)(bits >> (8 * i)));
  for (size_t i = 0; i < m.size(); i += 64) Sha1Block(h, &m[i]);
  for (int i = 0; i < 5; i++) { out20[4*i] = h[i] >> 24; out20[4*i+1] = h[i] >> 16; out20[4*i+2] = h[i] >> 8; out20[4*i+3] = h[i]; }
}
struct KeyStates { uint32_t inner[5], outer[5]; };
KeyStates StatesFor(int ver) {   // HMAC-SHA1 pad states for the EEPROM key of each kernel generation (0 = 1.0, 2 = 1.1-1.4, 3 = 1.6; the keys are the ones every title's XcHMAC call passes)
  static const uint8_t keys[3][16] = {{0x2a,0x3b,0xad,0x2c,0xb1,0x94,0x4f,0x93,0xaa,0xcd,0xcd,0x7e,0x0a,0xc2,0xee,0x5a}, {0x1d,0xf3,0x5c,0x83,0x8e,0xc9,0xb6,0xfc,0xbd,0xf6,0x61,0xab,0x4f,0x06,0x33,0xe4}, {0x2b,0x84,0x57,0xbe,0x9b,0x1e,0x65,0xc6,0xcd,0x9d,0x2b,0xce,0xc1,0xa2,0x09,0x61}};
  const uint8_t* k = keys[ver == 2 ? 1 : ver == 3 ? 2 : 0]; uint8_t ip[64], op[64]; for (int i = 0; i < 64; i++) { uint8_t kb = i < 16 ? k[i] : 0; ip[i] = kb ^ 0x36; op[i] = kb ^ 0x5c; }
  KeyStates st; static const uint32_t iv[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  memcpy(st.inner, iv, 20); Sha1Block(st.inner, ip); memcpy(st.outer, iv, 20); Sha1Block(st.outer, op); return st;
}
void XHash(int ver, const uint8_t* data, size_t len, uint8_t* out20) { KeyStates k = StatesFor(ver); uint8_t in[20]; Sha1From(k.inner, data, len, in); Sha1From(k.outer, in, 20, out20); }
void Rc4(const uint8_t* key, size_t kl, uint8_t* data, size_t n) {
  uint8_t S[256]; for (int i = 0; i < 256; i++) S[i] = (uint8_t)i; int j = 0; for (int i = 0; i < 256; i++) { j = (j + S[i] + key[i % kl]) & 255; std::swap(S[i], S[j]); }
  int a = 0, b = 0; for (size_t k = 0; k < n; k++) { a = (a + 1) & 255; b = (b + S[a]) & 255; std::swap(S[a], S[b]); data[k] ^= S[(S[a] + S[b]) & 255]; }
}
uint32_t EeCrc(const uint8_t* d, size_t len) { uint32_t high = 0, low = 0; for (size_t i = 0; i < len / 4; i++) { uint32_t v = ((const uint32_t*)d)[i]; uint64_t sum = ((uint64_t)high << 32) | low; high = (uint32_t)((sum + v) >> 32); low += v; } return ~(high + low); }
uint8_t g_hdd[16];   // plaintext HDD key (what the kernel exports as XboxHDKey)
bool Decrypt() {
  for (int ver : {2, 3, 0}) { uint8_t seed[20], plain[0x1C], chk[20]; XHash(ver, g_ee, 20, seed); { char h[48] = ""; for (int i = 0; i < 20; i++) sprintf(h + 2 * i, "%02x", seed[i]); if (getenv("XBOX_LOG_HMAC")) Log("[eeprom] ver %d seed %s", ver, h); } memcpy(plain, g_ee + 0x14, 0x1C); Rc4(seed, 20, plain, 0x1C); XHash(ver, plain, 0x1C, chk); if (!memcmp(chk, g_ee, 20)) { memcpy(g_hdd, plain + 8, 16); return true; } }
  return false;
}
void Generate() {
  memset(g_ee, 0, sizeof g_ee);
  Rand(g_ee + 0x14, 8); Rand(g_ee + 0x1C, 16); *(uint32_t*)(g_ee + 0x2C) = 1;                      // confounder, HDD key, region 1 (NTSC-U)
  for (int i = 0; i < 12; i++) { unsigned v; rand_s(&v); g_ee[0x34 + i] = (uint8_t)('0' + v % 10); }  // serial number: 12 digits
  g_ee[0x40] = 0x00; g_ee[0x41] = 0x50; g_ee[0x42] = 0xF2; Rand(g_ee + 0x43, 3);                      // MAC: Xbox OUI + random tail
  Rand(g_ee + 0x48, 16);                                                                            // online key
  *(uint32_t*)(g_ee + 0x58) = 0x00400100;                                                           // video standard NTSC-M
  memcpy(g_hdd, g_ee + 0x1C, 16);   // plaintext before encryption
  // checksums, then encrypt the security section (confounder + HDD key + region) the way the kernel does
  *(uint32_t*)(g_ee + 0x30) = EeCrc(g_ee + 0x34, 0x2C);
  int ver = getenv("XBOX_EEPROM_VER") ? atoi(getenv("XBOX_EEPROM_VER")) : 2;
  uint8_t seed[20]; XHash(ver, g_ee + 0x14, 0x1C, g_ee); XHash(ver, g_ee, 20, seed); Rc4(seed, 20, g_ee + 0x14, 0x1C);
}
}  // namespace

void EepromInit() {
  if (g_init) return; g_init = true; std::string path = EePath();
  if (FILE* f = fopen(path.c_str(), "rb")) { size_t n = fread(g_ee, 1, sizeof g_ee, f); fclose(f); if (n == sizeof g_ee) { if (!Decrypt()) { Log("[eeprom] %s does not decrypt: regenerating", path.c_str()); goto regen; } Log("[eeprom] loaded %s", path.c_str()); return; } }
regen:
  Generate();
  if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(g_ee, 1, sizeof g_ee, f); fclose(f); Log("[eeprom] generated a new console identity -> %s (serial %.12s)", path.c_str(), g_ee + 0x34); }
}
const uint8_t* EepromImage() { EepromInit(); return g_ee; }
const uint8_t* EepromHddKey() { EepromInit(); return g_hdd; }

// returns an NTSTATUS
uint32_t EepromQuery(uint32_t index, uint32_t* type, void* value, uint32_t len, uint32_t* resultLen) {
  EepromInit(); const uint8_t* src = nullptr; uint32_t n = 0, ty = 3; uint32_t dw = 0;
  switch (index) {
    case 0x100: src = g_ee + 0x34; n = 12; break;
    case 0x101: src = g_ee + 0x40; n = 6; break;
    case 0x102: src = g_ee + 0x48; n = 16; break;
    case 0x103: src = g_ee + 0x58; n = 4; ty = 4; break;
    case 0xFFFF: src = g_ee; n = 256; break;
    case 0xFFFE: src = g_ee; n = 48; break;   // the encrypted security section: hash, confounder, HDD key, region
    default:
      if (index <= 0x12) { ty = 4; n = 4; bool saved = (*(uint32_t*)(g_ee + 0xFC) >> index) & 1; dw = saved ? *(uint32_t*)(g_ee + 0x64 + 4 * index) : index == 7 ? 1 : 0; src = (const uint8_t*)&dw; break; }   // 7 = language English
      return 0xC0000225;   // STATUS_NOT_FOUND
  }
  if (type) *type = ty; if (resultLen) *resultLen = n;
  if (len < n) return 0xC0000023;   // STATUS_BUFFER_TOO_SMALL
  if (value) memcpy(value, src, n); return 0;
}
void EepromSave(uint32_t index, uint32_t value) {
  EepromInit(); if (index > 0x12) return;
  *(uint32_t*)(g_ee + 0x64 + 4 * index) = value; *(uint32_t*)(g_ee + 0xFC) |= 1u << index;
  if (FILE* f = fopen(EePath().c_str(), "wb")) { fwrite(g_ee, 1, sizeof g_ee, f); fclose(f); }   // persist: the next title (a new host process) reads it back
}
