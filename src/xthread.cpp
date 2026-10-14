// Per-thread Xbox state. Xbox code reads the KPCR through fs:[4] (TLS slot array), fs:[0x20] (Prcb), fs:[0x24] (Irql), fs:[0x28] (CurrentThread -> KTHREAD) and so on, which collide
// with Windows TEB fields (process id, thread id...). The 36 such reads in default.xbe are replaced by a call to a tiny stub that reads our own
// per-thread KPCR, found through fs:[0x14] (NT_TIB.ArbitraryUserPointer, unused here). fs:[0] (SEH chain) and fs:[4] (stack base) share Windows' layout.
#include "kernel.h"
#include "guestmem.h"
#include "plat.h"
#include <cstdio>
#include <cstring>

static uint32_t g_tlsTemplateSize = 0, g_tlsZeroFill = 0;
void XThreadSetTls(uint32_t tlsDirAddr) {
  if (!tlsDirAddr) { g_tlsTemplateSize = 0; g_tlsZeroFill = 0; return; }
  const uint32_t* d = (const uint32_t*)G(tlsDirAddr);  // start, end, index addr, callbacks, zero fill
  g_tlsTemplateSize = d[1] - d[0]; g_tlsZeroFill = d[4];
  Log("[xthread] TLS template %u bytes + %u zero-fill", g_tlsTemplateSize, g_tlsZeroFill);
}

// Layout used by the patched reads: pcr+0x00 NT_TIB, +0x1C self, +0x20 Prcb (-> pcr+0x28), +0x24 Irql, +0x28 Prcb.CurrentThread.
static thread_local uint32_t t_pcr = 0;
uint32_t XPcrGet() { return t_pcr; }
void XPcrSet(uint32_t pcr) {
  t_pcr = pcr;
#if defined(_M_IX86) && !defined(LIFT_NO_NATIVE)
  __writefsdword(0x14, pcr);   // native-fallback stubs read it through fs:[0x14]
#endif
}
void XThreadInit(uint32_t tlsDataSize) {
  // Xbox thread-local data: the kernel allocates TlsData (size = TlsDataSize given at thread creation); the title addresses it as [fs:4 + negative_index*4],
  // so fs:[4] must equal TlsData + TlsDataSize (the CRT computes index = -TlsDataSize/4 and stores a self pointer in TlsData[0]).
  if (tlsDataSize == 0) tlsDataSize = 0x200;
  uint32_t pcrA = GuestAlloc(0x2000), tlsA = GuestAlloc((tlsDataSize + 0x1FFF) & ~0xFFFu);   // guest heap: these addresses are visible to guest code
  uint8_t* pcr = G(pcrA); uint8_t* tls = G(tlsA); uint32_t kthreadA = pcrA + 0x1000; uint8_t* kthread = G(kthreadA);
  *(uint32_t*)(pcr + 0x04) = tlsA + tlsDataSize;
  *(uint32_t*)G(tlsA + tlsDataSize) = tlsA;   // slot 0 -> the TLS block (titles without a TLS directory, e.g. nxdk homebrew, use index 0)
  *(uint32_t*)(pcr + 0x1C) = pcrA;
  *(uint32_t*)(pcr + 0x20) = pcrA + 0x28;
  *(uint32_t*)(pcr + 0x28) = kthreadA;
  *(uint32_t*)(kthread + 0x28) = tlsA;
  XPcrSet(pcrA);
}

#ifndef LIFT_NO_NATIVE   // x86 machine-code stubs for the native-fallback build
static void* MakeStub(int reg, int width, uint32_t disp) {
  static uint8_t* pool = nullptr; static size_t used = 0;
  if (!pool) pool = (uint8_t*)plat::VmAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  uint8_t* p = pool + used; uint8_t* s = p;
  *s++ = 0x64; *s++ = 0x8B; *s++ = (uint8_t)(0x05 | (reg << 3)); *s++ = 0x14; *s++ = 0; *s++ = 0; *s++ = 0;  // mov reg, fs:[0x14]
  if (width == 4) { *s++ = 0x8B; } else { *s++ = 0x0F; *s++ = 0xB6; }                                          // mov / movzx reg, [reg + disp32]
  *s++ = (uint8_t)(0x80 | (reg << 3) | reg); memcpy(s, &disp, 4); s += 4;
  *s++ = 0xC3;
  used += 32; return p;
}
#endif

#ifdef LIFT_NO_NATIVE
int XThreadPatch(const char*) { return 0; }   // lifted code reads the thread block through LiftCpu::fsbase: nothing to patch
#else
int XThreadPatch(const char* listFile) {
  FILE* f = fopen(listFile, "r");
  if (!f) { Log("[xthread] %s not found: fs:[] reads are not patched (run tools/scan_fs.py)", listFile); return 0; }
  unsigned va, size, reg, width, disp; int n = 0;
  while (fscanf(f, "%x %u %u %u %x", &va, &size, &reg, &width, &disp) == 5) {
    uint8_t* site = (uint8_t*)(uintptr_t)va; void* stub = MakeStub((int)reg, (int)width, disp);
    int32_t rel = (int32_t)((uintptr_t)stub - (va + 5));
    site[0] = 0xE8; memcpy(site + 1, &rel, 4);
    for (unsigned i = 5; i < size; i++) site[i] = 0x90;
    ++n;
  }
  fclose(f);
  Log("[xthread] patched %d fs: reads", n);
  return n;
}

#endif

// Byte patches from a text file: "<hex address> <hex bytes>" per line ('#' comments). Used to short-circuit hardware waits in statically linked XDK code.
int PatchesApply(const char* file) {
  FILE* f = fopen(file, "r");
  if (!f) { Log("[patch] %s not found", file); return 0; }
  char line[512]; int n = 0;
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    unsigned addr; char hex[400];
    if (sscanf(line, "%x %399s", &addr, hex) != 2) continue;
    uint8_t* p = G(addr);
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) { unsigned b; sscanf(hex + i, "%2x", &b); *p++ = (uint8_t)b; }
    ++n;
  }
  fclose(f);
  Log("[patch] applied %d byte patches from %s", n, file);
  return n;
}
