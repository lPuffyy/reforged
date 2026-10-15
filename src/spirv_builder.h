// Minimal SPIR-V module builder (no external dependencies). Produces SPIR-V 1.0 for Vulkan shaders: enough for straight-line vertex/fragment code with
// uniform buffers, sampled images and the GLSL.std.450 extended instructions. Used by the NV2A vertex-program and register-combiner translators.
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

struct Spv {
  // opcodes (SPIR-V spec numbering)
  enum Op : uint16_t {
    OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17,
    OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeImage = 25, OpTypeSampledImage = 27, OpTypeArray = 28,
    OpTypeStruct = 30, OpTypePointer = 32, OpTypeFunction = 33, OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43, OpConstantComposite = 44,
    OpFunction = 54, OpFunctionParameter = 55, OpFunctionEnd = 56, OpFunctionCall = 57, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65,
    OpDecorate = 71, OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeConstruct = 80, OpCompositeExtract = 81, OpCompositeInsert = 82,
    OpImageSampleImplicitLod = 87, OpImageSampleProjImplicitLod = 91, OpImageQuerySizeLod = 103, OpConvertFToS = 110, OpConvertSToF = 111,
    OpFNegate = 127, OpVectorTimesScalar = 142, OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpFDiv = 136, OpDot = 148, OpSelect = 169, OpLogicalNot = 168, OpLogicalOr = 166,
    OpFOrdEqual = 180, OpFOrdLessThan = 184, OpFOrdGreaterThan = 186, OpFOrdLessThanEqual = 188, OpFOrdGreaterThanEqual = 190, OpFOrdNotEqual = 182,
    OpLabel = 248, OpBranch = 249, OpKill = 252, OpReturn = 253, OpSNegate = 126, OpIAdd = 128, OpSLessThan = 177, OpSGreaterThan = 173, OpExtInstDummy = 0,
  };
  // GLSL.std.450 instructions
  enum Glsl { G_Trunc = 3, G_FAbs = 4, G_FSign = 6, G_Floor = 8, G_Fract = 10, G_Pow = 26, G_Exp2 = 29, G_Log2 = 30, G_Sqrt = 31, G_InverseSqrt = 32,
              G_FMin = 37, G_FMax = 40, G_FClamp = 43, G_Normalize = 69, G_SClamp = 45, G_Length = 66 };
  enum Storage { Input = 1, Uniform = 2, Output = 3, Private = 6, Function = 7, UniformConstant = 0 };
  enum Deco { D_Block = 2, D_BuiltIn = 11, D_Location = 30, D_Binding = 33, D_DescriptorSet = 34, D_Offset = 35, D_ArrayStride = 6, D_Flat = 14, D_NoPerspective = 13, D_Invariant = 18 };
  enum BuiltIn { B_Position = 0 };

  std::vector<uint32_t> caps, ext, mem, entry, modes, annot, types, body;
  uint32_t nextId = 1;
  uint32_t glsl = nextId++;   // GLSL.std.450 import id, allocated up front: ExtInst is emitted before Header()
  std::map<std::tuple<int, uint32_t, uint32_t>, uint32_t> typeCache;
  std::map<uint64_t, uint32_t> constCache;
  std::vector<uint32_t> cur;   // instructions of the function being built

  uint32_t Id() { return nextId++; }
  static void Put(std::vector<uint32_t>& v, uint16_t op, std::initializer_list<uint32_t> w) {
    v.push_back(((uint32_t)(w.size() + 1) << 16) | op); for (uint32_t x : w) v.push_back(x);
  }
  static void PutVec(std::vector<uint32_t>& v, uint16_t op, const std::vector<uint32_t>& w) { v.push_back(((uint32_t)(w.size() + 1) << 16) | op); for (uint32_t x : w) v.push_back(x); }

  // ---- types ----
  uint32_t cached(int kind, uint32_t a, uint32_t b, const std::vector<uint32_t>& words, uint16_t op) {
    auto key = std::make_tuple(kind, a, b); auto it = typeCache.find(key); if (it != typeCache.end() && words.empty()) return it->second;
    uint32_t id = Id(); std::vector<uint32_t> w; w.push_back(id); w.insert(w.end(), words.begin(), words.end()); PutVec(types, op, w);
    if (words.empty()) {} typeCache[key] = id; return id;
  }
  uint32_t TVoid() { auto k = std::make_tuple(0, 0u, 0u); auto it = typeCache.find(k); if (it != typeCache.end()) return it->second; uint32_t id = Id(); Put(types, OpTypeVoid, {id}); typeCache[k] = id; return id; }
  uint32_t TBool() { auto k = std::make_tuple(1, 0u, 0u); auto it = typeCache.find(k); if (it != typeCache.end()) return it->second; uint32_t id = Id(); Put(types, OpTypeBool, {id}); typeCache[k] = id; return id; }
  uint32_t TFloat() { auto k = std::make_tuple(2, 32u, 0u); auto it = typeCache.find(k); if (it != typeCache.end()) return it->second; uint32_t id = Id(); Put(types, OpTypeFloat, {id, 32}); typeCache[k] = id; return id; }
  uint32_t TInt(bool sign) { auto k = std::make_tuple(3, 32u, (uint32_t)sign); auto it = typeCache.find(k); if (it != typeCache.end()) return it->second; uint32_t id = Id(); Put(types, OpTypeInt, {id, 32, (uint32_t)sign}); typeCache[k] = id; return id; }
  uint32_t TVec(uint32_t comp, uint32_t n) { auto k = std::make_tuple(4, comp, n); auto it = typeCache.find(k); if (it != typeCache.end()) return it->second; uint32_t id = Id(); Put(types, OpTypeVector, {id, comp, n}); typeCache[k] = id; return id; }
  uint32_t TPtr(uint32_t storage, uint32_t t) { auto k = std::make_tuple(5, storage, t); auto it = typeCache.find(k); if (it != typeCache.end()) return it->second; uint32_t id = Id(); Put(types, OpTypePointer, {id, storage, t}); typeCache[k] = id; return id; }
  uint32_t TArray(uint32_t t, uint32_t lenConst) { auto k = std::make_tuple(6, t, lenConst); auto it = typeCache.find(k); if (it != typeCache.end()) return it->second; uint32_t id = Id(); Put(types, OpTypeArray, {id, t, lenConst}); typeCache[k] = id; return id; }
  uint32_t TStruct(const std::vector<uint32_t>& members) { uint32_t id = Id(); std::vector<uint32_t> w{id}; w.insert(w.end(), members.begin(), members.end()); PutVec(types, OpTypeStruct, w); return id; }
  uint32_t TFunc(uint32_t ret) { auto k = std::make_tuple(7, ret, 0u); auto it = typeCache.find(k); if (it != typeCache.end()) return it->second; uint32_t id = Id(); Put(types, OpTypeFunction, {id, ret}); typeCache[k] = id; return id; }
  uint32_t TImage2D() { auto k = std::make_tuple(8, 0u, 0u); auto it = typeCache.find(k); if (it != typeCache.end()) return it->second; uint32_t f = TFloat(), id = Id(); Put(types, OpTypeImage, {id, f, 1 /*2D*/, 0, 0, 0, 1 /*sampled*/, 0 /*unknown*/}); typeCache[k] = id; return id; }
  uint32_t TImageCube() { auto k = std::make_tuple(10, 0u, 0u); auto it = typeCache.find(k); if (it != typeCache.end()) return it->second; uint32_t f = TFloat(), id = Id(); Put(types, OpTypeImage, {id, f, 3 /*Cube*/, 0, 0, 0, 1 /*sampled*/, 0 /*unknown*/}); typeCache[k] = id; return id; }
  uint32_t TSampledCube() { auto k = std::make_tuple(11, 0u, 0u); auto it = typeCache.find(k); if (it != typeCache.end()) return it->second; uint32_t im = TImageCube(), id = Id(); Put(types, OpTypeSampledImage, {id, im}); typeCache[k] = id; return id; }
  uint32_t TSampled2D() { auto k = std::make_tuple(9, 0u, 0u); auto it = typeCache.find(k); if (it != typeCache.end()) return it->second; uint32_t im = TImage2D(), id = Id(); Put(types, OpTypeSampledImage, {id, im}); typeCache[k] = id; return id; }
  uint32_t TVec4() { return TVec(TFloat(), 4); }

  // ---- constants ----
  uint32_t CFloat(float f) { uint32_t u; memcpy(&u, &f, 4); uint64_t key = (1ull << 40) | u; auto it = constCache.find(key); if (it != constCache.end()) return it->second; uint32_t id = Id(); Put(types, OpConstant, {TFloat(), id, u}); constCache[key] = id; return id; }
  uint32_t CInt(int32_t v) { uint64_t key = (2ull << 40) | (uint32_t)v; auto it = constCache.find(key); if (it != constCache.end()) return it->second; uint32_t id = Id(); Put(types, OpConstant, {TInt(true), id, (uint32_t)v}); constCache[key] = id; return id; }
  uint32_t CUint(uint32_t v) { uint64_t key = (3ull << 40) | v; auto it = constCache.find(key); if (it != constCache.end()) return it->second; uint32_t id = Id(); Put(types, OpConstant, {TInt(false), id, v}); constCache[key] = id; return id; }
  uint32_t CVec4(float a, float b, float c, float d) { uint32_t id = Id(); Put(types, OpConstantComposite, {TVec4(), id, CFloat(a), CFloat(b), CFloat(c), CFloat(d)}); return id; }
  uint32_t CVec(uint32_t n, uint32_t scalar) { uint32_t id = Id(); std::vector<uint32_t> w{TVec(TFloat(), n), id}; for (uint32_t i = 0; i < n; i++) w.push_back(scalar); PutVec(types, OpConstantComposite, w); return id; }

  // ---- module level ----
  uint32_t Variable(uint32_t type, uint32_t storage) { uint32_t id = Id(); Put(types, OpVariable, {TPtr(storage, type), id, storage}); return id; }
  void Decorate(uint32_t target, uint32_t deco, std::initializer_list<uint32_t> args = {}) { std::vector<uint32_t> w{target, deco}; w.insert(w.end(), args.begin(), args.end()); PutVec(annot, OpDecorate, w); }
  void MemberDecorate(uint32_t st, uint32_t member, uint32_t deco, std::initializer_list<uint32_t> args = {}) { std::vector<uint32_t> w{st, member, deco}; w.insert(w.end(), args.begin(), args.end()); PutVec(annot, OpMemberDecorate, w); }

  // ---- function body ----
  uint32_t Emit(uint16_t op, uint32_t type, std::initializer_list<uint32_t> w) { uint32_t id = Id(); std::vector<uint32_t> v{type, id}; v.insert(v.end(), w.begin(), w.end()); PutVec(cur, op, v); return id; }
  uint32_t EmitV(uint16_t op, uint32_t type, const std::vector<uint32_t>& w) { uint32_t id = Id(); std::vector<uint32_t> v{type, id}; v.insert(v.end(), w.begin(), w.end()); PutVec(cur, op, v); return id; }
  void EmitNoResult(uint16_t op, std::initializer_list<uint32_t> w) { Put(cur, op, w); }
  void Label(uint32_t id) { Put(cur, OpLabel, {id}); }
  uint32_t Load(uint32_t type, uint32_t ptr) { return Emit(OpLoad, type, {ptr}); }
  void Store(uint32_t ptr, uint32_t val) { Put(cur, OpStore, {ptr, val}); }
  uint32_t Glsl1(uint32_t type, uint32_t inst, uint32_t a) { return Emit(OpExtInst, type, {glsl, inst, a}); }
  uint32_t Glsl2(uint32_t type, uint32_t inst, uint32_t a, uint32_t b) { return Emit(OpExtInst, type, {glsl, inst, a, b}); }
  uint32_t Glsl3(uint32_t type, uint32_t inst, uint32_t a, uint32_t b, uint32_t c) { return Emit(OpExtInst, type, {glsl, inst, a, b, c}); }
  uint32_t Extract(uint32_t type, uint32_t v, uint32_t idx) { return Emit(OpCompositeExtract, type, {v, idx}); }
  uint32_t Vec4(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return Emit(OpCompositeConstruct, TVec4(), {x, y, z, w}); }
  uint32_t Shuffle(uint32_t type, uint32_t a, uint32_t b, std::initializer_list<uint32_t> comps) { std::vector<uint32_t> w{a, b}; w.insert(w.end(), comps.begin(), comps.end()); return EmitV(OpVectorShuffle, type, w); }

  // Begin the entry-point function (void main()); returns after emitting the entry label.
  uint32_t mainFn = 0;
  void BeginMain() { mainFn = Id(); Put(cur, OpFunction, {TVoid(), mainFn, 0, TFunc(TVoid())}); Label(Id()); }
  void EndMain() { Put(cur, OpReturn, {}); Put(cur, OpFunctionEnd, {}); }

  void Header(bool fragment, const std::vector<uint32_t>& interfaceVars) {
    Put(caps, OpCapability, {1});  // Shader
    { std::vector<uint32_t> w{glsl}; const char* n = "GLSL.std.450"; size_t len = strlen(n) + 1; std::vector<uint8_t> b((len + 3) & ~3, 0); memcpy(b.data(), n, len); for (size_t i = 0; i < b.size(); i += 4) { uint32_t x; memcpy(&x, &b[i], 4); w.push_back(x); } PutVec(ext, OpExtInstImport, w); }
    Put(mem, OpMemoryModel, {0, 1});  // Logical, GLSL450
    { std::vector<uint32_t> w{fragment ? 4u : 0u, mainFn}; const char* n = "main"; uint32_t x = 0; memcpy(&x, n, 4); w.push_back(x); w.push_back(0); w.insert(w.end(), interfaceVars.begin(), interfaceVars.end()); PutVec(entry, OpEntryPoint, w); }
    if (fragment) Put(modes, OpExecutionMode, {mainFn, 7});  // OriginUpperLeft
  }
  std::vector<uint32_t> Finish() {
    std::vector<uint32_t> out{0x07230203, 0x00010000, 0, nextId, 0};
    for (auto* v : {&caps, &ext, &mem, &entry, &modes, &annot, &types, &cur}) out.insert(out.end(), v->begin(), v->end());
    return out;
  }
};
