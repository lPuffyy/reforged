// Privileged instructions the title executes (it expects ring 0): port I/O, cli/sti, hlt, cache control, MSR access. They fault with
// EXCEPTION_PRIV_INSTRUCTION; we emulate or skip them and continue after the instruction. Port reads return what the modelled device says.
#include "kernel.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>

static int g_portLog = 300;
static std::map<uint32_t, uint64_t> g_privEips; static std::mutex g_privMutex;
void PrivDump() {
  std::lock_guard<std::mutex> l(g_privMutex);
  std::vector<std::pair<uint64_t, uint32_t>> v; for (auto& kv : g_privEips) v.push_back({kv.second, kv.first});
  std::sort(v.rbegin(), v.rend());
  for (size_t i = 0; i < v.size() && i < 10; i++) Log("[priv] %llu x privileged instruction at 0x%08x", (unsigned long long)v[i].first, v[i].second);
}

extern "C" uint32_t PortIn(uint16_t port, int size); extern "C" void PortOut(uint16_t port, uint32_t v, int size);
uint32_t PortIn(uint16_t port, int size) {
  uint32_t v = 0;
  switch (port) {
    case 0xC000: v = 0x10; break;   // SMBus host status: idle / transaction complete bit set so polls finish
    default: v = 0;
  }
  if (port != 0x80c0 && g_portLog > 0) { --g_portLog; Log("[io] in  port 0x%04x (%d) -> 0x%x", port, size, v); }
  return v;
}
void PortOut(uint16_t port, uint32_t v, int size) {
  if (g_portLog > 0) { --g_portLog; Log("[io] out port 0x%04x (%d) <- 0x%x", port, size, v); }
}

#ifndef LIFT_NO_NATIVE
static LONG WINAPI PrivHandler(EXCEPTION_POINTERS* ep) {
  auto* r = ep->ExceptionRecord; auto* c = ep->ContextRecord;
  if (r->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
  { std::lock_guard<std::mutex> l(g_privMutex); ++g_privEips[c->Eip]; }
  const uint8_t* ip = (const uint8_t*)(uintptr_t)c->Eip; int len = 0; bool op16 = false, rep = false;
  for (;; ++len) { if (ip[len] == 0x66) op16 = true; else if (ip[len] == 0xF3 || ip[len] == 0xF2) rep = true; else break; }
  uint8_t op = ip[len]; int size = op16 ? 2 : 4;
  switch (op) {
    case 0xE4: c->Eax = (c->Eax & ~0xFFu) | (PortIn(ip[len + 1], 1) & 0xFF); c->Eip += len + 2; return EXCEPTION_CONTINUE_EXECUTION;
    case 0xE5: { uint32_t v = PortIn(ip[len + 1], size); if (size == 2) c->Eax = (c->Eax & ~0xFFFFu) | (v & 0xFFFF); else c->Eax = v; c->Eip += len + 2; return EXCEPTION_CONTINUE_EXECUTION; }
    case 0xE6: PortOut(ip[len + 1], c->Eax & 0xFF, 1); c->Eip += len + 2; return EXCEPTION_CONTINUE_EXECUTION;
    case 0xE7: PortOut(ip[len + 1], size == 2 ? (c->Eax & 0xFFFF) : c->Eax, size); c->Eip += len + 2; return EXCEPTION_CONTINUE_EXECUTION;
    case 0xEC: c->Eax = (c->Eax & ~0xFFu) | (PortIn((uint16_t)c->Edx, 1) & 0xFF); c->Eip += len + 1; return EXCEPTION_CONTINUE_EXECUTION;
    case 0xED: { uint32_t v = PortIn((uint16_t)c->Edx, size); if (size == 2) c->Eax = (c->Eax & ~0xFFFFu) | (v & 0xFFFF); else c->Eax = v; c->Eip += len + 1; return EXCEPTION_CONTINUE_EXECUTION; }
    case 0xEE: PortOut((uint16_t)c->Edx, c->Eax & 0xFF, 1); c->Eip += len + 1; return EXCEPTION_CONTINUE_EXECUTION;
    case 0xEF: PortOut((uint16_t)c->Edx, size == 2 ? (c->Eax & 0xFFFF) : c->Eax, size); c->Eip += len + 1; return EXCEPTION_CONTINUE_EXECUTION;
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: {  // ins/outs: consume the whole rep count without touching memory (zeros in, discard out)
      int sz = (op & 1) ? size : 1; uint32_t n = rep ? c->Ecx : 1;
      if (op <= 0x6D) { memset((void*)(uintptr_t)c->Edi, 0, n * sz); c->Edi += n * sz; } else c->Esi += n * sz;
      if (rep) c->Ecx = 0; c->Eip += len + 1; return EXCEPTION_CONTINUE_EXECUTION;
    }
    case 0xFA: case 0xFB: case 0xF4: c->Eip += len + 1; return EXCEPTION_CONTINUE_EXECUTION;  // cli / sti / hlt (hlt waits for an interrupt: just yield)
    case 0x0F: {
      uint8_t op2 = ip[len + 1];
      if (op2 == 0x09 || op2 == 0x08 || op2 == 0x30 || op2 == 0x32 || op2 == 0x30) {  // wbinvd, invd, wrmsr, rdmsr
        if (op2 == 0x32) { c->Eax = 0; c->Edx = 0; }
        c->Eip += len + 2; return EXCEPTION_CONTINUE_EXECUTION;
      }
      if (op2 == 0x20 || op2 == 0x22) {  // mov r32, crN / mov crN, r32 (modrm: 1 more byte)
        if (op2 == 0x20) { uint8_t m = ip[len + 2]; DWORD* regs[8] = {&c->Eax, &c->Ecx, &c->Edx, &c->Ebx, &c->Esp, &c->Ebp, &c->Esi, &c->Edi}; *regs[m & 7] = 0x80000011; }
        c->Eip += len + 3; return EXCEPTION_CONTINUE_EXECUTION;
      }
      break;
    }
  }
  Log("[priv] unhandled privileged instruction at 0x%08lx: %02x %02x %02x %02x", c->Eip, ip[0], ip[1], ip[2], ip[3]);
  return EXCEPTION_CONTINUE_SEARCH;
}

void PrivInit() { AddVectoredExceptionHandler(1, PrivHandler); }
#else
void PrivInit() {}   // lifted code executes port I/O and cli/sti itself
#endif
