#pragma once
#include <cstdint>
#include <vector>

// Vertex program -> SPIR-V. Inputs: attribute locations for every v register the program reads (usedInputs bit i = attribute i).
// Uniform block (set 0, binding 0): vec4 c[192]; vec4 misc = (surface width, surface height, clip z max, 0).
// Outputs: gl_Position, location 0 d0, 1 d1, 2..5 t0..t3, 6 fog.
std::vector<uint32_t> GenVertexShader(const uint32_t* program, uint32_t startSlot, uint32_t* usedInputs);

// Fixed-function (T&L) state, read from the NV097 method registers; one shader per distinct state. Extra per-draw data comes in the ffp[] uniform array (see vk_ffp.cpp).
struct FfState {
  uint32_t lighting = 0, normalize = 0, localEye = 0, emission = 0, ambient = 0, diffuse = 0, specular = 0, skin = 0, fogOn = 0, fogGen = 0;
  uint32_t texMat[4] = {}, texgen[4][4] = {}, light[8] = {};
};
std::vector<uint32_t> GenFixedVertexShader(const FfState& st, uint32_t* usedInputs);

// Register combiner state (as written by the title) -> fragment shader.
struct FsInfo {
  uint32_t numStages = 0; bool uniqueC0 = false, uniqueC1 = false, muxMsb = false;
  uint32_t icwC[8] = {}, icwA[8] = {}, ocwC[8] = {}, ocwA[8] = {}, final0 = 0, final1 = 0;
  bool texCube[4] = {};   // the stage's texture is a cube map: sampled with the stage's (s,t,r) texture coordinate as a direction
  bool texPresent[4] = {}, texLinear[4] = {}, texDepth[4] = {};   // texDepth: the stage samples a depth buffer (Z24S8 seen as ARGB8: A = z[23:16], R = z[15:8], G = z[7:0], B = stencil)
   uint32_t texMode[4] = {};   // texMode: 0 none, 1 project 2D, 4 passthrough (others sampled as 2D)
  bool alphaTest = false; uint32_t alphaFunc = 0x207;
};
// Fragment uniform block (set 0, binding 5), std140-like with explicit offsets: fac0[8] @0, fac1[8] @128, final0 @256, final1 @272, fogColor @288,
// texSize[4] (xy = texel size of linear textures) @304, alphaRef (x, 0..255) @368. Samplers: set 0, binding 1..4.
std::vector<uint32_t> GenFragmentShader(const FsInfo& info);
std::vector<uint32_t> GenSimpleFragmentShader();
std::vector<uint32_t> GenClearVertexShader();   // full-screen triangle from gl_VertexIndex
std::vector<uint32_t> GenClearFragmentShader(float r, float g, float b, float a);   // constant colour
std::vector<uint32_t> GenSimpleVertexShader(uint32_t* usedInputs);
