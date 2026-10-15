// Fixed-function vertex pipeline (T&L) as a SPIR-V vertex shader: matrix-palette skinning, lighting (infinite / local / spot-as-local lights), texture-coordinate
// generation, texture matrices and the fog coordinate. Semantics follow the hardware documentation, with xemu's glsl/vsh-ff.c read as reference.
// Parameters that are not in the c[] constant file travel in the uniform array ffp[] (filled by FfpFill below, called from gpu_vk.cpp):
//   ffp[0] = scene ambient.xyz, material alpha | ffp[1] = material emission.xyz, specular power | ffp[2] = eye position | ffp[3] = fog plane
//   ffp[4 + 5*i ..] per light: ambient.xyz | diffuse.xyz | specular.xyz | position-or-direction.xyz, range | attenuation.xyz
#include "vk_shaders.h"
#include "spirv_builder.h"
#include <cstring>

std::vector<uint32_t> GenFixedVertexShader(const FfState& st, uint32_t* usedInputs) {
  Spv s; uint32_t mask = 0x1 | 0x4 | 0x8 | 0x10 | 0x20 | (0xFu << 9);   // position, normal, diffuse, specular, fog coord, texcoords
  if (st.skin) mask |= 0x2;
  *usedInputs = mask;
  uint32_t tF = s.TFloat(), tV4 = s.TVec4(), tV3 = s.TVec(tF, 3), tB = s.TBool();
  uint32_t arr = s.TArray(tV4, s.CUint(192)); s.Decorate(arr, Spv::D_ArrayStride, {16});
  uint32_t arrF = s.TArray(tV4, s.CUint(64)); s.Decorate(arrF, Spv::D_ArrayStride, {16});
  uint32_t ub = s.TStruct({arr, tV4, arrF}); s.Decorate(ub, Spv::D_Block);
  s.MemberDecorate(ub, 0, Spv::D_Offset, {0}); s.MemberDecorate(ub, 1, Spv::D_Offset, {192 * 16}); s.MemberDecorate(ub, 2, Spv::D_Offset, {193 * 16});
  uint32_t ubVar = s.Variable(ub, Spv::Uniform); s.Decorate(ubVar, Spv::D_DescriptorSet, {0}); s.Decorate(ubVar, Spv::D_Binding, {0});
  std::vector<uint32_t> iface; uint32_t inVar[16] = {};
  for (int i = 0; i < 16; i++) if (mask & (1u << i)) { inVar[i] = s.Variable(tV4, Spv::Input); s.Decorate(inVar[i], Spv::D_Location, {(uint32_t)i}); iface.push_back(inVar[i]); }
  uint32_t pv = s.TStruct({tV4}); s.Decorate(pv, Spv::D_Block); s.MemberDecorate(pv, 0, Spv::D_BuiltIn, {Spv::B_Position}); s.MemberDecorate(pv, 0, Spv::D_Invariant);   // identical position math in every vertex program, so a later pass can test EQUAL against an earlier pass's depth
  uint32_t posOut = s.Variable(pv, Spv::Output); iface.push_back(posOut);
  uint32_t varying[7]; for (int i = 0; i < 7; i++) { varying[i] = s.Variable(tV4, Spv::Output); s.Decorate(varying[i], Spv::D_Location, {(uint32_t)i}); iface.push_back(varying[i]); }
  s.BeginMain();
  uint32_t ptrV4U = s.TPtr(Spv::Uniform, tV4);
  auto cst = [&](int i) { return s.Load(tV4, s.Emit(Spv::OpAccessChain, ptrV4U, {ubVar, s.CInt(0), s.CInt(i)})); };
  auto ffp = [&](int i) { return s.Load(tV4, s.Emit(Spv::OpAccessChain, ptrV4U, {ubVar, s.CInt(2), s.CInt(i)})); };
  auto comp = [&](uint32_t v, uint32_t i) { return s.Extract(tF, v, i); };
  auto xyz = [&](uint32_t v) { return s.Shuffle(tV3, v, v, {0, 1, 2}); };
  auto dot = [&](uint32_t a, uint32_t b) { return s.Emit(Spv::OpDot, tF, {a, b}); };
  auto add4 = [&](uint32_t a, uint32_t b) { return s.Emit(Spv::OpFAdd, tV4, {a, b}); };
  auto add3 = [&](uint32_t a, uint32_t b) { return s.Emit(Spv::OpFAdd, tV3, {a, b}); };
  auto sub3 = [&](uint32_t a, uint32_t b) { return s.Emit(Spv::OpFSub, tV3, {a, b}); };
  auto mul3 = [&](uint32_t a, uint32_t b) { return s.Emit(Spv::OpFMul, tV3, {a, b}); };
  auto vec3 = [&](uint32_t x, uint32_t y, uint32_t z) { return s.Emit(Spv::OpCompositeConstruct, tV3, {x, y, z}); };
  auto scale4 = [&](uint32_t v, uint32_t f) { return s.Emit(Spv::OpFMul, tV4, {v, s.Vec4(f, f, f, f)}); };
  auto scale3 = [&](uint32_t v, uint32_t f) { return mul3(v, vec3(f, f, f)); };
  auto fmul = [&](uint32_t a, uint32_t b) { return s.Emit(Spv::OpFMul, tF, {a, b}); };
  auto fadd = [&](uint32_t a, uint32_t b) { return s.Emit(Spv::OpFAdd, tF, {a, b}); };
  auto fsub = [&](uint32_t a, uint32_t b) { return s.Emit(Spv::OpFSub, tF, {a, b}); };
  auto fdiv = [&](uint32_t a, uint32_t b) { return s.Emit(Spv::OpFDiv, tF, {a, b}); };
  auto fmax = [&](uint32_t a, uint32_t b) { return s.Glsl2(tF, Spv::G_FMax, a, b); };
  auto norm3 = [&](uint32_t v) { return s.Glsl1(tV3, Spv::G_Normalize, v); };
  auto len3 = [&](uint32_t v) { return s.Glsl1(tF, Spv::G_Length, v); };
  uint32_t zero = s.CFloat(0.f), one = s.CFloat(1.f), two = s.CFloat(2.f), zero3 = s.CVec(3, zero);
  // Matrices are stored transposed (four consecutive c[] rows): result[j] = dot(v, c[base + j])
  auto mulMat = [&](uint32_t v, int base) { return s.Vec4(dot(v, cst(base)), dot(v, cst(base + 1)), dot(v, cst(base + 2)), dot(v, cst(base + 3))); };
  uint32_t position = s.Load(tV4, inVar[0]), diffuse = s.Load(tV4, inVar[3]), specular = s.Load(tV4, inVar[4]);
  uint32_t normal4 = s.Load(tV4, inVar[2]); uint32_t n4 = s.Vec4(comp(normal4, 0), comp(normal4, 1), comp(normal4, 2), zero);
  // ---- model-view transform (with matrix-palette skinning) ----
  uint32_t tPos = mulMat(position, 8), tNormal = xyz(mulMat(n4, 0xC));
  if (st.skin) {
    static const int cnt[7] = {0, 2, 2, 3, 3, 4, 4}; static const bool mix[7] = {false, true, false, true, false, true, false};
    int k = st.skin > 6 ? 0 : (int)st.skin; int n = cnt[k]; bool mx = mix[k]; uint32_t w = s.Load(tV4, inVar[1]);
    uint32_t accP = s.CVec4(0, 0, 0, 0), accN = zero3, rem = one;
    for (int i = 0; i < n; i++) {
      uint32_t wi; if (mx) { if (i < n - 1) { wi = comp(w, i); rem = fsub(rem, wi); } else wi = rem; } else wi = comp(w, i);
      accP = add4(accP, scale4(mulMat(position, 8 + 8 * i), wi)); accN = add3(accN, scale3(xyz(mulMat(n4, 0xC + 8 * i)), wi));
    }
    tPos = accP; tNormal = accN;
  }
  if (st.normalize) tNormal = norm3(tNormal);
  // ---- screen position through the composite matrix (it contains the viewport) ----
  uint32_t r = s.Vec4(dot(position, cst(0)), dot(position, cst(1)), dot(position, cst(2)), dot(position, cst(3)));
  uint32_t misc = s.Load(tV4, s.Emit(Spv::OpAccessChain, ptrV4U, {ubVar, s.CInt(1)}));
  uint32_t W = comp(misc, 0), H = comp(misc, 1), ZM = comp(misc, 2);
  uint32_t pw = comp(r, 3); pw = s.Emit(Spv::OpSelect, tF, {s.Emit(Spv::OpFOrdEqual, tB, {pw, zero}), one, pw});
  uint32_t iw = fdiv(one, pw);
  uint32_t px = fmul(comp(r, 0), iw), py = fmul(comp(r, 1), iw), pz = fmul(comp(r, 2), iw);
  uint32_t cx = fdiv(fsub(fmul(two, px), W), W), cy = fdiv(fsub(fmul(two, py), H), H), cz = fdiv(pz, ZM);
  { static const bool margin = getenv("XBOX_NO_ZMARGIN") == nullptr; if (margin) cz = fadd(fmul(cz, s.CFloat(0.99998f)), s.CFloat(0.00001f)); }   // depth strictly inside the clip volume (see vk_shaders.cpp)
  s.Store(s.Emit(Spv::OpAccessChain, s.TPtr(Spv::Output, tV4), {posOut, s.CInt(0)}), s.Vec4(fmul(cx, pw), fmul(cy, pw), fmul(cz, pw), pw));
  // ---- texture coordinates: texgen + texture matrix ----
  bool needRefl = false; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) if (st.texgen[i][j] == 0x2402 || st.texgen[i][j] == 0x8512) needRefl = true;
  uint32_t refl = zero3;
  if (needRefl) { uint32_t u = norm3(xyz(tPos)); refl = sub3(u, scale3(tNormal, fmul(two, dot(tNormal, u)))); }   // reflect(u, n)
  for (int i = 0; i < 4; i++) {
    uint32_t tc = s.Load(tV4, inVar[9 + i]); uint32_t out[4];
    for (int j = 0; j < 4; j++) {
      uint32_t plane = cst(0x40 + i * 8 + j);
      switch (st.texgen[i][j]) {
        case 0x2400: out[j] = dot(plane, tPos); break;       // eye linear
        case 0x2401: out[j] = dot(plane, position); break;   // object linear
        case 0x2402:                                         // sphere map (S, T only)
          if (j < 2) { uint32_t ro = vec3(comp(refl, 0), comp(refl, 1), fadd(comp(refl, 2), one)); out[j] = fadd(fmul(comp(refl, j), fdiv(one, fmul(two, len3(ro)))), s.CFloat(0.5f)); }
          else out[j] = comp(tc, j);
          break;
        case 0x8512: out[j] = j < 3 ? comp(refl, j) : comp(tc, j); break;      // reflection map
        case 0x8511: out[j] = j < 3 ? comp(tNormal, j) : comp(tc, j); break;  // normal map
        default: out[j] = comp(tc, j); break;
      }
    }
    uint32_t o = s.Vec4(out[0], out[1], out[2], out[3]);
    if (st.texMat[i]) o = mulMat(o, 0x44 + i * 8);
    s.Store(varying[2 + i], o);
  }
  // ---- colours / lighting ----
  if (!st.lighting) { s.Store(varying[0], diffuse); s.Store(varying[1], specular); }
  else {
    uint32_t f0 = ffp(0), f1 = ffp(1), f2 = ffp(2);
    uint32_t sceneAmb = xyz(f0), matAlpha = comp(f0, 3), matEmis = xyz(f1), specPow = comp(f1, 3);
    uint32_t alpha = st.diffuse == 1 ? comp(diffuse, 3) : st.diffuse == 2 ? comp(specular, 3) : matAlpha;
    uint32_t d0 = st.ambient == 1 ? xyz(diffuse) : st.ambient == 2 ? xyz(specular) : sceneAmb;
    d0 = mul3(d0, matEmis);   // order as in the reference implementation
    d0 = add3(d0, st.emission == 0 ? sceneAmb : st.emission == 1 ? xyz(diffuse) : xyz(specular));
    uint32_t d1 = zero3;
    uint32_t eyeDir = st.localEye ? norm3(sub3(xyz(f2), xyz(tPos))) : zero3;
    for (int i = 0; i < 8; i++) {
      uint32_t lt = st.light[i]; if (!lt) continue;
      uint32_t la = xyz(ffp(4 + 5 * i)), ld = xyz(ffp(5 + 5 * i)), ls = xyz(ffp(6 + 5 * i)), lp = ffp(7 + 5 * i), at = ffp(8 + 5 * i);
      uint32_t att, nDotVP, nDotHV;
      if (lt == 1) {   // infinite light: lp.xyz = direction towards the light
        uint32_t dir = norm3(xyz(lp)); att = one; nDotVP = fmax(zero, dot(tNormal, dir));
        uint32_t hv = norm3(add3(dir, st.localEye ? eyeDir : vec3(zero, zero, one))); nDotHV = fmax(zero, dot(tNormal, hv));
      } else {         // local light (a spot cone is not modelled)
        uint32_t vp = sub3(xyz(lp), xyz(tPos)); uint32_t d = len3(vp), vpn = norm3(vp);
        uint32_t den = fadd(fadd(comp(at, 0), fmul(comp(at, 1), d)), fmul(fmul(comp(at, 2), d), d));
        att = s.Emit(Spv::OpSelect, tF, {s.Emit(Spv::OpFOrdLessThanEqual, tB, {d, comp(lp, 3)}), fdiv(one, den), zero});
        nDotVP = fmax(zero, dot(tNormal, vpn));
        nDotHV = fmax(zero, dot(tNormal, norm3(add3(vpn, st.localEye ? eyeDir : zero3))));
      }
      uint32_t none = s.Emit(Spv::OpLogicalOr, tB, {s.Emit(Spv::OpFOrdEqual, tB, {nDotVP, zero}), s.Emit(Spv::OpFOrdEqual, tB, {nDotHV, zero})});
      uint32_t pf = s.Emit(Spv::OpSelect, tF, {none, zero, s.Glsl2(tF, Spv::G_Pow, nDotHV, specPow)});
      uint32_t lDif = scale3(scale3(ld, att), nDotVP), lSpc = scale3(scale3(ls, att), pf);
      d0 = add3(d0, scale3(la, att));
      d0 = add3(d0, st.diffuse == 0 ? lDif : st.diffuse == 1 ? mul3(xyz(diffuse), lDif) : mul3(xyz(specular), lDif));
      d1 = add3(d1, st.specular == 0 ? lSpc : st.specular == 1 ? mul3(xyz(diffuse), lSpc) : mul3(xyz(specular), lSpc));
    }
    s.Store(varying[0], s.Vec4(comp(d0, 0), comp(d0, 1), comp(d0, 2), alpha));
    s.Store(varying[1], s.Vec4(comp(d1, 0), comp(d1, 1), comp(d1, 2), comp(specular, 3)));
  }
  // ---- fog coordinate ----
  uint32_t fog = zero;
  if (st.fogOn) {
    uint32_t fp = ffp(3);
    switch (st.fogGen) {
      case 0: fog = comp(specular, 3); break;
      case 1: fog = len3(xyz(tPos)); break;
      case 2: fog = fadd(dot(xyz(fp), xyz(tPos)), comp(fp, 3)); break;
      case 3: fog = s.Glsl1(tF, Spv::G_FAbs, fadd(dot(xyz(fp), xyz(tPos)), comp(fp, 3))); break;
      case 6: fog = comp(s.Load(tV4, inVar[5]), 0); break;
      default: break;
    }
  }
  s.Store(varying[6], s.Vec4(fog, zero, zero, zero));
  s.EndMain(); s.Header(false, iface); return s.Finish();
}
