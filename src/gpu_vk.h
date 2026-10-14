#pragma once
#include <cstddef>
#include <cstdint>
struct SoftVertex;
bool GpuVkReady();   // initialises Vulkan on first use; false when unavailable (or XBOX_SOFT=1): callers then use the software renderer
void GpuClear(uint32_t mask, uint32_t colorValue, uint32_t zValue, int x0, int y0, int x1, int y1);
void GpuDraw(uint32_t prim, const SoftVertex* verts, size_t count);
void GpuFlip();      // present the finished render target (title buffer swap)
