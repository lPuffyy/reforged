// Display (portable part): shows the frame buffer the CRTC scans out (NV_PCRTC_START, a physical address in the 64 MB RAM) or the snapshot taken at the
// title's last buffer swap. Putting pixels on screen is the platform backend's job (display.h).
#include "kernel.h"
#include "display.h"
#include "plat.h"
#include <cstdio>
#include <cstring>
#include <vector>

uint32_t MmioGet(uint32_t reg);
bool PgraphGetFront(std::vector<uint32_t>& out, int& w, int& h);
bool GpuVkActive();
uint8_t* KernelRamBase();
extern uint32_t g_avFramebufferForDisplay, g_avFormatForDisplay, g_avPitchForDisplay;

namespace {
uint32_t g_width = 640, g_height = 480;
}

void DisplayTick() {
  if (GpuVkActive()) return;   // the Vulkan swapchain owns the window
  { // preferred: the snapshot taken at the title's last buffer swap
    static std::vector<uint32_t> img; int fw, fh;
    if (PgraphGetFront(img, fw, fh)) { DisplayBlit(img.data(), fw, fh); return; }
  }
  uint32_t start = MmioGet(0x600800) & 0x03FFFFFF;
  if (!start) start = g_avFramebufferForDisplay & 0x03FFFFFF;
  if (!start) return;
  uint32_t pitch = g_avPitchForDisplay ? g_avPitchForDisplay : g_width * 4;
  bool is16 = g_avFormatForDisplay == 0x11 || g_avFormatForDisplay == 0x10 || g_avFormatForDisplay == 0x1C;
  if (is16 && pitch >= g_width * 4) is16 = false;  // pitch decides: 16-bit buffers have pitch = width * 2
  static uint32_t* pix = new uint32_t[640 * 480];
  const uint8_t* src = KernelRamBase() + start;
  for (uint32_t y = 0; y < g_height; y++) {
    const uint8_t* row = src + (uint64_t)y * (is16 ? g_width * 2 : g_width * 4);
    if (pitch != g_width * 4 && pitch != g_width * 2) row = src + (uint64_t)y * pitch;
    for (uint32_t x = 0; x < g_width; x++) {
      if (is16) { uint16_t v = *(const uint16_t*)(row + x * 2); pix[y * g_width + x] = ((v >> 11) << 19 | ((v >> 5) & 0x3F) << 10 | (v & 0x1F) << 3) & 0xFFFFFF; }
      else pix[y * g_width + x] = *(const uint32_t*)(row + x * 4) & 0xFFFFFF;
    }
  }
  { static int n = 0; if (getenv("XBOX_FBLOG") && (n++ % 60) == 0) { uint32_t* px = (uint32_t*)src; Log("[display] start=0x%x pitch=%u fmt=0x%x is16=%d px0=%08x px[1000]=%08x px[100000]=%08x", start, pitch, g_avFormatForDisplay, (int)is16, px[0], px[1000], px[100000]); } }
  DisplayBlit(pix, (int)g_width, (int)g_height);
}

void DumpBmp(const char* path, const uint32_t* argb, int w, int h) {
  FILE* f = fopen(path, "wb"); if (!f) return;
  uint32_t imgSize = (uint32_t)w * h * 4, fileSize = 54 + imgSize; uint8_t hdr[54] = {'B', 'M'};
  memcpy(hdr + 2, &fileSize, 4); uint32_t off = 54, dib = 40; memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4);
  int32_t ww = w, hh = -h; memcpy(hdr + 18, &ww, 4); memcpy(hdr + 22, &hh, 4); uint16_t planes = 1, bpp = 32; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2); memcpy(hdr + 34, &imgSize, 4);
  fwrite(hdr, 1, 54, f); fwrite(argb, 4, (size_t)w * h, f); fclose(f);
}
