#include "xbe.h"
#include "guestmem.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

bool XbeLoad(const char* path, XbeInfo* info, char* err, size_t errLen) {
  FILE* f = fopen(path, "rb");
  if (!f) { snprintf(err, errLen, "cannot open %s", path); return false; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* file = (uint8_t*)malloc(n);
  if (fread(file, 1, n, f) != (size_t)n) { fclose(f); snprintf(err, errLen, "short read"); return false; }
  fclose(f);
  if (memcmp(file, "XBEH", 4)) { snprintf(err, errLen, "not an XBE"); return false; }
  auto u32 = [&](uint32_t off) { uint32_t v; memcpy(&v, file + off, 4); return v; };
  uint32_t base = u32(0x104), imgSize = u32(0x10C), certAddr = u32(0x118), nsec = u32(0x11C), secAddr = u32(0x120);
  uint32_t entry = u32(0x128), tls = u32(0x12C), stackCommit = u32(0x130), kthunk = u32(0x158);
  // Retail / debug / Chihiro keys: the entry point (and thunk) must land inside the image to be the right pair.
  uint32_t e = 0, t = 0;
  const struct { uint32_t ek, tk; const char* name; } keys[] = {{0xA8FC57AB, 0x5B6D40B6, "retail"}, {0x94859D4B, 0xEFB1F152, "debug"}, {0x40B5C16C, 0x2290059D, "chihiro"}};
  const char* kind = nullptr;
  for (auto& k : keys) if ((entry ^ k.ek) >= base && (entry ^ k.ek) < base + imgSize) { e = entry ^ k.ek; t = kthunk ^ k.tk; kind = k.name; break; }
  if (!kind) { snprintf(err, errLen, "entry point matches no known key"); return false; }
  // Reserve the whole image range at the XBE's own address: guest pointers are host pointers.
  uint32_t alloc = (imgSize + 0xFFF) & ~0xFFFu;
  if (!VirtualAlloc(G(base), alloc, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
    snprintf(err, errLen, "cannot map image at 0x%x size 0x%x (error %lu)", base, alloc, GetLastError());
    return false;
  }
  uint32_t hdrSize = u32(0x108); memcpy(G(base), file, hdrSize < (uint32_t)n ? hdrSize : n);
  for (uint32_t i = 0; i < nsec; i++) {
    uint32_t o = secAddr - base + i * 56;
    uint32_t va = u32(o + 4), vsize = u32(o + 8), raw = u32(o + 12), rsize = u32(o + 16);
    if (rsize) memcpy(G(va), file + raw, rsize);
    (void)vsize;
  }
  info->base = base; info->entry = e; info->kernelThunk = t; info->stackCommit = stackCommit; info->tlsAddr = tls; info->certAddr = certAddr;
  info->imageEnd = base + imgSize; info->titleId = u32(certAddr - base + 8);
  printf("[xbe] %s keys, base 0x%x, entry 0x%x, thunk 0x%x, tls 0x%x, %u sections, title id 0x%08x\n", kind, base, e, t, tls, nsec, info->titleId);
  free(file);
  return true;
}
