// PGRAPH, class 0x97 (3D): method state and the parts executed in software so far (surface setup, clears).
// State is stored per method word so every later stage (vertex fetch, programs, combiners, textures, draws) can read it back by name.
#include "kernel.h"
#include "gpu_soft.h"
#include "gpu_vk.h"
#include "plat.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <algorithm>
#include <mutex>
#include <vector>

uint8_t* Nv2aDmaMap(uint32_t instance, uint32_t* limit);
uint32_t MmioGet(uint32_t reg);

// NV097 method offsets used so far (hardware facts; see envytools / xemu nv2a_regs.h for the full list).
enum : uint32_t {
  M_CONTEXT_DMA_COLOR = 0x194, M_CONTEXT_DMA_ZETA = 0x198,
  M_SURFACE_CLIP_H = 0x200, M_SURFACE_CLIP_V = 0x204, M_SURFACE_FORMAT = 0x208, M_SURFACE_PITCH = 0x20C, M_SURFACE_COLOR_OFFSET = 0x210, M_SURFACE_ZETA_OFFSET = 0x214,
  M_BEGIN_END = 0x17FC, M_ELEM16 = 0x1800, M_ELEM32 = 0x1808, M_DRAW_ARRAYS = 0x1810, M_INLINE_ARRAY = 0x1818, M_EXEC_MODE = 0x1E94, M_PROG_LOAD = 0x1E9C, M_PROG_START = 0x1EA0, M_CONST_LOAD = 0x1EA4,
  M_ZSTENCIL_CLEAR = 0x1D8C, M_COLOR_CLEAR = 0x1D90, M_CLEAR_SURFACE = 0x1D94, M_CLEAR_RECT_H = 0x1D98, M_CLEAR_RECT_V = 0x1D9C,
};

namespace {
uint32_t g_pg[0x2000 / 4 + 1];       // last value written to each method (index = method / 4)
uint32_t g_dmaColor, g_dmaZeta, g_dmaTexA, g_dmaTexB, g_dmaVtxA, g_dmaVtxB;   // RAMIN addresses of the DMA objects the 3D class was told about
uint64_t g_clears = 0;
std::mutex g_frontMutex; std::vector<uint32_t> g_front; int g_frontW = 0, g_frontH = 0; bool g_frontValid = false;
// vertex program / constants
uint32_t g_prog[136 * 4]; uint32_t g_progLoad = 0; uint32_t g_const[192 * 4]; uint32_t g_constLoad = 0;
// draw capture
SoftVertex g_cur; std::vector<SoftVertex> g_verts; std::vector<std::pair<uint32_t, uint32_t>> g_ranges; bool g_curInit = false;
uint32_t g_prim = 0; std::vector<uint32_t> g_inline; std::vector<uint32_t> g_elems; uint32_t g_drawStart = 0, g_drawCount = 0; uint64_t g_draws = 0;


uint32_t& R(uint32_t method) { return g_pg[method >> 2]; }

int ColorBytes(uint32_t fmt) { switch (fmt & 0xF) { case 1: case 2: case 3: return 2; case 4: case 5: case 6: case 7: case 8: return 4; case 9: return 1; case 10: return 2; default: return 4; } }

void ClearSurface(uint32_t mask) {
  ++g_clears;
  if (GpuVkReady()) {   // GPU path: the render target lives in a Vulkan image
    uint32_t ch = R(M_SURFACE_CLIP_H), cv = R(M_SURFACE_CLIP_V), rh = R(M_CLEAR_RECT_H), rv = R(M_CLEAR_RECT_V);
    int cx = ch & 0xFFFF, cy = cv & 0xFFFF, cw = ch >> 16, cht = cv >> 16;
    int x0 = cx + (rh & 0xFFFF), x1 = std::min<int>(cx + (rh >> 16), cx + cw - 1), y0 = cy + (rv & 0xFFFF), y1 = std::min<int>(cy + (rv >> 16), cy + cht - 1);
    GpuClear(mask, R(M_COLOR_CLEAR), R(M_ZSTENCIL_CLEAR), x0, y0, x1, y1);
    return;
  }
  uint32_t fmt = R(M_SURFACE_FORMAT), colorFmt = fmt & 0xF;
  uint32_t clipX = R(M_SURFACE_CLIP_H) & 0xFFFF, clipW = R(M_SURFACE_CLIP_H) >> 16, clipY = R(M_SURFACE_CLIP_V) & 0xFFFF, clipH = R(M_SURFACE_CLIP_V) >> 16;
  uint32_t x0 = R(M_CLEAR_RECT_H) & 0xFFFF, x1 = R(M_CLEAR_RECT_H) >> 16, y0 = R(M_CLEAR_RECT_V) & 0xFFFF, y1 = R(M_CLEAR_RECT_V) >> 16;
  // clear rectangle is relative to the surface clip window; the hardware clips it to that window
  x0 += clipX; x1 += clipX; y0 += clipY; y1 += clipY;
  x1 = std::min(x1, clipX + clipW - 1); y1 = std::min(y1, clipY + clipH - 1);
  { static int l = 6; if (l-- > 0) Log("[pgraph] CLEAR mask=0x%x color=0x%08x dmaColor=0x%x fmt=0x%08x pitch=0x%x off=0x%x clip=(%u,%u,%u,%u) rect=(%u,%u)-(%u,%u)", mask, R(M_COLOR_CLEAR), g_dmaColor, fmt, R(M_SURFACE_PITCH), R(M_SURFACE_COLOR_OFFSET), clipX, clipY, clipW, clipH, x0, y0, x1, y1); }
  if ((mask & 0x3) && g_dmaZeta) {   // depth / stencil clear (Z16 or Z24S8, pitch layout)
    uint32_t lim; uint8_t* zb = Nv2aDmaMap(g_dmaZeta, &lim) + R(M_SURFACE_ZETA_OFFSET); uint32_t zpitch = R(M_SURFACE_PITCH) >> 16, zfmt = (fmt >> 4) & 0xF, zc = R(M_ZSTENCIL_CLEAR);
    if (((fmt >> 8) & 0xF) == 1) for (uint32_t y = y0; y <= y1; y++) {
      uint8_t* row = zb + y * zpitch;
      if (zfmt == 1) { for (uint32_t x = x0; x <= x1; x++) ((uint16_t*)row)[x] = (uint16_t)zc; }
      else { for (uint32_t x = x0; x <= x1; x++) ((uint32_t*)row)[x] = zc; }
    }
  }
  if ((mask & 0xF0) && g_dmaColor) {
    uint32_t lim; uint8_t* base = Nv2aDmaMap(g_dmaColor, &lim);
    uint32_t pitch = R(M_SURFACE_PITCH) & 0xFFFF, off = R(M_SURFACE_COLOR_OFFSET), bpp = ColorBytes(colorFmt);
    uint32_t c = R(M_COLOR_CLEAR);
    static int log = 3;
    if (log-- > 0) Log("[pgraph] CLEAR color mask=0x%x value=0x%08x rect=(%u,%u)-(%u,%u) fmt=%u pitch=%u off=0x%x type=%u", mask, c, x0, y0, x1, y1, colorFmt, pitch, off, (fmt >> 8) & 0xF);
    if (((fmt >> 8) & 0xF) == 1) {  // pitch (linear) surfaces only for now; swizzled render targets come with the texture/swizzle code
      for (uint32_t y = y0; y <= y1; y++) {
        uint8_t* row = base + off + y * pitch;
        for (uint32_t x = x0; x <= x1; x++) {
          if (bpp == 4) *(uint32_t*)(row + x * 4) = c;
          else if (bpp == 2) { uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF; *(uint16_t*)(row + x * 2) = (colorFmt == 3) ? (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)) : (uint16_t)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)); }
        }
      }
    }
  }
}

void InitCur() {
  if (g_curInit) return; g_curInit = true;
  for (int i = 0; i < 16; i++) { g_cur.a[i][0] = g_cur.a[i][1] = g_cur.a[i][2] = 0; g_cur.a[i][3] = 1; }
  for (int k = 0; k < 4; k++) g_cur.a[3][k] = 1;   // diffuse defaults to white
}
void Emit() { if (g_prim) g_verts.push_back(g_cur); }
float F(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

int TypeBytes(uint32_t type) { switch (type) { case 2: return 4; case 0: case 4: return 1; case 1: case 5: return 2; case 6: return 4; default: return 4; } }
// Fetch attribute 'i' from memory at p according to its array format (F, UB, S1, S32K, CMP)
void FetchAttr(int i, const uint8_t* p, float* o) {
  uint32_t f = R(0x1760 + 4 * i), type = f & 15, size = (f >> 4) & 15;
  o[0] = o[1] = o[2] = 0; o[3] = 1;
  if (type == 6) { uint32_t v = *(const uint32_t*)p; int x = (int)(v & 0x7FF), y = (int)((v >> 11) & 0x7FF), z = (int)(v >> 22); if (x & 0x400) x -= 0x800; if (y & 0x400) y -= 0x800; if (z & 0x200) z -= 0x400; o[0] = x / 1023.f; o[1] = y / 1023.f; o[2] = z / 511.f; return; }
  for (uint32_t c = 0; c < size && c < 4; c++) {
    switch (type) {
      case 2: o[c] = *(const float*)(p + 4 * c); break;
      case 0: { static const int ord[4] = {2, 1, 0, 3}; o[c] = p[ord[c]] / 255.f; break; }       // D3DCOLOR: B G R A in memory -> r g b a
      case 4: o[c] = p[c] / 255.f; break;
      case 1: o[c] = *(const int16_t*)(p + 2 * c) / 32767.f; break;
      case 5: o[c] = (float)*(const int16_t*)(p + 2 * c); break;
      default: break;
    }
  }
}
// Per-draw vertex array setup, hoisted out of the per-vertex loop (register reads + DMA object lookups were ~40% of pusher time)
struct ArrAttr { int i; uint32_t type, size, stride; const uint8_t* base; };
ArrAttr g_arrs[16]; int g_nArrs = 0;
void PrepArrays() {
  g_nArrs = 0;
  for (int i = 0; i < 16; i++) {
    uint32_t f = R(0x1760 + 4 * i), size = (f >> 4) & 15, off = R(0x1720 + 4 * i); if (!size) continue;
    uint32_t dma = (off & 0x80000000u) ? g_dmaVtxB : g_dmaVtxA; if (!dma) continue;
    uint32_t lim; uint8_t* base = Nv2aDmaMap(dma, &lim);
    g_arrs[g_nArrs++] = {i, f & 15, size, f >> 8, base + (off & 0x7FFFFFFF)};
  }
  { static const bool lg = getenv("XBOX_ARRLOG") != nullptr; static std::set<uint64_t> seen; if (lg) { uint64_t h = 1469598103934665603ull; for (int k = 0; k < g_nArrs; k++) h = (h ^ (g_arrs[k].i | (g_arrs[k].type << 8) | (g_arrs[k].size << 12) | ((uint64_t)g_arrs[k].stride << 16))) * 1099511628211ull; if (seen.insert(h).second && seen.size() < 80) { char b[600] = ""; for (int k = 0; k < g_nArrs; k++) { char t[40]; snprintf(t, sizeof t, " a%d:t%u,s%u,st%u", g_arrs[k].i, g_arrs[k].type, g_arrs[k].size, g_arrs[k].stride); strcat(b, t); } Log("[arr]%s", b); } } }
}
void FetchAttrFast(const ArrAttr& a, const uint8_t* p, float* o) {
  o[0] = o[1] = o[2] = 0; o[3] = 1;
  if (a.type == 6) { uint32_t v = *(const uint32_t*)p; int x = (int)(v & 0x7FF), y = (int)((v >> 11) & 0x7FF), z = (int)(v >> 22); if (x & 0x400) x -= 0x800; if (y & 0x400) y -= 0x800; if (z & 0x200) z -= 0x400; o[0] = x / 1023.f; o[1] = y / 1023.f; o[2] = z / 511.f; return; }
  switch (a.type) {
    case 2: for (uint32_t c = 0; c < a.size && c < 4; c++) o[c] = *(const float*)(p + 4 * c); break;
    case 0: { static const int ord[4] = {2, 1, 0, 3}; for (uint32_t c = 0; c < a.size && c < 4; c++) o[c] = p[ord[c]] / 255.f; break; }
    case 4: for (uint32_t c = 0; c < a.size && c < 4; c++) o[c] = p[c] / 255.f; break;
    case 1: for (uint32_t c = 0; c < a.size && c < 4; c++) o[c] = *(const int16_t*)(p + 2 * c) / 32767.f; break;
    case 5: for (uint32_t c = 0; c < a.size && c < 4; c++) o[c] = (float)*(const int16_t*)(p + 2 * c); break;
    default: break;
  }
}
inline void FetchVertexInto(uint32_t index, SoftVertex& v) {
  v = g_cur;
  for (int k = 0; k < g_nArrs; k++) { const ArrAttr& a = g_arrs[k]; FetchAttrFast(a, a.base + (size_t)index * a.stride, v.a[a.i]); }
}
// Inline array: attributes are packed back to back per vertex in the order of the enabled array formats
void BuildInline() {
  uint32_t stride = 0; for (int i = 0; i < 16; i++) { uint32_t f = R(0x1760 + 4 * i), size = (f >> 4) & 15; if (size) stride += size * TypeBytes(f & 15) / ((f & 15) == 6 ? size : 1); }
  if (!stride) return;
  const uint8_t* p = (const uint8_t*)g_inline.data(); size_t total = g_inline.size() * 4;
  for (size_t at = 0; at + stride <= total; at += stride) {
    SoftVertex v = g_cur; size_t q = at;
    for (int i = 0; i < 16; i++) { uint32_t f = R(0x1760 + 4 * i), size = (f >> 4) & 15; if (!size) continue; FetchAttr(i, p + q, v.a[i]); q += (f & 15) == 6 ? 4 : size * TypeBytes(f & 15); }
    g_verts.push_back(v);
  }
}
void EndDraw() {
  InitCur();
  if (g_verts.empty()) {
    if (!g_inline.empty()) BuildInline();
    else if (!g_elems.empty()) { PrepArrays(); size_t n0 = g_verts.size(); g_verts.resize(n0 + g_elems.size()); for (size_t k = 0; k < g_elems.size(); k++) FetchVertexInto(g_elems[k], g_verts[n0 + k]); }
    else { PrepArrays(); for (auto& r : g_ranges) { size_t n0 = g_verts.size(); g_verts.resize(n0 + r.second); for (uint32_t i = 0; i < r.second; i++) FetchVertexInto(r.first + i, g_verts[n0 + i]); } }
  }
  if (!g_verts.empty()) { if (GpuVkReady()) GpuDraw(g_prim, g_verts.data(), g_verts.size()); else SoftDraw(g_prim, g_verts.data(), g_verts.size()); }
}

void DrawLog() {
  ++g_draws;
  static int logged = 0; if (logged >= 40) return; ++logged;
  uint32_t mode = R(M_EXEC_MODE);
  char fmts[400] = ""; for (int i = 0; i < 16; i++) { uint32_t f = R(0x1760 + 4 * i); if (f & 0xF0 || (f & 0xF) != 0 || R(0x1720 + 4 * i)) { char t[40]; sprintf(t, " a%d[ty%u sz%u st%u off%x]", i, f & 0xF, (f >> 4) & 0xF, f >> 8, R(0x1720 + 4 * i)); strcat(fmts, t); } }
  Log("[draw] #%llu prim=%u mode=0x%x progStart=%u inline=%zu elems=%zu draw(start=%u,count=%u) surf=%08x clip=%08x/%08x%s", (unsigned long long)g_draws, g_prim, mode, R(M_PROG_START), g_inline.size(), g_elems.size(), g_drawStart, g_drawCount, R(M_SURFACE_FORMAT), R(M_SURFACE_CLIP_H), R(M_SURFACE_CLIP_V), fmts);
  for (int t = 0; t < 4; t++) { uint32_t fmt = R(0x1B04 + 64 * t), off = R(0x1B00 + 64 * t), ctl = R(0x1B0C + 64 * t); if (fmt || off) Log("[draw]   tex%d offset=0x%x format=0x%08x control0=0x%08x filter=0x%08x rect=0x%08x", t, off, fmt, ctl, R(0x1B14 + 64 * t), R(0x1B1C + 64 * t)); }
  Log("[draw]   combiners: count/flags=0x%08x icw0=%08x ocw0=%08x alphaIcw0=%08x blend=%u(%08x/%08x) depth=%u alphatest=%u", R(0x1E60), R(0xAC0), R(0xAA0), R(0x260), R(0x304), R(0x344), R(0x348), R(0x30C), R(0x300));
}
}  // namespace

// Returns true if the method was consumed here.
bool Pgraph3dMethod(uint32_t method, uint32_t param) {
  if (method >= 0x2000) return false;
  R(method) = param;
  if (method >= 0xB00 && method < 0xB80) { if (g_progLoad < 136 * 4) g_prog[g_progLoad] = param; ++g_progLoad; return true; }          // SET_TRANSFORM_PROGRAM: auto-increment load pointer
  if (method >= 0xB80 && method < 0xC00) { if (g_constLoad < 192 * 4) g_const[g_constLoad] = param; ++g_constLoad; return true; }       // SET_TRANSFORM_CONSTANT
  // fixed-function transform context (XFCTX rows live in g_const below the program-constant range, c[0..95])
  if (method >= 0x440 && method < 0x900) {
    auto put = [&](int row, int e) { g_const[row * 4 + e] = param; };
    if (method < 0x480) { int sl = (method - 0x440) / 4; put(0x04 + sl / 4, sl % 4); }
    else if (method < 0x580) { int sl = (method - 0x480) / 4; put(0x08 + (sl / 16) * 8 + (sl % 16) / 4, sl % 4); }
    else if (method < 0x680) { int sl = (method - 0x580) / 4; put(0x0C + (sl / 16) * 8 + (sl % 16) / 4, sl % 4); }
    else if (method < 0x6C0) { int sl = (method - 0x680) / 4; put(sl / 4, sl % 4); }
    else if (method < 0x7C0) { int sl = (method - 0x6C0) / 4; put(0x44 + (sl / 16) * 8 + (sl % 16) / 4, sl % 4); }
    else if (method >= 0x840 && method < 0x8C0) { int sl = (method - 0x840) / 4; put(0x40 + (sl / 16) * 8 + (sl % 16) / 4, sl % 4); }
  }
  if (method >= 0xA20 && method < 0xA30) g_const[0x3B * 4 + (method - 0xA20) / 4] = param;
  if (method >= 0xAF0 && method < 0xB00) g_const[0x3A * 4 + (method - 0xAF0) / 4] = param;
  InitCur();
  if (method >= 0x1500 && method < 0x1510) { int sl = (method - 0x1500) / 4; g_cur.a[0][sl] = F(param); g_cur.a[0][3] = 1; if (sl == 2) Emit(); return true; }
  if (method >= 0x1518 && method < 0x1528) { int sl = (method - 0x1518) / 4; g_cur.a[0][sl] = F(param); if (sl == 3) Emit(); return true; }
  if (method >= 0x1880 && method < 0x1900) { int sl = (method - 0x1880) / 4, at = sl / 2, part = sl % 2; g_cur.a[at][part] = F(param); g_cur.a[at][2] = 0; g_cur.a[at][3] = 1; if (at == 0 && part == 1) Emit(); return true; }
  if (method >= 0x1900 && method < 0x1940) { int at = (method - 0x1900) / 4; g_cur.a[at][0] = (float)(int16_t)(param & 0xFFFF); g_cur.a[at][1] = (float)(int16_t)(param >> 16); g_cur.a[at][2] = 0; g_cur.a[at][3] = 1; if (at == 0) Emit(); return true; }
  if (method >= 0x1940 && method < 0x1980) { int at = (method - 0x1940) / 4; for (int k = 0; k < 4; k++) g_cur.a[at][k] = ((param >> (8 * k)) & 255) / 255.f; if (at == 0) Emit(); return true; }
  if (method >= 0x1980 && method < 0x1A00) { int sl = (method - 0x1980) / 4, at = sl / 2, part = sl % 2; g_cur.a[at][part * 2] = (float)(int16_t)(param & 0xFFFF); g_cur.a[at][part * 2 + 1] = (float)(int16_t)(param >> 16); if (at == 0 && part == 1) Emit(); return true; }
  if (method >= 0x1A00 && method < 0x1B00) { int sl = (method - 0x1A00) / 4, at = sl / 4, part = sl % 4; g_cur.a[at][part] = F(param); if (at == 0 && part == 3) Emit(); return true; }
  switch (method) {
    case M_CONTEXT_DMA_COLOR: g_dmaColor = param; return true;   // handle already translated to a RAMIN address by the puller
    case M_CONTEXT_DMA_ZETA: g_dmaZeta = param; return true;
    case 0x184: g_dmaTexA = param; return true;
    case 0x188: g_dmaTexB = param; return true;
    case 0x19C: g_dmaVtxA = param; return true;
    case 0x1A0: g_dmaVtxB = param; return true;
    case M_CLEAR_SURFACE: ClearSurface(param); return true;
    case M_BEGIN_END: if (param) { InitCur(); g_prim = param; g_inline.clear(); g_elems.clear(); g_verts.clear(); g_ranges.clear(); g_drawStart = g_drawCount = 0; } else if (g_prim) { DrawLog(); EndDraw(); g_prim = 0; } return true;
    case M_ELEM16: g_elems.push_back(param & 0xFFFF); g_elems.push_back(param >> 16); return true;
    case M_ELEM32: g_elems.push_back(param); return true;
    case M_DRAW_ARRAYS: g_drawStart = param & 0xFFFFFF; g_drawCount = (param >> 24) + 1; g_ranges.push_back({g_drawStart, g_drawCount}); return true;
    case M_INLINE_ARRAY: g_inline.push_back(param); return true;
    case M_PROG_LOAD: g_progLoad = param * 4; return true;       // slot index -> dword index
    case M_CONST_LOAD: g_constLoad = param * 4; return true;     // constant index -> dword index
    case M_SURFACE_CLIP_H: case M_SURFACE_CLIP_V: case M_SURFACE_FORMAT: case M_SURFACE_PITCH: case M_SURFACE_COLOR_OFFSET: case M_SURFACE_ZETA_OFFSET:
    case M_ZSTENCIL_CLEAR: case M_COLOR_CLEAR: case M_CLEAR_RECT_H: case M_CLEAR_RECT_V: return true;
  }
  return false;
}
uint32_t Pgraph3dState(uint32_t method) { return R(method); }
void PgraphDump() { Log("[pgraph] %llu clears, %llu draws", (unsigned long long)g_clears, (unsigned long long)g_draws); }

const uint32_t* Pgraph3dProgram() { return g_prog; }
const float* Pgraph3dConstants() { return (const float*)g_const; }
uint32_t Pgraph3dDmaInstance(int which) { switch (which) { case 0: return g_dmaColor; case 1: return g_dmaZeta; case 2: return g_dmaTexA; case 3: return g_dmaTexB; case 4: return g_dmaVtxA; default: return g_dmaVtxB; } }

// Called when the title swaps buffers (FLIP_STALL): snapshot the colour surface it just finished so the display never shows a half-drawn frame.
void PgraphFlip() {
  if (GpuVkReady()) { GpuFlip(); return; }
  { static uint64_t lastDraws = 0; static uint32_t lastT = (uint32_t)plat::TickMs(); static int n = 0; uint32_t now = (uint32_t)plat::TickMs();
    if (getenv("XBOX_FRAMELOG") && (n++ % 5) == 0) Log("[frame] flip #%d: %llu draws since last flip, %lu ms since last flip", n, (unsigned long long)(g_draws - lastDraws), now - lastT);
    lastDraws = g_draws; lastT = now; }
  uint32_t dma = g_dmaColor; if (!dma) return;
  uint32_t fmt = R(M_SURFACE_FORMAT), cf = fmt & 0xF; if (((fmt >> 8) & 0xF) != 1) return;
  uint32_t ch = R(M_SURFACE_CLIP_H), cv = R(M_SURFACE_CLIP_V);
  int w = ch >> 16, h = cv >> 16; if (w <= 0 || h <= 0 || w > 1920 || h > 1080) return;
  int x0 = ch & 0xFFFF, y0 = cv & 0xFFFF;
  uint32_t lim; const uint8_t* base = Nv2aDmaMap(dma, &lim) + R(M_SURFACE_COLOR_OFFSET); uint32_t pitch = R(M_SURFACE_PITCH) & 0xFFFF; int bpp = ColorBytes(cf);
  std::vector<uint32_t> img((size_t)w * h);
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    const uint8_t* p = base + (size_t)(y + y0) * pitch + (size_t)(x + x0) * bpp;
    if (bpp == 4) img[(size_t)y * w + x] = *(const uint32_t*)p & 0xFFFFFF;
    else { uint16_t v = *(const uint16_t*)p; img[(size_t)y * w + x] = cf == 3 ? ((v >> 11) * 255 / 31) << 16 | (((v >> 5) & 63) * 255 / 63) << 8 | ((v & 31) * 255 / 31) : (((v >> 10) & 31) * 255 / 31) << 16 | (((v >> 5) & 31) * 255 / 31) << 8 | ((v & 31) * 255 / 31); }
  }
  std::lock_guard<std::mutex> l(g_frontMutex); g_front.swap(img); g_frontW = w; g_frontH = h; g_frontValid = true;
}
bool PgraphGetFront(std::vector<uint32_t>& out, int& w, int& h) {
  std::lock_guard<std::mutex> l(g_frontMutex); if (!g_frontValid) return false; out = g_front; w = g_frontW; h = g_frontH; return true;
}
