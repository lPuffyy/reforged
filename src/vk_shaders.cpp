// NV2A -> SPIR-V translators: the vertex program (microcode, straight-line) and the register combiners + final combiner (pixel pipeline).
// Semantics follow the hardware documentation (xemu's glsl/vsh-prog.c and glsl/psh.c were read as reference); the software renderer in gpu_soft.cpp is the
// executable specification these translators are checked against.
#include "vk_shaders.h"
#include "spirv_builder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace {
uint32_t Field(const uint32_t* t, int dword, int pos, int bits) { return (t[dword] >> pos) & ((1u << bits) - 1); }
}

// ======================= vertex shader =======================
std::vector<uint32_t> GenVertexShader(const uint32_t* prog, uint32_t start, uint32_t* usedInputs) {
  Spv s; uint32_t mask = 0;
  // which v registers does the program read?
  for (uint32_t pc = start; pc < 136; pc++) {
    const uint32_t* t = prog + pc * 4;
    for (int w = 0; w < 3; w++) { uint32_t mux = w == 0 ? Field(t, 2, 26, 2) : w == 1 ? Field(t, 2, 11, 2) : Field(t, 3, 28, 2); if (mux == 2) mask |= 1u << Field(t, 1, 9, 4); }
    if (Field(t, 3, 0, 1)) break;
  }
  *usedInputs = mask;
  uint32_t tF = s.TFloat(), tV4 = s.TVec4(), tI = s.TInt(true), tVoid = s.TVoid();
  // uniform block: vec4 c[192]; vec4 misc (width, height, zmax, 0)
  uint32_t arr = s.TArray(tV4, s.CUint(192)); s.Decorate(arr, Spv::D_ArrayStride, {16});
  uint32_t ub = s.TStruct({arr, tV4}); s.Decorate(ub, Spv::D_Block); s.MemberDecorate(ub, 0, Spv::D_Offset, {0}); s.MemberDecorate(ub, 1, Spv::D_Offset, {192 * 16});
  uint32_t ubVar = s.Variable(ub, Spv::Uniform); s.Decorate(ubVar, Spv::D_DescriptorSet, {0}); s.Decorate(ubVar, Spv::D_Binding, {0});
  std::vector<uint32_t> iface;
  uint32_t inVar[16] = {};
  for (int i = 0; i < 16; i++) if (mask & (1u << i)) { inVar[i] = s.Variable(tV4, Spv::Input); s.Decorate(inVar[i], Spv::D_Location, {(uint32_t)i}); iface.push_back(inVar[i]); }
  // outputs: gl_Position block + varyings
  uint32_t pv = s.TStruct({tV4}); s.Decorate(pv, Spv::D_Block); s.MemberDecorate(pv, 0, Spv::D_BuiltIn, {Spv::B_Position});
  uint32_t posOut = s.Variable(pv, Spv::Output); iface.push_back(posOut);
  uint32_t varying[7];  // d0, d1, t0..t3, fog
  for (int i = 0; i < 7; i++) { varying[i] = s.Variable(tV4, Spv::Output); s.Decorate(varying[i], Spv::D_Location, {(uint32_t)i}); iface.push_back(varying[i]); }
  // private / function state
  uint32_t o[16]; for (int i = 0; i < 16; i++) o[i] = s.Variable(tV4, Spv::Private);
  uint32_t zero4 = s.CVec4(0, 0, 0, 0), initO = s.CVec4(0, 0, 0, 1);
  s.BeginMain();
  // function-scope variables must come first in the entry block: allocate R0..R11 and a0 now
  uint32_t R[12]; for (int i = 0; i < 12; i++) { R[i] = s.Id(); Spv::Put(s.cur, Spv::OpVariable, {s.TPtr(Spv::Function, tV4), R[i], Spv::Function}); }
  uint32_t a0 = s.Id(); Spv::Put(s.cur, Spv::OpVariable, {s.TPtr(Spv::Function, tI), a0, Spv::Function});
  for (int i = 0; i < 12; i++) s.Store(R[i], zero4);
  s.Store(a0, s.CInt(0));
  for (int i = 0; i < 16; i++) s.Store(o[i], initO);
  uint32_t ptrV4U = s.TPtr(Spv::Uniform, tV4);

  auto swz = [&](uint32_t v, uint32_t sel, bool neg) {  // sel packs x,y,z,w selectors (2 bits each, x in the high bits)
    uint32_t r = s.Shuffle(tV4, v, v, {(sel >> 6) & 3, (sel >> 4) & 3, (sel >> 2) & 3, sel & 3});
    return neg ? s.Emit(Spv::OpFNegate, tV4, {r}) : r;
  };
  auto operand = [&](const uint32_t* t, int which) {
    uint32_t mux, reg, sel, neg;
    if (which == 0) { mux = Field(t, 2, 26, 2); reg = Field(t, 2, 28, 4); sel = Field(t, 1, 0, 8); neg = Field(t, 1, 8, 1); }
    else if (which == 1) { mux = Field(t, 2, 11, 2); reg = Field(t, 2, 13, 4); sel = Field(t, 2, 17, 8); neg = Field(t, 2, 25, 1); }
    else { mux = Field(t, 3, 28, 2); reg = (Field(t, 2, 0, 2) << 2) | Field(t, 3, 30, 2); sel = Field(t, 2, 2, 8); neg = Field(t, 2, 10, 1); }
    uint32_t v;
    if (mux == 1) v = s.Load(tV4, reg == 12 ? o[0] : R[reg % 12]);        // r12 reads back oPos
    else if (mux == 2) v = inVar[Field(t, 1, 9, 4)] ? s.Load(tV4, inVar[Field(t, 1, 9, 4)]) : zero4;
    else if (mux == 3) {
      uint32_t idx = s.CInt((int)Field(t, 1, 13, 8));
      if (Field(t, 3, 1, 1)) { idx = s.Emit(Spv::OpIAdd, tI, {idx, s.Load(tI, a0)}); idx = s.Glsl3(tI, Spv::G_SClamp, idx, s.CInt(0), s.CInt(191)); }
      uint32_t p = s.Emit(Spv::OpAccessChain, ptrV4U, {ubVar, s.CInt(0), idx}); v = s.Load(tV4, p);
    } else v = zero4;
    return swz(v, sel, neg != 0);
  };
  auto comp = [&](uint32_t v, uint32_t i) { return s.Extract(tF, v, i); };
  auto splat = [&](uint32_t f) { return s.Vec4(f, f, f, f); };
  auto masked = [&](uint32_t oldv, uint32_t newv, uint32_t m) {   // m: 8=x 4=y 2=z 1=w
    if ((m & 15) == 15) return newv;
    return s.Shuffle(tV4, oldv, newv, {(m & 8) ? 4u : 0u, (m & 4) ? 5u : 1u, (m & 2) ? 6u : 2u, (m & 1) ? 7u : 3u});
  };
  uint32_t one = s.CFloat(1.f), zero = s.CFloat(0.f);

  static int maxSlots = getenv("XBOX_VS_SLOTS") ? atoi(getenv("XBOX_VS_SLOTS")) : 9999;
  for (uint32_t pc = start; pc < 136; pc++) {
    if ((int)(pc - start) >= maxSlots) break;
    const uint32_t* t = prog + pc * 4;
    uint32_t mac = Field(t, 1, 21, 4), ilu = Field(t, 1, 25, 3);
    if (mac || ilu) {
      uint32_t A = operand(t, 0), B = operand(t, 1), C = operand(t, 2);
      uint32_t M = 0, I = 0; bool arl = false;
      switch (mac) {
        case 1: M = A; break;
        case 2: M = s.Emit(Spv::OpFMul, tV4, {A, B}); break;
        case 3: M = s.Emit(Spv::OpFAdd, tV4, {A, C}); break;
        case 4: M = s.Emit(Spv::OpFAdd, tV4, {s.Emit(Spv::OpFMul, tV4, {A, B}), C}); break;
        case 5: case 6: case 7: {
          uint32_t av = s.Shuffle(s.TVec(tF, 3), A, A, {0, 1, 2}), bv = s.Shuffle(s.TVec(tF, 3), B, B, {0, 1, 2});
          uint32_t d = s.Emit(Spv::OpDot, tF, {av, bv});
          if (mac == 6) d = s.Emit(Spv::OpFAdd, tF, {d, comp(B, 3)});
          if (mac == 7) d = s.Emit(Spv::OpDot, tF, {A, B});
          M = splat(d); break; }
        case 8: M = s.Vec4(one, s.Emit(Spv::OpFMul, tF, {comp(A, 1), comp(B, 1)}), comp(A, 2), comp(B, 3)); break;
        case 9: M = s.Glsl2(tV4, Spv::G_FMin, A, B); break;
        case 10: M = s.Glsl2(tV4, Spv::G_FMax, A, B); break;
        case 11: case 12: {
          uint32_t op = mac == 11 ? (uint32_t)Spv::OpFOrdLessThan : (uint32_t)Spv::OpFOrdGreaterThanEqual;   // on vectors these yield a bvec4
          uint32_t cm = s.Emit((uint16_t)op, s.TVec(s.TBool(), 4), {A, B});
          M = s.Emit(Spv::OpSelect, tV4, {cm, s.CVec4(1, 1, 1, 1), zero4}); break; }
        case 13: arl = true; break;
      }
      switch (ilu) {
        case 1: I = C; break;
        case 2: { uint32_t x = comp(C, 0); I = splat(s.Emit(Spv::OpFDiv, tF, {one, x})); break; }
        case 3: { uint32_t x = comp(C, 0); uint32_t r = s.Emit(Spv::OpFDiv, tF, {one, x}); I = splat(s.Glsl3(tF, Spv::G_FClamp, r, s.CFloat(-1.8446744e19f), s.CFloat(1.8446744e19f))); break; }
        case 4: { uint32_t x = s.Glsl1(tF, Spv::G_FAbs, comp(C, 0)); I = splat(s.Glsl1(tF, Spv::G_InverseSqrt, x)); break; }
        case 5: { uint32_t x = comp(C, 0), f = s.Glsl1(tF, Spv::G_Floor, x); I = s.Vec4(s.Glsl1(tF, Spv::G_Exp2, f), s.Emit(Spv::OpFSub, tF, {x, f}), s.Glsl1(tF, Spv::G_Exp2, x), one); break; }
        case 6: { uint32_t x = s.Glsl1(tF, Spv::G_FAbs, comp(C, 0)), l = s.Glsl1(tF, Spv::G_Log2, x), e = s.Glsl1(tF, Spv::G_Floor, l); uint32_t m = s.Emit(Spv::OpFDiv, tF, {x, s.Glsl1(tF, Spv::G_Exp2, e)}); I = s.Vec4(e, m, l, one); break; }
        case 7: {
          uint32_t x = s.Glsl2(tF, Spv::G_FMax, comp(C, 0), zero), y = s.Glsl2(tF, Spv::G_FMax, comp(C, 1), zero), w = s.Glsl3(tF, Spv::G_FClamp, comp(C, 3), s.CFloat(-127.9961f), s.CFloat(127.9961f));
          uint32_t p = s.Glsl2(tF, Spv::G_Pow, y, w); uint32_t gt = s.Emit(Spv::OpFOrdGreaterThan, s.TBool(), {x, zero}); uint32_t z = s.Emit(Spv::OpSelect, tF, {gt, p, zero});
          I = s.Vec4(one, x, z, one); break; }
      }
      uint32_t outR = Field(t, 3, 20, 4), outMux = Field(t, 3, 2, 1), oMask = Field(t, 3, 12, 4), orb = Field(t, 3, 11, 1), addr = Field(t, 3, 3, 8);
      uint32_t macMask = Field(t, 3, 24, 4), iluMask = Field(t, 3, 16, 4); bool paired = mac && ilu;
      if (mac) {
        if (arl) { uint32_t f = s.Glsl1(tF, Spv::G_Floor, comp(A, 0)); s.Store(a0, s.Emit(Spv::OpConvertFToS, tI, {f})); }
        else if (!(paired && outR == 1) && macMask) { uint32_t reg = R[outR % 12]; s.Store(reg, masked(s.Load(tV4, reg), M, macMask)); }
      }
      if (ilu && iluMask) { uint32_t reg = R[(paired ? 1 : outR) % 12]; s.Store(reg, masked(s.Load(tV4, reg), I, iluMask)); }
      if (oMask && orb == 1) {
        uint32_t v = outMux == 0 ? M : I;
        if (v && (outMux == 0 ? mac != 0 && !arl : ilu != 0)) { uint32_t dst = o[addr & 0xF]; s.Store(dst, masked(s.Load(tV4, dst), v, oMask)); }
      }
    }
    if (Field(t, 3, 0, 1)) break;
  }
  // epilogue: screen-space oPos -> clip space (the shaders already divided by w); hardware varyings
  uint32_t pos = s.Load(tV4, o[0]);
  uint32_t misc = s.Load(tV4, s.Emit(Spv::OpAccessChain, ptrV4U, {ubVar, s.CInt(1)}));
  uint32_t W = comp(misc, 0), H = comp(misc, 1), ZM = comp(misc, 2), two = s.CFloat(2.f);
  uint32_t px = comp(pos, 0), py = comp(pos, 1), pz = comp(pos, 2), pw = comp(pos, 3);
  uint32_t isz = s.Emit(Spv::OpFOrdEqual, s.TBool(), {pw, zero}); pw = s.Emit(Spv::OpSelect, tF, {isz, one, pw});
  uint32_t cx = s.Emit(Spv::OpFDiv, tF, {s.Emit(Spv::OpFSub, tF, {s.Emit(Spv::OpFMul, tF, {two, px}), W}), W});
  uint32_t cy = s.Emit(Spv::OpFDiv, tF, {s.Emit(Spv::OpFSub, tF, {s.Emit(Spv::OpFMul, tF, {two, py}), H}), H});
  uint32_t cz = s.Emit(Spv::OpFDiv, tF, {pz, ZM});
  { static const bool margin = getenv("XBOX_NO_ZMARGIN") == nullptr;   // keep depth strictly inside the clip volume: screen-space draws sit exactly at z = 0 / zmax and a GPU that clips on the boundary would drop them
    if (margin) cz = s.Emit(Spv::OpFAdd, tF, {s.Emit(Spv::OpFMul, tF, {cz, s.CFloat(0.99998f)}), s.CFloat(0.00001f)}); }
  uint32_t clip = s.Vec4(s.Emit(Spv::OpFMul, tF, {cx, pw}), s.Emit(Spv::OpFMul, tF, {cy, pw}), s.Emit(Spv::OpFMul, tF, {cz, pw}), pw);
  uint32_t ptrOutV4 = s.TPtr(Spv::Output, tV4);
  s.Store(s.Emit(Spv::OpAccessChain, ptrOutV4, {posOut, s.CInt(0)}), clip);
  s.Store(varying[0], s.Load(tV4, o[3]));  // oD0
  s.Store(varying[1], s.Load(tV4, o[4]));  // oD1
  for (int i = 0; i < 4; i++) s.Store(varying[2 + i], s.Load(tV4, o[9 + i]));  // oT0..oT3
  s.Store(varying[6], s.Load(tV4, o[5]));  // oFog
  s.EndMain();
  s.Header(false, iface);
  return s.Finish();
}

// ======================= fragment shader =======================
std::vector<uint32_t> GenFragmentShader(const FsInfo& f) {
  static const char* dbgOnly = getenv("XBOX_FS_DBGSTAGES");   // debug views (XBOX_FS_DEBUG*) apply only to shaders with this many combiner stages
  const bool dbgOn = !dbgOnly || (uint32_t)atoi(dbgOnly) == f.numStages;
  Spv s;
  uint32_t tF = s.TFloat(), tV4 = s.TVec4(), tV3 = s.TVec(tF, 3), tBool = s.TBool();
  // uniforms: fac0[8], fac1[8], final0, final1, fogColor, texSize[4] (xy), alphaRef
  uint32_t arr8 = s.TArray(tV4, s.CUint(8)); s.Decorate(arr8, Spv::D_ArrayStride, {16});
  uint32_t arr4 = s.TArray(tV4, s.CUint(4)); s.Decorate(arr4, Spv::D_ArrayStride, {16});
  uint32_t ub = s.TStruct({arr8, arr8, tV4, tV4, tV4, arr4, tV4}); s.Decorate(ub, Spv::D_Block);
  uint32_t offs[7] = {0, 128, 256, 272, 288, 304, 368}; for (int i = 0; i < 7; i++) s.MemberDecorate(ub, i, Spv::D_Offset, {offs[i]});
  uint32_t ubVar = s.Variable(ub, Spv::Uniform); s.Decorate(ubVar, Spv::D_DescriptorSet, {0}); s.Decorate(ubVar, Spv::D_Binding, {5});
  uint32_t tSamp = s.TSampled2D(), tSampCube = s.TSampledCube(); uint32_t sampVar[4] = {};
  std::vector<uint32_t> iface;
  for (int i = 0; i < 4; i++) { sampVar[i] = s.Variable(f.texCube[i] ? tSampCube : tSamp, Spv::UniformConstant); s.Decorate(sampVar[i], Spv::D_DescriptorSet, {0}); s.Decorate(sampVar[i], Spv::D_Binding, {(uint32_t)(1 + i)}); }
  uint32_t inD0 = s.Variable(tV4, Spv::Input), inD1 = s.Variable(tV4, Spv::Input), inT[4], inFog = s.Variable(tV4, Spv::Input);
  s.Decorate(inD0, Spv::D_Location, {0}); s.Decorate(inD1, Spv::D_Location, {1});
  for (int i = 0; i < 4; i++) { inT[i] = s.Variable(tV4, Spv::Input); s.Decorate(inT[i], Spv::D_Location, {(uint32_t)(2 + i)}); iface.push_back(inT[i]); }
  s.Decorate(inFog, Spv::D_Location, {6}); iface.push_back(inD0); iface.push_back(inD1); iface.push_back(inFog);
  uint32_t outCol = s.Variable(tV4, Spv::Output); s.Decorate(outCol, Spv::D_Location, {0}); iface.push_back(outCol);
  uint32_t ptrU = s.TPtr(Spv::Uniform, tV4);
  s.BeginMain();
  uint32_t zero4 = s.CVec4(0, 0, 0, 0), one4 = s.CVec4(1, 1, 1, 1);
  uint32_t reg[16]; for (int i = 0; i < 16; i++) { reg[i] = s.Id(); Spv::Put(s.cur, Spv::OpVariable, {s.TPtr(Spv::Function, tV4), reg[i], Spv::Function}); }
  for (int i = 0; i < 16; i++) s.Store(reg[i], zero4);
  auto comp = [&](uint32_t v, uint32_t i) { return s.Extract(tF, v, i); };
  auto sat4 = [&](uint32_t v) { return s.Glsl3(tV4, Spv::G_FClamp, v, zero4, one4); };
  auto uniformVec = [&](uint32_t member, uint32_t idx) { uint32_t p = idx == ~0u ? s.Emit(Spv::OpAccessChain, ptrU, {ubVar, s.CInt((int)member)}) : s.Emit(Spv::OpAccessChain, ptrU, {ubVar, s.CInt((int)member), s.CInt((int)idx)}); return s.Load(tV4, p); };
  // inputs: v0 / v1 colours are clamped to [0,1]; fog colour is a uniform
  s.Store(reg[4], sat4(s.Load(tV4, inD0))); s.Store(reg[5], sat4(s.Load(tV4, inD1))); s.Store(reg[3], uniformVec(4, ~0u));
  uint32_t one = s.CFloat(1.f), zero = s.CFloat(0.f), half = s.CFloat(0.5f);
  // texture stages
  for (int i = 0; i < 4; i++) {
    if (getenv("XBOX_FS_NOTEX") || !f.texPresent[i] || f.texMode[i] == 0) continue;
    uint32_t tc = s.Load(tV4, inT[i]);
    uint32_t uv = s.Shuffle(s.TVec(tF, 2), tc, tc, {0, 1});
    if (f.texMode[i] == 1) { uint32_t q = comp(tc, 3); uint32_t qz = s.Emit(Spv::OpFOrdEqual, tBool, {q, zero}); q = s.Emit(Spv::OpSelect, tF, {qz, one, q}); uint32_t qv = s.Emit(Spv::OpCompositeConstruct, s.TVec(tF, 2), {q, q}); uv = s.Emit(Spv::OpFDiv, s.TVec(tF, 2), {uv, qv}); }
    if (f.texLinear[i]) { uint32_t sz = uniformVec(5, (uint32_t)i); uint32_t sz2 = s.Shuffle(s.TVec(tF, 2), sz, sz, {0, 1}); uv = s.Emit(Spv::OpFDiv, s.TVec(tF, 2), {uv, sz2}); }
    uint32_t smp;
    if (f.texCube[i]) { uint32_t dir = s.Shuffle(tV3, tc, tc, {0, 1, 2}); uint32_t imgc = s.Load(tSampCube, sampVar[i]); smp = s.Emit(Spv::OpImageSampleImplicitLod, tV4, {imgc, dir}); }   // cube map: (s,t,r) is the lookup direction
    else { uint32_t img = s.Load(tSamp, sampVar[i]); smp = s.Emit(Spv::OpImageSampleImplicitLod, tV4, {img, uv}); }
    // Dot-product (bump / reflection) stage modes: stages 1..3 take the dot product of their texture coordinate with the signed-expanded colour of stage 0 (a normal map).
    // 9 DOT_ST: 2D lookup at (dot of stage i-1, dot of stage i); 11 DOT_RFLCT_DIFF: cube lookup along the normal N = (dot1, dot2, dot3);
    // 12 DOT_RFLCT_SPEC: cube lookup along the reflection of the eye vector E = (q1, q2, q3) about N.  (17 DOTPRODUCT stages only feed the later ones.)
    static const bool dotModes = getenv("XBOX_DOTMODES") != nullptr;   // off by default: on the phone it produced black blocky patches on floors and fixed nothing visible; kept for further work
    if (dotModes && i >= 1 && (f.texMode[i] == 9 || f.texMode[i] == 11 || f.texMode[i] == 12)) {
      uint32_t t0 = s.Load(tV4, reg[8]), rgb0 = s.Shuffle(tV3, t0, t0, {0, 1, 2});
      uint32_t n0 = s.Emit(Spv::OpFSub, tV3, {s.Emit(Spv::OpFMul, tV3, {rgb0, s.CVec(3, s.CFloat(2.f))}), s.CVec(3, one)});
      auto dotAt = [&](int k) { uint32_t tk = s.Load(tV4, inT[k]); return s.Emit(Spv::OpDot, tF, {s.Shuffle(tV3, tk, tk, {0, 1, 2}), n0}); };
      if (f.texMode[i] == 9) {
        uint32_t st = s.Emit(Spv::OpCompositeConstruct, s.TVec(tF, 2), {dotAt(i - 1), dotAt(i)});
        smp = s.Emit(Spv::OpImageSampleImplicitLod, tV4, {s.Load(tSamp, sampVar[i]), st});
      } else if (f.texCube[i]) {
        uint32_t nrm = s.Emit(Spv::OpCompositeConstruct, tV3, {dotAt(1), dotAt(2), dotAt(3)}), dir = nrm;
        if (f.texMode[i] == 12) {
          uint32_t q1 = comp(s.Load(tV4, inT[1]), 3), q2 = comp(s.Load(tV4, inT[2]), 3), q3 = comp(s.Load(tV4, inT[3]), 3);
          uint32_t eye = s.Emit(Spv::OpCompositeConstruct, tV3, {q1, q2, q3});
          uint32_t ne = s.Emit(Spv::OpDot, tF, {nrm, eye}), nn = s.Emit(Spv::OpDot, tF, {nrm, nrm});
          uint32_t k = s.Emit(Spv::OpFDiv, tF, {s.Emit(Spv::OpFMul, tF, {s.CFloat(2.f), ne}), s.Glsl2(tF, Spv::G_FMax, nn, s.CFloat(1e-6f))});
          dir = s.Emit(Spv::OpFSub, tV3, {s.Emit(Spv::OpVectorTimesScalar, tV3, {nrm, k}), eye});
        }
        smp = s.Emit(Spv::OpImageSampleImplicitLod, tV4, {s.Load(tSampCube, sampVar[i]), dir});
      }
    }
    if (f.texDepth[i]) {   // depth image sample .x in [0,1] -> 24-bit integer -> Xbox Z24S8-as-ARGB8 byte channels
      uint32_t d = comp(smp, 0), k24 = s.CFloat(16777215.f), k65536 = s.CFloat(65536.f), k256 = s.CFloat(256.f), k255 = s.CFloat(255.f);
      uint32_t z = s.Glsl1(tF, Spv::G_Floor, s.Emit(Spv::OpFAdd, tF, {s.Emit(Spv::OpFMul, tF, {d, k24}), half}));
      uint32_t a = s.Glsl1(tF, Spv::G_Floor, s.Emit(Spv::OpFDiv, tF, {z, k65536}));
      uint32_t rem = s.Emit(Spv::OpFSub, tF, {z, s.Emit(Spv::OpFMul, tF, {a, k65536})});
      uint32_t r = s.Glsl1(tF, Spv::G_Floor, s.Emit(Spv::OpFDiv, tF, {rem, k256}));
      uint32_t g = s.Emit(Spv::OpFSub, tF, {rem, s.Emit(Spv::OpFMul, tF, {r, k256})});
      smp = s.Vec4(s.Emit(Spv::OpFDiv, tF, {r, k255}), s.Emit(Spv::OpFDiv, tF, {g, k255}), zero, s.Emit(Spv::OpFDiv, tF, {a, k255}));
    }
    s.Store(reg[8 + i], smp);
  }
  // ---- combiner helpers ----
  auto mapIn = [&](uint32_t v /*vec3 or float*/, uint32_t mode, bool isVec) {
    uint32_t t = isVec ? tV3 : tF; uint32_t zero_ = isVec ? s.CVec(3, zero) : zero, one_ = isVec ? s.CVec(3, one) : one, two_ = isVec ? s.CVec(3, s.CFloat(2.f)) : s.CFloat(2.f), half_ = isVec ? s.CVec(3, half) : half;
    uint32_t x = s.Glsl2(t, Spv::G_FMax, v, zero_);
    switch (mode >> 5) {
      case 0: return x;
      case 1: return s.Emit(Spv::OpFSub, t, {one_, x});
      case 2: return s.Emit(Spv::OpFSub, t, {s.Emit(Spv::OpFMul, t, {two_, x}), one_});
      case 3: return s.Emit(Spv::OpFSub, t, {one_, s.Emit(Spv::OpFMul, t, {two_, x})});
      case 4: return s.Emit(Spv::OpFSub, t, {x, half_});
      case 5: return s.Emit(Spv::OpFSub, t, {half_, x});
      case 6: return v;
      default: return s.Emit(Spv::OpFNegate, t, {v});
    }
  };
  auto fetchRgb = [&](uint32_t in) {   // vec3
    uint32_t r = in & 15, alphaSel = (in >> 4) & 1, mode = in & 0xE0;
    uint32_t src = r == 0 ? zero4 : s.Load(tV4, reg[r]); uint32_t v;
    if (alphaSel) { uint32_t a = comp(src, 3); v = s.Emit(Spv::OpCompositeConstruct, tV3, {a, a, a}); } else v = s.Shuffle(tV3, src, src, {0, 1, 2});
    return mapIn(v, mode, true);
  };
  auto fetchAlpha = [&](uint32_t in) {  // scalar: blue (alphaSel=0) or alpha
    uint32_t r = in & 15, alphaSel = (in >> 4) & 1, mode = in & 0xE0;
    uint32_t src = r == 0 ? zero4 : s.Load(tV4, reg[r]); return mapIn(comp(src, alphaSel ? 3 : 2), mode, false);
  };
  auto postV3 = [&](uint32_t v, uint32_t mapping) {
    uint32_t t = tV3; uint32_t c;
    auto k = [&](float x) { return s.CVec(3, s.CFloat(x)); };
    switch (mapping) { case 0x08: v = s.Emit(Spv::OpFSub, t, {v, k(0.5f)}); break; case 0x10: v = s.Emit(Spv::OpFMul, t, {v, k(2.f)}); break;
      case 0x18: c = s.Emit(Spv::OpFSub, t, {v, k(0.5f)}); v = s.Emit(Spv::OpFMul, t, {c, k(2.f)}); break; case 0x20: v = s.Emit(Spv::OpFMul, t, {v, k(4.f)}); break;
      case 0x30: v = s.Emit(Spv::OpFMul, t, {v, k(0.5f)}); break; default: break; }
    return s.Glsl3(t, Spv::G_FClamp, v, k(-1.f), k(1.f));
  };
  auto postF = [&](uint32_t v, uint32_t mapping) {
    switch (mapping) { case 0x08: v = s.Emit(Spv::OpFSub, tF, {v, half}); break; case 0x10: v = s.Emit(Spv::OpFMul, tF, {v, s.CFloat(2.f)}); break;
      case 0x18: v = s.Emit(Spv::OpFMul, tF, {s.Emit(Spv::OpFSub, tF, {v, half}), s.CFloat(2.f)}); break; case 0x20: v = s.Emit(Spv::OpFMul, tF, {v, s.CFloat(4.f)}); break;
      case 0x30: v = s.Emit(Spv::OpFMul, tF, {v, half}); break; default: break; }
    return s.Glsl3(tF, Spv::G_FClamp, v, s.CFloat(-1.f), one);
  };
  auto writeRgb = [&](uint32_t r, uint32_t v3) { if (!r) return; uint32_t old = s.Load(tV4, reg[r]); uint32_t a = comp(old, 3); s.Store(reg[r], s.Emit(Spv::OpCompositeConstruct, tV4, {comp(v3, 0), comp(v3, 1), comp(v3, 2), a})); };
  auto writeA = [&](uint32_t r, uint32_t v) { if (!r) return; uint32_t old = s.Load(tV4, reg[r]); s.Store(reg[r], s.Emit(Spv::OpCompositeInsert, tV4, {v, old, 3})); };
  static int maxStages = getenv("XBOX_FS_STAGES") ? atoi(getenv("XBOX_FS_STAGES")) : 99;
  int dbgStage = -1, dbgReg = 0; if (const char* dr = getenv("XBOX_FS_DEBUGREG")) if (dbgOn) sscanf(dr, "%d,%d", &dbgStage, &dbgReg);   // debug: show register <reg> after combiner stage <stage>
  uint32_t dbgVal = 0;
  for (uint32_t st = 0; st < f.numStages && st < 8 && (int)st < maxStages; st++) {
    s.Store(reg[1], uniformVec(0, f.uniqueC0 ? st : 0)); s.Store(reg[2], uniformVec(1, f.uniqueC1 ? st : 0));
    // RGB portion
    {
      uint32_t icw = f.icwC[st], ocw = f.ocwC[st];
      uint32_t A = fetchRgb(icw >> 24), B = fetchRgb((icw >> 16) & 255), C = fetchRgb((icw >> 8) & 255), D = fetchRgb(icw & 255);
      uint32_t flags = ocw >> 12; bool abDot = flags & 2, cdDot = flags & 1, mux = flags & 4; uint32_t mapping = flags & 0x38;
      uint32_t ab = s.Emit(Spv::OpFMul, tV3, {A, B}); if (abDot) { uint32_t d = s.Emit(Spv::OpDot, tF, {A, B}); ab = s.Emit(Spv::OpCompositeConstruct, tV3, {d, d, d}); }
      uint32_t cd = s.Emit(Spv::OpFMul, tV3, {C, D}); if (cdDot) { uint32_t d = s.Emit(Spv::OpDot, tF, {C, D}); cd = s.Emit(Spv::OpCompositeConstruct, tV3, {d, d, d}); }
      uint32_t sum;
      if (mux) {
        uint32_t r0a = comp(s.Load(tV4, reg[12]), 3);
        uint32_t useAB;
        if (f.muxMsb) useAB = s.Emit(Spv::OpFOrdLessThan, tBool, {r0a, half});
        else { uint32_t v255 = s.Emit(Spv::OpFMul, tF, {r0a, s.CFloat(255.f)}); uint32_t fl = s.Glsl1(tF, Spv::G_Floor, v255); uint32_t hv = s.Emit(Spv::OpFMul, tF, {fl, half}); uint32_t fr = s.Glsl1(tF, Spv::G_Fract, hv); useAB = s.Emit(Spv::OpFOrdLessThan, tBool, {fr, half}); }
        uint32_t bsel = s.Emit(Spv::OpCompositeConstruct, s.TVec(tBool, 3), {useAB, useAB, useAB}); sum = s.Emit(Spv::OpSelect, tV3, {bsel, ab, cd});
      } else sum = s.Emit(Spv::OpFAdd, tV3, {ab, cd});
      ab = postV3(ab, mapping); cd = postV3(cd, mapping); sum = postV3(sum, mapping);
      writeRgb(ocw & 15, cd); writeRgb((ocw >> 4) & 15, ab); writeRgb((ocw >> 8) & 15, sum);
    }
    // alpha portion
    {
      uint32_t icw = f.icwA[st], ocw = f.ocwA[st];
      uint32_t A = fetchAlpha(icw >> 24), B = fetchAlpha((icw >> 16) & 255), C = fetchAlpha((icw >> 8) & 255), D = fetchAlpha(icw & 255);
      uint32_t flags = ocw >> 12; bool mux = flags & 4; uint32_t mapping = flags & 0x38;
      uint32_t ab = s.Emit(Spv::OpFMul, tF, {A, B}), cd = s.Emit(Spv::OpFMul, tF, {C, D}), sum;
      if (mux) { uint32_t r0a = comp(s.Load(tV4, reg[12]), 3); uint32_t useAB;
        if (f.muxMsb) useAB = s.Emit(Spv::OpFOrdLessThan, tBool, {r0a, half});
        else { uint32_t v255 = s.Emit(Spv::OpFMul, tF, {r0a, s.CFloat(255.f)}); uint32_t fl = s.Glsl1(tF, Spv::G_Floor, v255); uint32_t hv = s.Emit(Spv::OpFMul, tF, {fl, half}); uint32_t fr = s.Glsl1(tF, Spv::G_Fract, hv); useAB = s.Emit(Spv::OpFOrdLessThan, tBool, {fr, half}); }
        sum = s.Emit(Spv::OpSelect, tF, {useAB, ab, cd}); }
      else sum = s.Emit(Spv::OpFAdd, tF, {ab, cd});
      ab = postF(ab, mapping); cd = postF(cd, mapping); sum = postF(sum, mapping);
      writeA(ocw & 15, cd); writeA((ocw >> 4) & 15, ab); writeA((ocw >> 8) & 15, sum);
    }
    if ((int)st == dbgStage && dbgReg > 0 && dbgReg < 16) { uint32_t dv = s.Load(tV4, reg[dbgReg]); dbgVal = s.Shuffle(tV3, dv, dv, {0, 1, 2}); }
  }
  // ---- final combiner ----
  uint32_t finalRgb, finalA;
  if ((!f.final0 && !f.final1) || (dbgOn && getenv("XBOX_FS_NOFINAL"))) {
    uint32_t src = s.Load(tV4, reg[f.numStages ? 12 : 4]); finalRgb = s.Shuffle(tV3, src, src, {0, 1, 2}); finalA = comp(src, 3);
  } else {
    uint32_t flags = f.final1 & 0xFF; bool clampSum = flags & 0x80;
    uint32_t v1 = s.Load(tV4, reg[5]), r0 = s.Load(tV4, reg[12]);
    uint32_t v1v = s.Shuffle(tV3, v1, v1, {0, 1, 2}), r0v = s.Shuffle(tV3, r0, r0, {0, 1, 2});
    uint32_t sumv = s.Emit(Spv::OpFAdd, tV3, {v1v, r0v}); if (clampSum) sumv = s.Glsl3(tV3, Spv::G_FClamp, sumv, s.CVec(3, zero), s.CVec(3, one));
    s.Store(reg[14], s.Emit(Spv::OpCompositeConstruct, tV4, {comp(sumv, 0), comp(sumv, 1), comp(sumv, 2), zero}));
    // final constants (c0 / c1 of the final stage) live in the uniform block (final-stage factors): registers 1 and 2
    s.Store(reg[1], uniformVec(2, ~0u)); s.Store(reg[2], uniformVec(3, ~0u));
    auto get3 = [&](uint32_t in) {
      uint32_t r = in & 15, alphaSel = (in >> 4) & 1, mode = in & 0xE0; uint32_t src = r == 0 ? zero4 : s.Load(tV4, reg[r]); uint32_t v;
      if (alphaSel) { uint32_t a = comp(src, 3); v = s.Emit(Spv::OpCompositeConstruct, tV3, {a, a, a}); } else v = s.Shuffle(tV3, src, src, {0, 1, 2});
      return mapIn(v, mode, true);
    };
    // E*F product (register 0xF) must be available to inputs
    uint32_t E = get3(f.final1 >> 24), F = get3((f.final1 >> 16) & 255); uint32_t ef = s.Emit(Spv::OpFMul, tV3, {E, F});
    s.Store(reg[15], s.Emit(Spv::OpCompositeConstruct, tV4, {comp(ef, 0), comp(ef, 1), comp(ef, 2), zero}));
    uint32_t A = get3(f.final0 >> 24), B = get3((f.final0 >> 16) & 255), C = get3((f.final0 >> 8) & 255), D = get3(f.final0 & 255);
    uint32_t oneM = s.Emit(Spv::OpFSub, tV3, {s.CVec(3, one), A});
    uint32_t res = s.Emit(Spv::OpFAdd, tV3, {s.Emit(Spv::OpFAdd, tV3, {s.Emit(Spv::OpFMul, tV3, {A, B}), s.Emit(Spv::OpFMul, tV3, {oneM, C})}), D});
    finalRgb = s.Glsl3(tV3, Spv::G_FClamp, res, s.CVec(3, zero), s.CVec(3, one));
    if (dbgVal) finalRgb = s.Glsl3(tV3, Spv::G_FClamp, dbgVal, s.CVec(3, zero), s.CVec(3, one));
    if (const char* dbg = dbgOn ? getenv("XBOX_FS_DEBUGFINAL") : nullptr) {   // debug: show one piece of the final combiner instead (1 = E*F, 2 = E, 3 = F, 4 = A, 5 = B)
      uint32_t pick = dbg[0] == '1' ? ef : dbg[0] == '2' ? E : dbg[0] == '3' ? F : dbg[0] == '4' ? A : dbg[0] == '6' ? D : dbg[0] == '7' ? res : B;
      finalRgb = s.Glsl3(tV3, Spv::G_FClamp, pick, s.CVec(3, zero), s.CVec(3, one)); }
    uint32_t gin = (f.final1 >> 8) & 255; uint32_t gsrc = (gin & 15) == 0 ? zero4 : s.Load(tV4, reg[gin & 15]);
    finalA = s.Glsl3(tF, Spv::G_FClamp, mapIn(comp(gsrc, (gin & 0x10) ? 3 : 2), gin & 0xE0, false), zero, one);   // G input channel: bit 4 set = alpha, clear = blue (xemu get_input_var)
  }
  if (getenv("XBOX_FS_FORCEALPHA")) finalA = one;   // debug: opaque output alpha
  uint32_t color = s.Emit(Spv::OpCompositeConstruct, tV4, {comp(finalRgb, 0), comp(finalRgb, 1), comp(finalRgb, 2), finalA});
  if (const char* dtc = dbgOn ? getenv("XBOX_FS_DEBUGTC") : nullptr) {   // debug: show the stage's texture coordinate (x, y, w) instead of the shaded colour
    uint32_t tcv = s.Load(tV4, inT[atoi(dtc) & 3]);
    auto cl = [&](uint32_t v) { return s.Glsl3(tF, Spv::G_FClamp, v, zero, one); };
    color = s.Vec4(cl(comp(tcv, 0)), cl(comp(tcv, 1)), cl(comp(tcv, 3)), one); }
  // alpha test (discard)
  if (f.alphaTest && !getenv("XBOX_FS_NOALPHA")) {
    uint32_t a = finalA; uint32_t ref = s.Emit(Spv::OpFDiv, tF, {comp(uniformVec(6, ~0u), 0), s.CFloat(255.f)});
    uint32_t pass;
    switch (f.alphaFunc) {
      case 0x200: pass = s.CFloat(0.f); pass = s.Emit(Spv::OpFOrdLessThan, tBool, {one, zero}); break;
      case 0x201: pass = s.Emit(Spv::OpFOrdLessThan, tBool, {a, ref}); break;
      case 0x202: pass = s.Emit(Spv::OpFOrdEqual, tBool, {a, ref}); break;
      case 0x203: pass = s.Emit(Spv::OpFOrdLessThanEqual, tBool, {a, ref}); break;
      case 0x204: pass = s.Emit(Spv::OpFOrdGreaterThan, tBool, {a, ref}); break;
      case 0x205: pass = s.Emit(Spv::OpFOrdNotEqual, tBool, {a, ref}); break;
      case 0x206: pass = s.Emit(Spv::OpFOrdGreaterThanEqual, tBool, {a, ref}); break;
      default: pass = s.Emit(Spv::OpFOrdLessThan, tBool, {zero, one}); break;
    }
    uint32_t killL = s.Id(), contL = s.Id();
    Spv::Put(s.cur, 247, {contL, 0});                 // OpSelectionMerge
    Spv::Put(s.cur, 250, {pass, contL, killL});       // OpBranchConditional pass ? continue : kill
    s.Label(killL); Spv::Put(s.cur, Spv::OpKill, {});
    s.Label(contL);
  }
  s.Store(outCol, color);
  s.EndMain();
  s.Header(true, iface);
  return s.Finish();
}

// ---- bisecting helpers: trivial shaders with the same interfaces ----
std::vector<uint32_t> GenSimpleFragmentShader() {
  Spv s; uint32_t tV4 = s.TVec4(); std::vector<uint32_t> iface;
  uint32_t inD0 = s.Variable(tV4, Spv::Input); s.Decorate(inD0, Spv::D_Location, {0}); iface.push_back(inD0);
  uint32_t out = s.Variable(tV4, Spv::Output); s.Decorate(out, Spv::D_Location, {0}); iface.push_back(out);
  s.BeginMain(); s.Store(out, s.Load(tV4, inD0)); s.EndMain(); s.Header(true, iface); return s.Finish();
}
std::vector<uint32_t> GenSimpleVertexShader(uint32_t* usedInputs) {
  Spv s; uint32_t tF = s.TFloat(), tV4 = s.TVec4(); *usedInputs = 1; std::vector<uint32_t> iface;
  uint32_t in0 = s.Variable(tV4, Spv::Input); s.Decorate(in0, Spv::D_Location, {0}); iface.push_back(in0);
  uint32_t pv = s.TStruct({tV4}); s.Decorate(pv, Spv::D_Block); s.MemberDecorate(pv, 0, Spv::D_BuiltIn, {Spv::B_Position});
  uint32_t posOut = s.Variable(pv, Spv::Output); iface.push_back(posOut);
  uint32_t vary[7]; for (int i = 0; i < 7; i++) { vary[i] = s.Variable(tV4, Spv::Output); s.Decorate(vary[i], Spv::D_Location, {(uint32_t)i}); iface.push_back(vary[i]); }
  s.BeginMain(); uint32_t v = s.Load(tV4, in0);
  uint32_t x = s.Emit(Spv::OpFSub, tF, {s.Emit(Spv::OpFDiv, tF, {s.Extract(tF, v, 0), s.CFloat(320.f)}), s.CFloat(1.f)});
  uint32_t y = s.Emit(Spv::OpFSub, tF, {s.Emit(Spv::OpFDiv, tF, {s.Extract(tF, v, 1), s.CFloat(240.f)}), s.CFloat(1.f)});
  s.Store(s.Emit(Spv::OpAccessChain, s.TPtr(Spv::Output, tV4), {posOut, s.CInt(0)}), s.Vec4(x, y, s.CFloat(0.5f), s.CFloat(1.f)));
  for (int i = 0; i < 7; i++) s.Store(vary[i], s.CVec4(1, 0, 0, 1));
  s.EndMain(); s.Header(false, iface); return s.Finish();
}
