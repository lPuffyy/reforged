// Software reference renderer for the NV2A 3D pipeline: vertex fetch, vertex-program interpreter, triangle rasteriser, texture sampling,
// register combiners, blending. It exists to validate the decoded GPU state against the real game (and as the reference for the Vulkan back end);
// it is slow and only covers the features the title has needed so far. NV2A behaviour was taken from the hardware documentation (xemu's pgraph and
// envytools were read as reference).
#include "kernel.h"
#include "gpu_soft.h"
#include "plat.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <set>
#include <array>
#include <thread>

// ---- interface to pgraph.cpp ----
uint32_t Pgraph3dState(uint32_t method);
const uint32_t* Pgraph3dProgram();
const float* Pgraph3dConstants();
uint32_t Pgraph3dDmaInstance(int which);  // 0 color, 1 zeta, 2 texture A, 3 texture B, 4 vertex A, 5 vertex B
uint8_t* Nv2aDmaMap(uint32_t instance, uint32_t* limit);

namespace {
struct V4 { float x, y, z, w; };
typedef SoftVertex Vertex;
struct Pix { float c[4]; };              // r g b a
struct Out { float pos[4]; float d0[4], d1[4], fog, t[4][4]; };

float Fl(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
float Sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
uint32_t R(uint32_t m) { return Pgraph3dState(m); }

// ---------------- vertex program ----------------
uint32_t Field(const uint32_t* t, int dword, int pos, int bits) { return (t[dword] >> pos) & ((1u << bits) - 1); }

struct VmState { float r[12][4]; float a0; };

void Swz(const float* in, uint32_t sel, bool neg, float* out) {
  // sel packs x,y,z,w selectors (2 bits each, x in the high bits)
  for (int i = 0; i < 4; i++) { float v = in[(sel >> (6 - 2 * i)) & 3]; out[i] = neg ? -v : v; }
}

void ReadOperand(const uint32_t* tk, int which, const VmState& st, const Vertex& in, const float* consts, const float (*oreg)[4], float* out) {
  // which: 0 = A, 1 = B, 2 = C
  uint32_t mux, reg, swz, neg;
  if (which == 0) { mux = Field(tk, 2, 26, 2); reg = Field(tk, 2, 28, 4); swz = Field(tk, 1, 0, 8); neg = Field(tk, 1, 8, 1); }
  else if (which == 1) { mux = Field(tk, 2, 11, 2); reg = Field(tk, 2, 13, 4); swz = Field(tk, 2, 17, 8); neg = Field(tk, 2, 25, 1); }
  else { mux = Field(tk, 3, 28, 2); reg = (Field(tk, 2, 0, 2) << 2) | Field(tk, 3, 30, 2); swz = Field(tk, 2, 2, 8); neg = Field(tk, 2, 10, 1); }
  float src[4] = {0, 0, 0, 1};
  if (mux == 1) { if (reg == 12) memcpy(src, oreg[0], 16); else memcpy(src, st.r[reg % 12], 16); }   // r12 reads back the oPos output register
  else if (mux == 2) memcpy(src, in.a[Field(tk, 1, 9, 4)], 16);
  else if (mux == 3) {
    int idx = (int)Field(tk, 1, 13, 8); if (Field(tk, 3, 1, 1)) idx += (int)st.a0;
    if (idx < 0) idx = 0; if (idx > 191) idx = 191;
    memcpy(src, consts + idx * 4, 16);
  }
  Swz(src, swz, neg != 0, out);
}

void RunVertexProgram(const Vertex& in, Out* out) {
  const uint32_t* prog = Pgraph3dProgram(); const float* consts = Pgraph3dConstants();
  VmState st; memset(&st, 0, sizeof st);
  float o[16][4]; for (int i = 0; i < 16; i++) { o[i][0] = o[i][1] = o[i][2] = 0; o[i][3] = 1; }
  uint32_t start = R(0x1EA0);
  for (uint32_t pc = start; pc < 136; pc++) {
    const uint32_t* tk = prog + pc * 4;
    uint32_t mac = Field(tk, 1, 21, 4), ilu = Field(tk, 1, 25, 3);
    float a[4] = {}, b[4] = {}, c[4] = {};
    if (mac || ilu) { ReadOperand(tk, 0, st, in, consts, o, a); ReadOperand(tk, 1, st, in, consts, o, b); ReadOperand(tk, 2, st, in, consts, o, c); }
    float m[4] = {}, il[4] = {}; bool arl = false;
    switch (mac) {
      case 1: memcpy(m, a, 16); break;
      case 2: for (int i = 0; i < 4; i++) m[i] = a[i] * b[i]; break;
      case 3: for (int i = 0; i < 4; i++) m[i] = a[i] + c[i]; break;
      case 4: for (int i = 0; i < 4; i++) m[i] = a[i] * b[i] + c[i]; break;
      case 5: m[0] = m[1] = m[2] = m[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; break;
      case 6: m[0] = m[1] = m[2] = m[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3]; break;
      case 7: m[0] = m[1] = m[2] = m[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; break;
      case 8: m[0] = 1; m[1] = a[1] * b[1]; m[2] = a[2]; m[3] = b[3]; break;
      case 9: for (int i = 0; i < 4; i++) m[i] = std::min(a[i], b[i]); break;
      case 10: for (int i = 0; i < 4; i++) m[i] = std::max(a[i], b[i]); break;
      case 11: for (int i = 0; i < 4; i++) m[i] = a[i] < b[i] ? 1.f : 0.f; break;
      case 12: for (int i = 0; i < 4; i++) m[i] = a[i] >= b[i] ? 1.f : 0.f; break;
      case 13: arl = true; break;
    }
    switch (ilu) {
      case 1: memcpy(il, c, 16); break;
      case 2: { float v = c[0] == 0 ? INFINITY : 1.f / c[0]; il[0] = il[1] = il[2] = il[3] = v; break; }
      case 3: { float v = c[0] == 0 ? INFINITY : 1.f / c[0]; v = std::max(std::min(v, 1.8446744e19f), -1.8446744e19f); il[0] = il[1] = il[2] = il[3] = v; break; }
      case 4: { float v = c[0] == 0 ? INFINITY : 1.f / sqrtf(fabsf(c[0])); il[0] = il[1] = il[2] = il[3] = v; break; }
      case 5: { float f = floorf(c[0]); il[0] = exp2f(f); il[1] = c[0] - f; il[2] = exp2f(c[0]); il[3] = 1; break; }
      case 6: { float v = fabsf(c[0]); int e; float mant = frexpf(v, &e); il[0] = (float)(e - 1); il[1] = mant * 2; il[2] = v == 0 ? -INFINITY : log2f(v); il[3] = 1; break; }
      case 7: { float x = std::max(c[0], 0.f), y = std::max(c[1], 0.f), w = std::min(std::max(c[3], -127.9961f), 127.9961f); il[0] = 1; il[1] = x; il[2] = x > 0 ? powf(std::max(y, 0.f), w) : 0.f; il[3] = 1; break; }
    }
    uint32_t outR = Field(tk, 3, 20, 4), outMux = Field(tk, 3, 2, 1), oMask = Field(tk, 3, 12, 4), orb = Field(tk, 3, 11, 1), addr = Field(tk, 3, 3, 8);
    uint32_t macMask = Field(tk, 3, 24, 4), iluMask = Field(tk, 3, 16, 4);
    bool paired = mac && ilu;
    auto writeMask = [](float* dst, const float* v, uint32_t mask) { for (int i = 0; i < 4; i++) if (mask & (8 >> i)) dst[i] = v[i]; };
    if (mac) {
      if (arl) st.a0 = floorf(a[0]);
      else if (!(paired && outR == 1)) writeMask(st.r[outR % 12], m, macMask);
    }
    if (ilu) { uint32_t r = paired ? 1 : outR; writeMask(st.r[r % 12], il, iluMask); }
    if (oMask && orb == 1) {  // output register write (orb == 0 would be a writable constant: unsupported)
      const float* v = outMux == 0 ? m : il;
      if (outMux == 0 ? mac != 0 : ilu != 0) {
        uint32_t reg = addr & 0xF;
        if (reg < 16) writeMask(o[reg], v, oMask);
      }
    }
    if (Field(tk, 3, 0, 1)) break;  // FINAL
  }
  memcpy(out->pos, o[0], 16); memcpy(out->d0, o[3], 16); memcpy(out->d1, o[4], 16); out->fog = o[5][0];
  for (int i = 0; i < 4; i++) memcpy(out->t[i], o[9 + i], 16);
}

// ---------------- textures ----------------
struct Tex { bool linear = false; bool valid = false; const uint8_t* data = nullptr; int w = 0, h = 0, fmt = 0; bool swizzled = false; uint32_t pitch = 0; const uint32_t* pal = nullptr; int palLen = 0; };

uint32_t Unswizzle(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  // Morton (Z-order) interleave; for non-square textures the shorter axis runs out of bits first.
  uint32_t out = 0, bit = 1, xb = 1, yb = 1;
  while (xb < w || yb < h) {
    if (xb < w) { if (x & xb) out |= bit; bit <<= 1; xb <<= 1; }
    if (yb < h) { if (y & yb) out |= bit; bit <<= 1; yb <<= 1; }
  }
  return out;
}

void Dxt1Block(const uint8_t* b, uint32_t out[16], bool alphaBlock) {
  uint16_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8; uint32_t col[4];
  auto exp = [](uint16_t c, uint32_t* o) { o[0] = ((c >> 11) & 31) * 255 / 31; o[1] = ((c >> 5) & 63) * 255 / 63; o[2] = (c & 31) * 255 / 31; };
  uint32_t a[3], bb[3]; exp(c0, a); exp(c1, bb);
  col[0] = 0xFF000000u | a[0] << 16 | a[1] << 8 | a[2]; col[1] = 0xFF000000u | bb[0] << 16 | bb[1] << 8 | bb[2];
  if (c0 > c1 || alphaBlock) {
    for (int i = 0; i < 3; i++) { }
    col[2] = 0xFF000000u | ((2 * a[0] + bb[0]) / 3) << 16 | ((2 * a[1] + bb[1]) / 3) << 8 | ((2 * a[2] + bb[2]) / 3);
    col[3] = 0xFF000000u | ((a[0] + 2 * bb[0]) / 3) << 16 | ((a[1] + 2 * bb[1]) / 3) << 8 | ((a[2] + 2 * bb[2]) / 3);
  } else {
    col[2] = 0xFF000000u | ((a[0] + bb[0]) / 2) << 16 | ((a[1] + bb[1]) / 2) << 8 | ((a[2] + bb[2]) / 2);
    col[3] = 0;  // transparent black
  }
  uint32_t bits = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
  for (int i = 0; i < 16; i++) out[i] = col[(bits >> (2 * i)) & 3];
}

// Fetch one texel as ARGB.
uint32_t TexelARGB(const Tex& t, int x, int y) {
  x = std::max(0, std::min(x, t.w - 1)); y = std::max(0, std::min(y, t.h - 1));
  auto at = [&](int bpp) -> const uint8_t* {
    uint32_t off = t.swizzled ? Unswizzle(x, y, t.w, t.h) * bpp : (uint32_t)y * t.pitch + x * bpp; return t.data + off; };
  switch (t.fmt) {
    case 0x06: case 0x12: return *(const uint32_t*)at(4);                              // A8R8G8B8 (swizzled / linear)
    case 0x07: case 0x1E: return *(const uint32_t*)at(4) | 0xFF000000u;                // X8R8G8B8
    case 0x05: case 0x11: { uint16_t v = *(const uint16_t*)at(2); return 0xFF000000u | ((v >> 11) * 255 / 31) << 16 | (((v >> 5) & 63) * 255 / 63) << 8 | ((v & 31) * 255 / 31); }  // R5G6B5
    case 0x02: case 0x10: { uint16_t v = *(const uint16_t*)at(2); return ((v >> 15) ? 0xFF000000u : 0) | (((v >> 10) & 31) * 255 / 31) << 16 | (((v >> 5) & 31) * 255 / 31) << 8 | ((v & 31) * 255 / 31); }  // A1R5G5B5
    case 0x03: case 0x1C: { uint16_t v = *(const uint16_t*)at(2); return 0xFF000000u | (((v >> 10) & 31) * 255 / 31) << 16 | (((v >> 5) & 31) * 255 / 31) << 8 | ((v & 31) * 255 / 31); }  // X1R5G5B5
    case 0x04: case 0x1D: { uint16_t v = *(const uint16_t*)at(2); uint32_t a = (v >> 12) * 17, r = ((v >> 8) & 15) * 17, g = ((v >> 4) & 15) * 17, b = (v & 15) * 17; return a << 24 | r << 16 | g << 8 | b; }  // A4R4G4B4
    case 0x0B: { uint32_t idx = *at(1); return (t.pal && (int)idx < t.palLen) ? t.pal[idx] : 0xFF000000u; }              // P8 via palette
    case 0x00: case 0x13: { uint32_t v = *at(1); return 0xFF000000u | v << 16 | v << 8 | v; }                    // Y8
    case 0x19: case 0x1F: { uint32_t v = *at(1); return v << 24 | 0x00FFFFFF; }                                    // A8
    case 0x01: case 0x1B: { uint32_t v = *at(1); return v << 24 | v << 16 | v << 8 | v; }                         // AY8
    case 0x1A: case 0x20: { uint16_t v = *(const uint16_t*)at(2); uint32_t l = v & 255, a = v >> 8; return a << 24 | l << 16 | l << 8 | l; }  // A8Y8
    case 0x0C: case 0x0E: case 0x0F: {                                                                            // DXT1 / DXT3 / DXT5 (block compressed, always swizzled by block order = linear blocks)
      int bw = (t.w + 3) / 4, bx = x / 4, by = y / 4; int bsz = t.fmt == 0x0C ? 8 : 16;
      const uint8_t* blk = t.data + ((size_t)by * bw + bx) * bsz; uint32_t px[16];
      Dxt1Block(blk + (bsz == 8 ? 0 : 8), px, bsz == 16);
      uint32_t c = px[(y & 3) * 4 + (x & 3)];
      if (t.fmt == 0x0E) { uint32_t a = (blk[((y & 3) * 4 + (x & 3)) / 2] >> (4 * (x & 1))) & 15; c = (c & 0xFFFFFF) | (a * 17) << 24; }
      else if (t.fmt == 0x0F) {
        uint8_t a0 = blk[0], a1 = blk[1]; uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)blk[2 + i] << (8 * i);
        int idx = (int)((bits >> (3 * ((y & 3) * 4 + (x & 3)))) & 7); uint32_t a;
        if (a0 > a1) a = idx == 0 ? a0 : idx == 1 ? a1 : ((8 - idx) * a0 + (idx - 1) * a1) / 7;
        else a = idx == 0 ? a0 : idx == 1 ? a1 : idx == 6 ? 0 : idx == 7 ? 255 : ((6 - idx) * a0 + (idx - 1) * a1) / 5;
        c = (c & 0xFFFFFF) | a << 24;
      }
      return c;
    }
    default: { static std::set<uint32_t> seen; if (seen.insert(t.fmt).second) Log("[tex] UNSUPPORTED texture colour format 0x%02x (%dx%d swizzled=%d) -> magenta", t.fmt, t.w, t.h, (int)t.swizzled); return 0xFFFF00FF; }  // unknown format: magenta so it is visible
  }
}

Tex GetTexture(int i) {
  Tex t; uint32_t base = 0x1B00 + 64 * i, off = R(base), fmt = R(base + 4), ctl0 = R(base + 0xC), ctl1 = R(base + 0x10), rect = R(base + 0x1C);
  if (!(ctl0 & 0x40000000u)) return t;  // texture stage disabled
  uint32_t dma = ((fmt & 3) == 2) ? Pgraph3dDmaInstance(3) : Pgraph3dDmaInstance(2);
  if (!dma) return t;
  uint32_t lim; uint8_t* p = Nv2aDmaMap(dma, &lim);
  t.fmt = (fmt >> 8) & 0xFF; t.data = p + off; t.valid = true;
  {   // diagnostics: log each distinct kind of texture the title uses once (format, size, cube map, dimensionality, mip levels), so unsupported kinds are visible in the log
    static std::set<uint64_t> kinds; static int nk = 0;
    uint32_t cube = (fmt >> 2) & 1, dim = (fmt >> 4) & 15, mips = (fmt >> 16) & 15, lw = (fmt >> 20) & 15, lh = (fmt >> 24) & 15, ld = fmt >> 28;
    uint64_t key = (uint64_t)((fmt >> 8) & 0xFF) | (uint64_t)cube << 8 | (uint64_t)dim << 9 | (uint64_t)lw << 16 | (uint64_t)lh << 24 | (uint64_t)ld << 32 | (uint64_t)(rect >> 16) << 36;
    if (nk < 400 && kinds.insert(key).second) { ++nk; Log("[tex] stage %d: format 0x%02x, %s, dim %u, %ux%u (log2 %u,%u,%u), %u mip level(s)%s%s", i, (fmt >> 8) & 0xFF, cube ? "CUBE MAP" : "2D", dim, rect >> 16, rect & 0xFFFF, lw, lh, ld, mips, ((fmt >> 8) & 0xFF) >= 0x10 ? ", linear" : "", dim == 3 ? ", VOLUME" : ""); }
  }
  if (t.fmt == 0x0B) {   // P8: 256/128/64/32 ARGB palette entries (NV097_SET_TEXTURE_PALETTE: bit0 dma A/B, bits 2-3 length, offset in the upper bits)
    uint32_t pr = R(base + 0x20), pdma = (pr & 1) ? Pgraph3dDmaInstance(3) : Pgraph3dDmaInstance(2);
    if (pdma) { uint32_t plim; t.pal = (const uint32_t*)(Nv2aDmaMap(pdma, &plim) + (pr & 0xFFFFFFC0u)); t.palLen = 256 >> ((pr >> 2) & 3); }
  }
  bool linear = t.fmt >= 0x10 && t.fmt != 0x2C && !(t.fmt >= 0x0C && t.fmt <= 0x0F) && (t.fmt <= 0x20 || t.fmt >= 0x24);
  t.swizzled = !linear && !(t.fmt >= 0x0C && t.fmt <= 0x0F);
  if (t.fmt >= 0x0C && t.fmt <= 0x0F) t.swizzled = false;
  t.linear = linear;
  if (linear) { t.w = rect >> 16; t.h = rect & 0xFFFF; t.pitch = ctl1 >> 16; }
  else { t.w = 1 << ((fmt >> 20) & 15); t.h = 1 << ((fmt >> 24) & 15); t.pitch = 0; }
  return t;
}

void Sample(const Tex& t, float s, float tt, float* rgba) {
  // nearest sampling with wrap/clamp ignored (clamp): enough for the UI draws; bilinear comes later
  int x = (int)floorf(s * t.w), y = (int)floorf(tt * t.h);
  x = ((x % t.w) + t.w) % t.w; y = ((y % t.h) + t.h) % t.h;
  uint32_t c = TexelARGB(t, x, y);
  rgba[0] = ((c >> 16) & 255) / 255.f; rgba[1] = ((c >> 8) & 255) / 255.f; rgba[2] = (c & 255) / 255.f; rgba[3] = (c >> 24) / 255.f;
}

// ---------------- register combiners ----------------
struct CombState { float reg[16][4]; };

void ArgbToRgba(uint32_t v, float* o) { o[0] = ((v >> 16) & 255) / 255.f; o[1] = ((v >> 8) & 255) / 255.f; o[2] = (v & 255) / 255.f; o[3] = (v >> 24) / 255.f; }

float MapIn(float x, uint32_t mode) {
  switch (mode >> 5) {
    case 0: return std::max(0.f, x);
    case 1: return 1.f - std::max(0.f, x);
    case 2: return 2.f * std::max(0.f, x) - 1.f;
    case 3: return 1.f - 2.f * std::max(0.f, x);
    case 4: return std::max(0.f, x) - 0.5f;
    case 5: return 0.5f - std::max(0.f, x);
    case 6: return x;
    default: return -x;
  }
}

// one input -> vec3 (rgb) or scalar (alpha)
void FetchInput(const CombState& cs, uint32_t in, bool alphaStage, float* v3, float* v1) {
  uint32_t reg = in & 15, alphaSel = (in >> 4) & 1, mode = in & 0xE0;
  float src[4] = {0, 0, 0, 0};
  if (reg != 0) memcpy(src, cs.reg[reg], 16);
  if (alphaStage) { *v1 = MapIn(alphaSel ? src[3] : src[2], mode); return; }
  if (alphaSel) { v3[0] = v3[1] = v3[2] = MapIn(src[3], mode); }
  else { v3[0] = MapIn(src[0], mode); v3[1] = MapIn(src[1], mode); v3[2] = MapIn(src[2], mode); }
}

void RunCombiners(CombState& cs, const float* factor0[8], const float* factor1[8], uint32_t numStages, float* finalRgb, float* finalA) {
  for (uint32_t s = 0; s < numStages && s < 8; s++) {
    cs.reg[1][0] = factor0[s][0]; cs.reg[1][1] = factor0[s][1]; cs.reg[1][2] = factor0[s][2]; cs.reg[1][3] = factor0[s][3];
    cs.reg[2][0] = factor1[s][0]; cs.reg[2][1] = factor1[s][1]; cs.reg[2][2] = factor1[s][2]; cs.reg[2][3] = factor1[s][3];
    uint32_t icwC = R(0xAC0 + 4 * s), icwA = R(0x260 + 4 * s), ocwC = R(0x1E40 + 4 * s), ocwA = R(0xAA0 + 4 * s);
    // ---- RGB ----
    {
      float A[3], B[3], C[3], D[3], dummy;
      FetchInput(cs, icwC >> 24, false, A, &dummy); FetchInput(cs, (icwC >> 16) & 255, false, B, &dummy);
      FetchInput(cs, (icwC >> 8) & 255, false, C, &dummy); FetchInput(cs, icwC & 255, false, D, &dummy);
      uint32_t flags = ocwC >> 12; bool abDot = flags & 2, cdDot = flags & 1, mux = flags & 4; uint32_t mapping = flags & 0x38;
      float ab[3], cd[3], sum[3];
      if (abDot) { float d = A[0] * B[0] + A[1] * B[1] + A[2] * B[2]; ab[0] = ab[1] = ab[2] = d; } else for (int i = 0; i < 3; i++) ab[i] = A[i] * B[i];
      if (cdDot) { float d = C[0] * D[0] + C[1] * D[1] + C[2] * D[2]; cd[0] = cd[1] = cd[2] = d; } else for (int i = 0; i < 3; i++) cd[i] = C[i] * D[i];
      bool useAB = cs.reg[12][3] < 0.5f;  // mux on r0.a (lsb mode only)
      for (int i = 0; i < 3; i++) sum[i] = mux ? (useAB ? ab[i] : cd[i]) : ab[i] + cd[i];
      auto post = [&](float* x) {
        for (int i = 0; i < 3; i++) {
          switch (mapping) { case 0x08: x[i] -= 0.5f; break; case 0x10: x[i] *= 2; break; case 0x18: x[i] = (x[i] - 0.5f) * 2; break; case 0x20: x[i] *= 4; break; case 0x30: x[i] /= 2; break; }
          x[i] = std::max(-1.f, std::min(x[i], 1.f));
        }
      };
      post(ab); post(cd); post(sum);
      if (ocwC & 0xF) { uint32_t r = ocwC & 15; for (int i = 0; i < 3; i++) cs.reg[r][i] = cd[i]; }
      if ((ocwC >> 4) & 0xF) { uint32_t r = (ocwC >> 4) & 15; for (int i = 0; i < 3; i++) cs.reg[r][i] = ab[i]; }
      if ((ocwC >> 8) & 0xF) { uint32_t r = (ocwC >> 8) & 15; for (int i = 0; i < 3; i++) cs.reg[r][i] = sum[i]; }
    }
    // ---- Alpha ----
    {
      float A, B, C, D, dummy[3];
      FetchInput(cs, icwA >> 24, true, dummy, &A); FetchInput(cs, (icwA >> 16) & 255, true, dummy, &B);
      FetchInput(cs, (icwA >> 8) & 255, true, dummy, &C); FetchInput(cs, icwA & 255, true, dummy, &D);
      uint32_t flags = ocwA >> 12; bool mux = flags & 4; uint32_t mapping = flags & 0x38;
      float ab = A * B, cd = C * D; bool useAB = cs.reg[12][3] < 0.5f;
      float sum = mux ? (useAB ? ab : cd) : ab + cd;
      auto post = [&](float x) {
        switch (mapping) { case 0x08: x -= 0.5f; break; case 0x10: x *= 2; break; case 0x18: x = (x - 0.5f) * 2; break; case 0x20: x *= 4; break; case 0x30: x /= 2; break; }
        return std::max(-1.f, std::min(x, 1.f)); };
      ab = post(ab); cd = post(cd); sum = post(sum);
      if (ocwA & 0xF) cs.reg[ocwA & 15][3] = cd;
      if ((ocwA >> 4) & 0xF) cs.reg[(ocwA >> 4) & 15][3] = ab;
      if ((ocwA >> 8) & 0xF) cs.reg[(ocwA >> 8) & 15][3] = sum;
    }
  }
  // ---- final combiner ----
  uint32_t f0 = R(0x288), f1 = R(0x28C);
  if (!f0 && !f1) { finalRgb[0] = cs.reg[4][0]; finalRgb[1] = cs.reg[4][1]; finalRgb[2] = cs.reg[4][2]; *finalA = cs.reg[4][3]; if (numStages) { memcpy(finalRgb, cs.reg[12], 12); *finalA = cs.reg[12][3]; } return; }
  uint32_t flags = f1 & 0xFF;
  float v1r0[3]; for (int i = 0; i < 3; i++) { float v = cs.reg[5][i] + cs.reg[12][i]; v1r0[i] = (flags & 0x80) ? Sat(v) : v; }
  cs.reg[14][0] = v1r0[0]; cs.reg[14][1] = v1r0[1]; cs.reg[14][2] = v1r0[2]; cs.reg[14][3] = 0;
  auto get = [&](uint32_t in, float* o3, float* oa) {
    uint32_t reg = in & 15, alphaSel = (in >> 4) & 1, mode = in & 0xE0; float src[4] = {0, 0, 0, 0};
    if (reg == 0xF) { /* EF_PROD filled below */ }
    if (reg) memcpy(src, cs.reg[reg], 16);
    for (int i = 0; i < 3; i++) o3[i] = alphaSel ? MapIn(src[3], mode) : MapIn(src[i], mode);
    *oa = MapIn(src[3], mode);
  };
  float e[3], ef[3], g[3], ea, fa, ga, dummy;
  get(f1 >> 24, e, &ea); get((f1 >> 16) & 255, ef, &fa); for (int i = 0; i < 3; i++) cs.reg[15][i] = e[i] * ef[i];
  float a3[3], b3[3], c3[3], d3[3];
  get(f0 >> 24, a3, &dummy); get((f0 >> 16) & 255, b3, &dummy); get((f0 >> 8) & 255, c3, &dummy); get(f0 & 255, d3, &dummy);
  get((f1 >> 8) & 255, g, &ga);
  for (int i = 0; i < 3; i++) finalRgb[i] = Sat(a3[i] * b3[i] + (1.f - a3[i]) * c3[i] + d3[i]);
  *finalA = Sat(ga);
}

// ---------------- rasteriser ----------------
struct Surface { uint8_t* base; uint32_t pitch; int bpp; uint32_t fmt; int x0, y0, x1, y1; uint8_t* zbase; uint32_t zpitch; int zfmt; float zmax; int ymin, ymax; };   // [ymin,ymax] = rows this thread draws

bool GetColorSurface(Surface* s) {
  uint32_t dma = Pgraph3dDmaInstance(0); if (!dma) return false;
  uint32_t lim; s->base = Nv2aDmaMap(dma, &lim) + R(0x210); uint32_t sf = R(0x208);
  s->fmt = sf & 0xF; s->pitch = R(0x20C) & 0xFFFF; if (((sf >> 8) & 0xF) != 1) return false;  // pitch surfaces only
  s->bpp = (s->fmt == 1 || s->fmt == 2 || s->fmt == 3) ? 2 : 4;
  uint32_t ch = R(0x200), cv = R(0x204);
  s->x0 = ch & 0xFFFF; s->y0 = cv & 0xFFFF; s->x1 = s->x0 + (ch >> 16); s->y1 = s->y0 + (cv >> 16);
  s->zbase = nullptr; s->zfmt = (sf >> 4) & 0xF; s->zpitch = R(0x20C) >> 16; s->zmax = 16777215.f;
  uint32_t zdma = Pgraph3dDmaInstance(1); if (zdma && s->zfmt) { uint32_t zl; s->zbase = Nv2aDmaMap(zdma, &zl) + R(0x214); }
  { float zm = Fl(R(0x398)); s->zmax = zm > 0 ? zm : 16777215.f; }   // SET_CLIP_MAX (z range of the program output)
  s->ymin = s->y0; s->ymax = s->y1 - 1;
  return true;
}

void ReadPix(const Surface& s, int x, int y, float* o) {
  uint8_t* p = s.base + (size_t)y * s.pitch + x * s.bpp;
  if (s.bpp == 4) { uint32_t c = *(uint32_t*)p; ArgbToRgba(c, o); if (s.fmt == 4 || s.fmt == 5) o[3] = 1; }
  else { uint16_t v = *(uint16_t*)p; if (s.fmt == 3) { o[0] = (v >> 11) / 31.f; o[1] = ((v >> 5) & 63) / 63.f; o[2] = (v & 31) / 31.f; o[3] = 1; } else { o[0] = ((v >> 10) & 31) / 31.f; o[1] = ((v >> 5) & 31) / 31.f; o[2] = (v & 31) / 31.f; o[3] = (v >> 15) ? 1.f : 0.f; } }
}
void WritePix(const Surface& s, int x, int y, const float* c) {
  uint8_t* p = s.base + (size_t)y * s.pitch + x * s.bpp;
  uint32_t r = (uint32_t)(Sat(c[0]) * 255.f + 0.5f), g = (uint32_t)(Sat(c[1]) * 255.f + 0.5f), b = (uint32_t)(Sat(c[2]) * 255.f + 0.5f), a = (uint32_t)(Sat(c[3]) * 255.f + 0.5f);
  if (s.bpp == 4) *(uint32_t*)p = a << 24 | r << 16 | g << 8 | b;
  else if (s.fmt == 3) *(uint16_t*)p = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
  else *(uint16_t*)p = (uint16_t)((a ? 0x8000 : 0) | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
}

float BlendFactor(uint32_t f, const float* src, const float* dst, const float* constColor, int comp) {
  switch (f) {
    case 0: return 0; case 1: return 1;
    case 0x300: return src[comp]; case 0x301: return 1 - src[comp];
    case 0x302: return src[3]; case 0x303: return 1 - src[3];
    case 0x304: return dst[3]; case 0x305: return 1 - dst[3];
    case 0x306: return dst[comp]; case 0x307: return 1 - dst[comp];
    case 0x308: return comp == 3 ? 1.f : std::min(src[3], 1 - dst[3]);
    case 0x8001: return constColor[comp]; case 0x8002: return 1 - constColor[comp];
    case 0x8003: return constColor[3]; case 0x8004: return 1 - constColor[3];
  }
  return 1;
}

void DrawTriangle(const Surface& surf, const Out& v0, const Out& v1, const Out& v2) {
  float x0 = v0.pos[0], y0 = v0.pos[1], x1 = v1.pos[0], y1 = v1.pos[1], x2 = v2.pos[0], y2 = v2.pos[1];
  float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
  if (area == 0.f || !std::isfinite(area)) return;
  if (R(0x308)) {  // culling: front face from the sign of the screen-space area (y points down)
    uint32_t cullMode = R(0x39C), front = R(0x3A0);
    bool frontFace = front == 0x901 ? area < 0 : area > 0;
    if ((cullMode == 0x404 && frontFace) || (cullMode == 0x405 && !frontFace) || cullMode == 0x408) return;
  }
  int minx = std::max(surf.x0, (int)floorf(std::min({x0, x1, x2}))), maxx = std::min(surf.x1 - 1, (int)ceilf(std::max({x0, x1, x2})));
  int miny = std::max(surf.ymin, (int)floorf(std::min({y0, y1, y2}))), maxy = std::min(surf.ymax, (int)ceilf(std::max({y0, y1, y2})));
  if (minx > maxx || miny > maxy) return;
  float w0 = v0.pos[3], w1 = v1.pos[3], w2 = v2.pos[3];
  if (w0 == 0 || w1 == 0 || w2 == 0) { w0 = w1 = w2 = 1; }
  float iw0 = 1 / w0, iw1 = 1 / w1, iw2 = 1 / w2;
  Tex tex[4]; for (int i = 0; i < 4; i++) tex[i] = GetTexture(i);
  uint32_t numStages = R(0x1E60) & 0xFF;
  uint32_t texModeWord = R(0x1E70);
  float fac0[8][4], fac1[8][4]; const float* f0p[8]; const float* f1p[8];
  bool uniqueC0 = (R(0x1E60) >> 8) & 0x10, uniqueC1 = (R(0x1E60) >> 8) & 0x100;
  for (int i = 0; i < 8; i++) { ArgbToRgba(R(0xA60 + 4 * (uniqueC0 ? i : 0)), fac0[i]); ArgbToRgba(R(0xA80 + 4 * (uniqueC1 ? i : 0)), fac1[i]); f0p[i] = fac0[i]; f1p[i] = fac1[i]; }
  float fogCol[4]; ArgbToRgba(R(0x294), fogCol);
  bool blend = R(0x304); uint32_t sf = R(0x344), df = R(0x348), eq = R(0x350); float bc[4]; ArgbToRgba(R(0x34C), bc);
  bool alphaTest = R(0x300); uint32_t alphaFunc = R(0x33C), alphaRef = R(0x340) & 0xFF;
  uint32_t colorMask = R(0x358); bool depthTest = R(0x30C); uint32_t depthFunc = R(0x354), depthMask = R(0x35C);
  for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
    float px = x + 0.5f, py = y + 0.5f;
    float e0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;   // weight of v0
    float e1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;   // weight of v1
    float e2 = 1 - e0 - e1;
    if (e0 < 0 || e1 < 0 || e2 < 0) continue;
    float pw = e0 * iw0 + e1 * iw1 + e2 * iw2; if (pw == 0) continue;
    float b0 = e0 * iw0 / pw, b1 = e1 * iw1 / pw, b2 = e2 * iw2 / pw;
    auto lerp4 = [&](const float* a, const float* b, const float* c, float* o) { for (int i = 0; i < 4; i++) o[i] = a[i] * b0 + b[i] * b1 + c[i] * b2; };
    float zpix = e0 * v0.pos[2] + e1 * v1.pos[2] + e2 * v2.pos[2];
    uint32_t zval = 0; uint8_t* zp = nullptr; bool zwrite = false;
    if (surf.zbase && depthTest) {
      float zd = zpix / surf.zmax; zd = zd < 0 ? 0 : zd > 1 ? 1 : zd;
      zp = surf.zbase + (size_t)y * surf.zpitch + (size_t)x * (surf.zfmt == 1 ? 2 : 4);
      uint32_t zmaxi = surf.zfmt == 1 ? 0xFFFF : 0xFFFFFF; zval = (uint32_t)(zd * zmaxi);
      uint32_t cur = surf.zfmt == 1 ? *(uint16_t*)zp : (*(uint32_t*)zp >> 8); bool pass;
      switch (depthFunc) { case 0x200: pass = false; break; case 0x201: pass = zval < cur; break; case 0x202: pass = zval == cur; break; case 0x203: pass = zval <= cur; break; case 0x204: pass = zval > cur; break; case 0x205: pass = zval != cur; break; case 0x206: pass = zval >= cur; break; default: pass = true; }
      if (!pass) continue; zwrite = depthMask != 0;
    }
    CombState cs; memset(&cs, 0, sizeof cs);
    float d0[4], d1[4]; lerp4(v0.d0, v1.d0, v2.d0, d0); lerp4(v0.d1, v1.d1, v2.d1, d1);
    for (int i = 0; i < 4; i++) { d0[i] = Sat(d0[i]); d1[i] = Sat(d1[i]); }
    memcpy(cs.reg[4], d0, 16); memcpy(cs.reg[5], d1, 16); memcpy(cs.reg[3], fogCol, 16);
    for (int t = 0; t < 4; t++) {
      float tc[4]; lerp4(v0.t[t], v1.t[t], v2.t[t], tc);
      uint32_t mode = (texModeWord >> (5 * t)) & 0x1F;
      if (mode == 0 || !tex[t].valid) { memset(cs.reg[8 + t], 0, 16); if (mode == 0 && tex[t].valid) { } continue; }
      float s = tc[0], tt = tc[1]; if (mode == 1 && tc[3] != 0) { s /= tc[3]; tt /= tc[3]; }  // PROJECT2D
      if (tex[t].linear) { s /= tex[t].w; tt /= tex[t].h; }                                     // linear textures take unnormalised coordinates
      Sample(tex[t], s, tt, cs.reg[8 + t]);
    }
    float rgb[3], a;
    RunCombiners(cs, f0p, f1p, numStages, rgb, &a);
    float src[4] = {rgb[0], rgb[1], rgb[2], a};
    if (x == 320 && y == 240) { static int pl = getenv("XBOX_VLOG") ? 4 : 0; if (pl > 0) { --pl; Log("[soft] pixel(320,240): stages=%u t0=(%g %g %g %g) v0=(%g %g %g %g) r0=(%g %g %g %g) final=(%g %g %g %g) icw0=%08x ocw0=%08x aicw0=%08x aocw0=%08x f0=%08x f1=%08x texmode=%08x fac0[0]=%08x fac1[0]=%08x fac0[1]=%08x fac1[1]=%08x ocw1=%08x icw1=%08x aicw1=%08x aocw1=%08x", numStages, cs.reg[8][0], cs.reg[8][1], cs.reg[8][2], cs.reg[8][3], cs.reg[4][0], cs.reg[4][1], cs.reg[4][2], cs.reg[4][3], cs.reg[12][0], cs.reg[12][1], cs.reg[12][2], cs.reg[12][3], rgb[0], rgb[1], rgb[2], a, R(0xAC0), R(0x1E40), R(0x260), R(0xAA0), R(0x288), R(0x28C), R(0x1E70), R(0xA60), R(0xA80), R(0xA64), R(0xA84), R(0x1E44), R(0xAC4), R(0x264), R(0xAA4)); } }
    if (alphaTest) {
      uint32_t aa = (uint32_t)(Sat(a) * 255.f + 0.5f); bool pass = true;
      switch (alphaFunc) { case 0x200: pass = false; break; case 0x201: pass = aa < alphaRef; break; case 0x202: pass = aa == alphaRef; break; case 0x203: pass = aa <= alphaRef; break; case 0x204: pass = aa > alphaRef; break; case 0x205: pass = aa != alphaRef; break; case 0x206: pass = aa >= alphaRef; break; default: break; }
      if (!pass) continue;
    }
    float dst[4]; ReadPix(surf, x, y, dst); float out[4];
    if (blend) {
      for (int i = 0; i < 4; i++) {
        float sfv = BlendFactor(sf, src, dst, bc, i), dfv = BlendFactor(df, src, dst, bc, i);
        float s = src[i] * sfv, d = dst[i] * dfv;
        out[i] = eq == 0x800A ? s - d : eq == 0x800B ? d - s : eq == 0x8007 ? std::min(src[i], dst[i]) : eq == 0x8008 ? std::max(src[i], dst[i]) : s + d;
      }
    } else memcpy(out, src, 16);
    if (!(colorMask & 0x00010000)) out[0] = dst[0];   // SET_COLOR_MASK: red bit 16, green bit 8, blue bit 0, alpha bit 24
    if (!(colorMask & 0x00000100)) out[1] = dst[1];
    if (!(colorMask & 0x00000001)) out[2] = dst[2];
    if (!(colorMask & 0x01000000)) out[3] = dst[3];
    WritePix(surf, x, y, out);
    if (zwrite) { if (surf.zfmt == 1) *(uint16_t*)zp = (uint16_t)zval; else *(uint32_t*)zp = (zval << 8) | (*(uint32_t*)zp & 0xFF); }
  }
}
}  // namespace

static void SoftDrawImpl(uint32_t prim, const SoftVertex* verts, size_t n);
void SoftDraw(uint32_t prim, const SoftVertex* verts, size_t n) {
  uint32_t t0 = (uint32_t)plat::TickMs(); SoftDrawImpl(prim, verts, n); uint32_t dt = (uint32_t)plat::TickMs() - t0;
  static int slowLog = 40; if (dt > 150 && slowLog > 0) { --slowLog; Log("[soft] SLOW draw: prim=%u vertices=%zu took %lu ms (tex0=0x%08x fmt, mode=0x%x, stages=%u, blend=%u, depth=%u)", prim, n, dt, R(0x1B04), R(0x1E94), R(0x1E60) & 0xFF, R(0x304), R(0x30C)); }
}
static void SoftDrawImpl(uint32_t prim, const SoftVertex* verts, size_t n) {
  Surface surf; if (!GetColorSurface(&surf)) { static bool warned = false; if (!warned) { warned = true; Log("[soft] colour surface not supported (swizzled or no DMA): draw skipped"); } return; }
  if ((R(0x1E94) & 3) != 2) { static bool warned = false; if (!warned) { warned = true; Log("[soft] fixed-function transform not implemented: draw skipped"); } return; }
  static int dumps = 0; static int drawIdx = 0; ++drawIdx; static int dumpFrom = getenv("XBOX_DUMP_FROM") ? atoi(getenv("XBOX_DUMP_FROM")) : 0;
  bool wantDump = getenv("XBOX_DUMP") && dumps < 6 && drawIdx >= dumpFrom;
  if (wantDump) {
    Tex t0 = GetTexture(0);
    if (t0.valid && t0.w > 0 && t0.h > 0) { std::vector<uint32_t> img((size_t)t0.w * t0.h); for (int y = 0; y < t0.h; y++) for (int x = 0; x < t0.w; x++) img[(size_t)y * t0.w + x] = TexelARGB(t0, x, y); char path[100]; sprintf(path, "work/dump_tex0_%d.bmp", dumps); DumpBmp(path, img.data(), t0.w, t0.h); Log("[soft] dumped tex0 %dx%d fmt=0x%x swz=%d -> %s", t0.w, t0.h, t0.fmt, (int)t0.swizzled, path); }
  }
  std::vector<Out> o(n);
  for (size_t i = 0; i < n; i++) RunVertexProgram(verts[i], &o[i]);
  { static int vl = getenv("XBOX_VLOG") ? 3 : 0;
    if (vl > 0) { --vl; Log("[soft] prim=%u n=%zu start=%u clip=(%d,%d)-(%d,%d)", prim, n, R(0x1EA0), surf.x0, surf.y0, surf.x1, surf.y1);
      for (size_t i = 0; i < n && i < 6; i++) Log("[soft]  v%zu in.pos=(%g %g %g %g) in.a3=(%g %g %g %g) in.a9=(%g %g %g %g) -> pos=(%g %g %g %g) d0=(%g %g %g %g) t0=(%g %g %g %g)", i, verts[i].a[0][0], verts[i].a[0][1], verts[i].a[0][2], verts[i].a[0][3], verts[i].a[3][0], verts[i].a[3][1], verts[i].a[3][2], verts[i].a[3][3], verts[i].a[9][0], verts[i].a[9][1], verts[i].a[9][2], verts[i].a[9][3], o[i].pos[0], o[i].pos[1], o[i].pos[2], o[i].pos[3], o[i].d0[0], o[i].d0[1], o[i].d0[2], o[i].d0[3], o[i].t[0][0], o[i].t[0][1], o[i].t[0][2], o[i].t[0][3]);
      const uint32_t* pr = Pgraph3dProgram();
      static const char* macN[] = {"nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4", "dst", "min", "max", "slt", "sge", "arl", "?", "?"};
      static const char* iluN[] = {"nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit"};
      static const char* oN[] = {"oPos", "?", "?", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1", "oT0", "oT1", "oT2", "oT3", "?", "?", "?"};
      for (uint32_t k = R(0x1EA0); k < 136; k++) {
        const uint32_t* t = pr + k * 4; auto F2 = [&](int d, int p2, int b) { return Field(t, d, p2, b); };
        char ops[3][40]; for (int w = 0; w < 3; w++) {
          uint32_t mux = w == 0 ? F2(2, 26, 2) : w == 1 ? F2(2, 11, 2) : F2(3, 28, 2), reg = w == 0 ? F2(2, 28, 4) : w == 1 ? F2(2, 13, 4) : (F2(2, 0, 2) << 2 | F2(3, 30, 2));
          uint32_t sw = w == 0 ? F2(1, 0, 8) : w == 1 ? F2(2, 17, 8) : F2(2, 2, 8), neg = w == 0 ? F2(1, 8, 1) : w == 1 ? F2(2, 25, 1) : F2(2, 10, 1);
          const char* sn = "xyzw"; char sws[5] = {sn[(sw >> 6) & 3], sn[(sw >> 4) & 3], sn[(sw >> 2) & 3], sn[sw & 3], 0};
          if (mux == 1) sprintf(ops[w], "%sr%u.%s", neg ? "-" : "", reg, sws); else if (mux == 2) sprintf(ops[w], "%sv%u.%s", neg ? "-" : "", F2(1, 9, 4), sws); else if (mux == 3) sprintf(ops[w], "%sc[%u%s].%s", neg ? "-" : "", F2(1, 13, 8), F2(3, 1, 1) ? "+a0" : "", sws); else sprintf(ops[w], "?");
        }
        Log("[soft]  %3u: mac=%s ilu=%s A=%s B=%s C=%s outR=r%u macmask=%x ilumask=%x -> %s mask=%x mux=%u%s", k, macN[F2(1, 21, 4)], iluN[F2(1, 25, 3)], ops[0], ops[1], ops[2], F2(3, 20, 4), F2(3, 24, 4), F2(3, 16, 4), F2(3, 11, 1) ? oN[F2(3, 3, 8) & 15] : "c[]", F2(3, 12, 4), F2(3, 2, 1), F2(3, 0, 1) ? " FINAL" : "");
        if (F2(3, 0, 1)) break;
      }
      const float* cc = Pgraph3dConstants(); for (int k = 0; k < 8; k++) Log("[soft]  c%d = %g %g %g %g", k, cc[k * 4], cc[k * 4 + 1], cc[k * 4 + 2], cc[k * 4 + 3]); } }
  std::vector<std::array<size_t, 3>> tris;
  auto tri = [&](size_t a, size_t b, size_t c) { tris.push_back({a, b, c}); };
  switch (prim) {
    case 5: for (size_t i = 0; i + 2 < n; i += 3) tri(i, i + 1, i + 2); break;
    case 6: for (size_t i = 0; i + 2 < n; i++) { if (i & 1) tri(i + 1, i, i + 2); else tri(i, i + 1, i + 2); } break;
    case 7: case 10: for (size_t i = 1; i + 1 < n; i++) tri(0, i, i + 1); break;
    case 8: for (size_t i = 0; i + 3 < n; i += 4) { tri(i, i + 1, i + 2); tri(i, i + 2, i + 3); } break;
    case 9: for (size_t i = 0; i + 3 < n; i += 2) { tri(i, i + 1, i + 3); tri(i, i + 3, i + 2); } break;
    default: break;  // points and lines are not rasterised yet
  }
  {
    int rows = surf.y1 - surf.y0; unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency())); if (tris.size() < 8) threads = 1;
    auto work = [&](int ya, int yb) { Surface sc = surf; sc.ymin = ya; sc.ymax = yb; for (auto& t : tris) DrawTriangle(sc, o[t[0]], o[t[1]], o[t[2]]); };
    if (threads == 1) work(surf.y0, surf.y1 - 1);
    else {
      std::vector<std::thread> th;
      for (unsigned i = 0; i < threads; i++) { int ya = surf.y0 + rows * i / threads, yb = surf.y0 + rows * (i + 1) / threads - 1; th.emplace_back(work, ya, yb); }
      for (auto& t : th) t.join();
    }
  }
  if (wantDump) {
    int w = surf.x1 - surf.x0, h = surf.y1 - surf.y0; std::vector<uint32_t> img((size_t)w * h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { float c[4]; ReadPix(surf, x + surf.x0, y + surf.y0, c); img[(size_t)y * w + x] = 0xFF000000u | (uint32_t)(c[0] * 255) << 16 | (uint32_t)(c[1] * 255) << 8 | (uint32_t)(c[2] * 255); }
    char path[100]; sprintf(path, "work/dump_fb_%d.bmp", dumps); DumpBmp(path, img.data(), w, h); Log("[soft] draw n=%zu prim=%u dumped colour surface -> %s", n, prim, path); ++dumps;
  }
}

// Decode texture 'stage' for the Vulkan back end: returns size + content hash (bgra == nullptr: cheap probe) or the decoded BGRA8 pixels.
static size_t TexBytes(const Tex& t) {
  switch (t.fmt) {
    case 0x0C: return (size_t)((t.w + 3) / 4) * ((t.h + 3) / 4) * 8;
    case 0x0E: case 0x0F: return (size_t)((t.w + 3) / 4) * ((t.h + 3) / 4) * 16;
    case 0x06: case 0x07: case 0x12: case 0x1E: return t.linear ? (size_t)t.pitch * t.h : (size_t)t.w * t.h * 4;
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1B: case 0x1F: return t.linear ? (size_t)t.pitch * t.h : (size_t)t.w * t.h;
    default: return t.linear ? (size_t)t.pitch * t.h : (size_t)t.w * t.h * 2;
  }
}
bool SoftScreenBBox(const SoftVertex* verts, size_t n, float* x0, float* y0, float* x1, float* y1) {   // debug: screen-space bounding box of a draw via the software vertex-program interpreter
  if (!n) return false; *x0 = *y0 = 1e30f; *x1 = *y1 = -1e30f;
  for (size_t i = 0; i < n && i < 4096; i++) { Out o; RunVertexProgram(verts[i], &o); float w = o.pos[3] != 0 ? o.pos[3] : 1.f, px = o.pos[0] / w, py = o.pos[1] / w; if (px < *x0) *x0 = px; if (px > *x1) *x1 = px; if (py < *y0) *y0 = py; if (py > *y1) *y1 = py; }
  return true;
}
bool SoftFirstVertexOut(const SoftVertex* verts, size_t n, float* pos, float* d0, float* d1, float* t0) {   // debug: vertex-program outputs of vertex 0
  if (!n) return false; Out o; RunVertexProgram(verts[0], &o); memcpy(pos, o.pos, 16); memcpy(d0, o.d0, 16); memcpy(d1, o.d1, 16); memcpy(t0, o.t[0], 16); return true;
}
bool SoftTextureDesc(int stage, uint64_t* key, int* w, int* h, bool* linear) {   // cheap identity of the bound texture (no content hashing)
  Tex t = GetTexture(stage); if (!t.valid || t.w <= 0 || t.h <= 0 || t.w > 4096 || t.h > 4096) return false;
  *w = t.w; *h = t.h; *linear = t.linear; *key = ((uint64_t)(uintptr_t)t.data * 1099511628211ull) ^ ((uint64_t)t.fmt << 40) ^ ((uint64_t)t.w << 20) ^ (uint64_t)t.h ^ ((uint64_t)TexBytes(t) << 52);
  return true;
}
bool SoftDecodeTexture(int stage, std::vector<uint32_t>* bgra, int* w, int* h, uint64_t* hash, bool* linear) {
  Tex t = GetTexture(stage); if (!t.valid || t.w <= 0 || t.h <= 0 || t.w > 4096 || t.h > 4096) return false;
  *w = t.w; *h = t.h; *linear = t.linear;
  size_t bytes = TexBytes(t); const uint64_t* p = (const uint64_t*)t.data; uint64_t hh = 1469598103934665603ull ^ (uint64_t)t.fmt ^ ((uint64_t)bytes << 8);
  for (size_t i = 0; i < bytes / 8; i++) { hh ^= p[i]; hh *= 1099511628211ull; hh ^= hh >> 29; }
  if (t.pal) { const uint64_t* pp = (const uint64_t*)t.pal; for (int i = 0; i < t.palLen / 2; i++) { hh ^= pp[i]; hh *= 1099511628211ull; hh ^= hh >> 29; } }
  *hash = hh;
  if (!bgra) return true;
  bgra->resize((size_t)t.w * t.h);
  for (int y = 0; y < t.h; y++) for (int x = 0; x < t.w; x++) (*bgra)[(size_t)y * t.w + x] = TexelARGB(t, x, y);
  return true;
}
