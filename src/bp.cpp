// Debug breakpoints on guest code: XBOX_BP="addr,addr,..." (hex). Each hit logs registers and the top of the stack (first 20 hits per address), then execution continues.
#include "kernel.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#ifdef LIFT_NO_NATIVE
void BpInit() {}   // the breakpoint tool patches x86 code bytes: not applicable to lifted code
#else
namespace {
struct Bp { uint8_t orig; int hits; };
std::map<uint32_t, Bp> g_bps;
std::mutex g_bpMutex;
thread_local uint32_t t_rearm = 0;

LONG WINAPI BpHandler(EXCEPTION_POINTERS* ep) {
  auto* r = ep->ExceptionRecord; auto* c = ep->ContextRecord;
  if (r->ExceptionCode == EXCEPTION_BREAKPOINT) {
    uint32_t a = c->Eip; std::lock_guard<std::mutex> l(g_bpMutex);
    auto it = g_bps.find(a);
    if (it == g_bps.end()) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old; VirtualProtect((void*)(uintptr_t)a, 1, PAGE_EXECUTE_READWRITE, &old);
    *(uint8_t*)(uintptr_t)a = it->second.orig;
    if (it->second.hits++ < 20) {
      uint32_t* sp = (uint32_t*)c->Esp;
      Log("[bp] 0x%x hit %d tid=%lu eax=%08lx ecx=%08lx edx=%08lx ebx=%08lx esi=%08lx edi=%08lx ebp=%08lx stack: %08x %08x %08x %08x %08x %08x", a, it->second.hits, GetCurrentThreadId(),
          c->Eax, c->Ecx, c->Edx, c->Ebx, c->Esi, c->Edi, c->Ebp, sp[0], sp[1], sp[2], sp[3], sp[4], sp[5]);
      if (getenv("XBOX_BP_FRAME") && !IsBadReadPtr((void*)(uintptr_t)(c->Ebp - 0x40), 0x50)) { const uint32_t* f = (const uint32_t*)(uintptr_t)(c->Ebp - 0x40); Log("[bp]   frame[ebp-0x40..+0x10]: %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15], f[16], f[17], f[18], f[19]); }
      if (getenv("XBOX_BP_DEREF")) {   // also dump 8 dwords behind each stack argument that looks like a pointer (XBOX_BP_DEREF=1)
        for (int k = 1; k <= 4; k++) { uint32_t v = sp[k]; if (v > 0x10000 && v < 0xF0000000u && !IsBadReadPtr((void*)(uintptr_t)v, 32)) { const uint32_t* q = (const uint32_t*)(uintptr_t)v; Log("[bp]   arg%d=%08x -> %08x %08x %08x %08x %08x %08x %08x %08x", k, v, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
            for (int j = 0; j < 8; j++) { uint32_t w = q[j]; if (w > 0x10000 && w < 0xF0000000u && !IsBadReadPtr((void*)(uintptr_t)w, 16)) { const uint32_t* r2 = (const uint32_t*)(uintptr_t)w; Log("[bp]      arg%d[%d]=%08x -> %08x %08x %08x %08x", k, j, w, r2[0], r2[1], r2[2], r2[3]); } } } }
      }
    }
    c->EFlags |= 0x100; t_rearm = a;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && t_rearm) {
    *(uint8_t*)(uintptr_t)t_rearm = 0xCC; t_rearm = 0; c->EFlags &= ~0x100u;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void BpInit() {
  const char* env = getenv("XBOX_BP");
  if (!env) return;
  char buf[512]; strncpy(buf, env, sizeof buf - 1); buf[sizeof buf - 1] = 0;
  AddVectoredExceptionHandler(1, BpHandler);
  for (char* tok = strtok(buf, ","); tok; tok = strtok(nullptr, ",")) {
    unsigned a = 0; if (sscanf(tok, "%x", &a) != 1) continue;
    uint8_t* p = (uint8_t*)(uintptr_t)a; g_bps[a] = {*p, 0}; *p = 0xCC;
  }
  Log("[bp] %zu breakpoints armed", g_bps.size());
}
#endif
