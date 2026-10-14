// DirectSound HLE: the DSOUND library inside default.xbe drives the MCPX audio processor and its DSP, which we do not emulate. Instead the title's
// calls into the library's entry points (found with XbSymbolDatabase, listed in work/title/dsound_hle.txt) are redirected to host functions here.
// Objects are plain host memory blocks. Sound is not produced yet: buffers and streams accept data and report completion so the game keeps running.
#include "kernel.h"
#include "guestmem.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <cstdlib>

namespace {
// Host-side object layout (the game only passes the pointers back to us): +0 vtable (unused), +4 data pointer, +8 data size, +0xC kind (1 buffer, 2 stream)
uint32_t NewObject(uint32_t kind) {
  uint32_t oA = GuestAlloc(0x100); uint8_t* o = G(oA);
  *(uint32_t*)(o + 0xC) = kind;
  if (kind == 2) *(uint32_t*)o = 0x417170;  // CDirectSoundStream vtable in the XBE (its methods are patched to our hooks)
  return oA;
}
int g_log = 40;
void Note(const char* what) { if (g_log > 0) { --g_log; Log("[dsound] %s", what); } }
}  // namespace

void LiftAddOverride(uint32_t addr, void* fn, int argBytes, uint32_t ptrMask = 0);
namespace {
struct HookEntry { const char* name; void* fn; };
std::vector<HookEntry>& Hooks() { static std::vector<HookEntry> h; return h; }
}  // namespace
void HleRegister(const char* name, void* fn) { Hooks().push_back({name, fn}); }
static std::string g_names[128]; static int g_nameCount = 0; static int g_traceBudget = 400;
extern "C" void __cdecl DsoundTrace(int idx) { if (getenv("XBOX_DSTRACE") && g_traceBudget > 0) { --g_traceBudget; Log("[dsound] stub call %s", g_names[idx].c_str()); } }

// Trampoline for a stubbed entry point: log the call (optional), return 0 and pop N bytes of arguments.
static void* MakeStubTrampoline(int idx, int ret) {
  static uint8_t* pool = nullptr; static size_t used = 0;
  if (!pool) pool = (uint8_t*)VirtualAlloc(nullptr, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  uint8_t* p = pool + used; uint8_t* s = p;
  *s++ = 0x68; memcpy(s, &idx, 4); s += 4;                                         // push idx
  *s++ = 0xE8; int32_t rel = (int32_t)((uintptr_t)&DsoundTrace - ((uintptr_t)s + 4)); memcpy(s, &rel, 4); s += 4;  // call DsoundTrace
  *s++ = 0x83; *s++ = 0xC4; *s++ = 0x04;                                           // add esp, 4
  *s++ = 0x33; *s++ = 0xC0;                                                        // xor eax, eax
  if (ret == 0) *s++ = 0xC3; else { *s++ = 0xC2; *s++ = (uint8_t)ret; *s++ = (uint8_t)(ret >> 8); }
  used += 32; return p;
}

int DsoundHleInstall(const char* file) {
  FILE* f = fopen(file, "r");
  if (!f) { Log("[dsound] %s not found: run tools/gen_dsound_hle.py (DSOUND will run on the unemulated APU and stall)", file); return 0; }
  char line[256]; int hooked = 0, stubbed = 0;
  while (fgets(line, sizeof line, f)) {
    unsigned addr; char name[128]; int ret;
    if (sscanf(line, "%x %127s %d", &addr, name, &ret) != 3) continue;
    // arg counts of the four jump-thunk entry points, which the scanner could not see
    if (ret < 0) ret = !strcmp(name, "XAudioDownloadEffectsImage") ? 16 : !strcmp(name, "XAudioSetEffectData") ? 12 : 8;
    const HookEntry* h = nullptr;
    for (const HookEntry& e : Hooks()) if (!strcmp(e.name, name)) h = &e;
    if (h) { { uint32_t pm = HlePtrMask(name); LiftAddOverride(addr, h->fn, ret, pm); LiftAddOverride(GA(h->fn), h->fn, ret, pm); }   // guest entry point, and the host pointer that HLE-created vtables hand out
#ifdef GUEST_IDENTITY
       { uint8_t* p = G(addr); int32_t rel = (int32_t)((uintptr_t)h->fn - (addr + 5)); p[0] = 0xE9; memcpy(p + 1, &rel, 4); }   // native fallback: jump to the hook
#endif
       ++hooked; }
    else {
      int idx = g_nameCount < 128 ? g_nameCount++ : 127; g_names[idx] = name; LiftAddOverride(addr, nullptr, ret);
#ifdef GUEST_IDENTITY
      { void* t = MakeStubTrampoline(idx, ret); LiftAddOverride(GA(t), nullptr, ret); uint8_t* p = G(addr); int32_t rel = (int32_t)((uintptr_t)t - (addr + 5)); p[0] = 0xE9; memcpy(p + 1, &rel, 4); }
#endif
      ++stubbed;
    }
  }
  fclose(f);
  Log("[dsound] HLE installed: %d hooked, %d stubbed (returns S_OK)", hooked, stubbed);
  return hooked + stubbed;
}
