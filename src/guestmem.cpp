// Guest-side heap and handle table. Objects the guest keeps pointers to (kernel pool, device objects, HLE COM objects) must live in guest memory so the
// pointer fits in 32 bits on any host; Win32/POSIX handles are handed to the guest as small table indices, never as raw values.
#include "guestmem.h"
#include "kernel.h"
#include <map>
#include <mutex>
#include <vector>
#include <cstring>
#include <cstddef>
#include "plat.h"

extern "C" uintptr_t g_guestBase;
#ifndef GUEST_IDENTITY
// Choose where the 4 GB guest address space sits in this 64-bit process: a window that is entirely free. Everything the guest sees lives at base + guest address.
void GuestMemInit() {
  if (g_guestBase) return;
  static const uint64_t cands[] = {0x100000000ull, 0x200000000ull, 0x400000000ull, 0x800000000ull, 0x10000000000ull, 0x20000000000ull};
  const size_t kWindow = 0x100000000ull;
  for (uint64_t c : cands) {   // a window that is entirely free: reserve it to prove that, then give it back (guest allocations reserve their own pieces)
    void* p = plat::VmReserve((void*)(uintptr_t)c, kWindow);
    if (p) { plat::VmFree(p, 0, MEM_RELEASE); g_guestBase = (uintptr_t)c; break; }
  }
  if (!g_guestBase) { void* p = plat::VmReserve(nullptr, kWindow); if (p) { plat::VmFree(p, 0, MEM_RELEASE); g_guestBase = (uintptr_t)p; } }
  Log("[mm] guest address space at %p", (void*)g_guestBase);
}
#else
void GuestMemInit() {}
#endif
// Reserve/commit host memory that the guest will see, always below the hardware windows (0xFD000000) and the 0x80000000 RAM view where possible.
// In identity mode the host chooses guest-visible addresses; Windows may hand out high ones, which the explicit-MMIO path would misroute.
void* GuestVirtualAlloc(uint32_t size, uint32_t type, uint32_t prot) {
  static const bool anywhere = getenv("XBOX_ALLOC_ANYWHERE") != nullptr; if (anywhere) { void* q = plat::VmAlloc(nullptr, size, type, prot); return q; }   // debug: old behaviour
  type |= MEM_RESERVE;   // no base given: this is a fresh allocation, so reserving is implied (and probing a fixed address for commit-only would hit our own reservations)
  for (uint32_t a = (getenv("XBOX_ALLOC_BASE") ? (uint32_t)strtoul(getenv("XBOX_ALLOC_BASE"), nullptr, 16) : 0x06000000u); a < 0x7C000000u; a += 0x00400000u) {   // probe candidate bases between the XBE and the RAM view
    void* p = plat::VmAlloc(G(a), size, type, prot); if (p) return p;
  }
#ifndef GUEST_IDENTITY
  return nullptr;   // the guest window is full of reservations: nothing else is guest-addressable
#endif
  void* p = plat::VmAlloc(nullptr, size, type, prot);   // last resort (identity builds only)
  if (p && (uintptr_t)p >= 0xFD000000u) { static int n = 0; if (n++ < 5) Log("[mm] WARNING: guest allocation landed in the hardware range (%p)", p); }
  return p;
}
namespace {
std::mutex g_mx;
uint8_t* g_arena = nullptr; uint32_t g_arenaSize = 0, g_top = 0;
std::map<uint32_t, uint32_t> g_free;                    // offset -> size (freed blocks)
std::map<uint32_t, uint32_t> g_used;                    // guest address -> size
const uint32_t kArenaBytes = 192u << 20;
void Init() {
  if (g_arena) return;
  g_arena = (uint8_t*)GuestVirtualAlloc(kArenaBytes, MEM_RESERVE, PAGE_READWRITE);   // reserved; pages are committed as the arena grows
  g_arenaSize = kArenaBytes;
}
}
// Allocate from the guest heap (zeroed, 16-byte aligned). Returns a guest address.
uint32_t GuestAlloc(uint32_t size) {
  std::lock_guard<std::mutex> l(g_mx); Init(); if (!g_arena) { Log("[mm] guest heap unavailable"); return 0; }
  size = (size ? size : 1); size = (size + 15) & ~15u;
  for (auto it = g_free.begin(); it != g_free.end(); ++it) if (it->second >= size) {
    uint32_t off = it->first, len = it->second; g_free.erase(it); if (len > size) g_free[off + size] = len - size;
    uint32_t a = GA(g_arena + off); g_used[a] = size; memset(G(a), 0, size); return a;
  }
  if ((uint64_t)g_top + size > g_arenaSize) { Log("[mm] guest heap exhausted (size 0x%x, top 0x%x)", size, g_top); return 0; }
  uint32_t off = g_top; g_top += size;
  plat::VmAlloc(g_arena + off, size, MEM_COMMIT, PAGE_READWRITE);
  uint32_t a = GA(g_arena + off); g_used[a] = size; return a;
}
uint32_t GuestAllocSize(uint32_t a) { std::lock_guard<std::mutex> l(g_mx); auto it = g_used.find(a); return it == g_used.end() ? 0 : it->second; }
void GuestFree(uint32_t a) {
  std::lock_guard<std::mutex> l(g_mx); auto it = g_used.find(a); if (it == g_used.end()) return;
  uint32_t off = (uint32_t)(G(a) - g_arena), size = it->second; g_used.erase(it);
  auto nx = g_free.lower_bound(off);
  if (nx != g_free.end() && off + size == nx->first) { size += nx->second; nx = g_free.erase(nx); }
  if (nx != g_free.begin()) { auto pv = std::prev(nx); if (pv->first + pv->second == off) { off = pv->first; size += pv->second; g_free.erase(pv); } }
  g_free[off] = size;
}

// ---- handle table: guest-visible 32-bit values for host objects ----
namespace { std::mutex g_hmx; std::vector<void*> g_handles(1); std::vector<uint32_t> g_hfree; }
uint32_t HandleNew(void* h) {
  std::lock_guard<std::mutex> l(g_hmx); uint32_t i;
  if (!g_hfree.empty()) { i = g_hfree.back(); g_hfree.pop_back(); g_handles[i] = h; } else { i = (uint32_t)g_handles.size(); g_handles.push_back(h); }
  return 0x100 + i * 4;   // never 0 or a small pointer-looking value
}
void* HandleGet(uint32_t v) { std::lock_guard<std::mutex> l(g_hmx); if (v < 0x100 || (v & 3)) return nullptr; uint32_t i = (v - 0x100) / 4; return i < g_handles.size() ? g_handles[i] : nullptr; }
void* HandleRelease(uint32_t v) { std::lock_guard<std::mutex> l(g_hmx); if (v < 0x100 || (v & 3)) return nullptr; uint32_t i = (v - 0x100) / 4; if (i >= g_handles.size() || !g_handles[i]) return nullptr; void* h = g_handles[i]; g_handles[i] = nullptr; g_hfree.push_back(i); return h; }

bool GuestPtrOk(const void* p, size_t n) {
#ifndef GUEST_IDENTITY
  if ((uintptr_t)p < g_guestBase || (uintptr_t)p + n > g_guestBase + 0x100000000ull) return false;
#endif
  plat::VmInfo vi; if (!plat::VmQuery(p, &vi)) return false;
  return vi.state == 2 && (vi.prot & plat::kProtRead) && (const uint8_t*)p + n <= (const uint8_t*)vi.base + vi.size;
}
