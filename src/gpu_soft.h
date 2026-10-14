#pragma once
#include <cstddef>
#include <cstdint>
struct SoftVertex { float a[16][4]; };                      // the 16 vertex attributes (position, weight, normal, diffuse, specular, fog, point size, back diffuse/specular, tex0-3, ...)
void SoftDraw(uint32_t prim, const SoftVertex* verts, size_t count);  // software reference renderer (gpu_soft.cpp)
void DumpBmp(const char* path, const uint32_t* argb, int w, int h);  // display.cpp: 32-bit BMP writer (debugging)
bool SoftTextureDesc(int stage, uint64_t* key, int* w, int* h, bool* linear);   // identity of the bound texture without hashing its contents
bool SoftScreenBBox(const SoftVertex* verts, size_t n, float* x0, float* y0, float* x1, float* y1);   // debug helper
bool SoftFirstVertexOut(const SoftVertex* verts, size_t n, float* pos, float* d0, float* d1, float* t0);   // debug helper
