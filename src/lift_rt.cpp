// Runtime for lifted guest code: dispatch (lifted function / kernel import / native fallback), host->guest calls, atomics.
// Enabled with XBOX_LIFT=1 (hybrid mode on the Windows host: guest memory is real memory, unlifted functions still run natively).
#include "lift_rt.h"
#include "kernel.h"
#include "plat.h"
#ifndef LIFT_NO_NATIVE   // native-fallback and self-test code needs the Windows exception/thread-context API
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if !defined(_MSC_VER) && (defined(__x86_64__) || defined(__i386__))
#include <x86intrin.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <mutex>
#include <atomic>

struct LiftEntry { uint32_t addr; LiftFn fn; uint32_t hi; uint32_t flags; };
#ifdef HAVE_LIFT
extern "C" const LiftEntry g_liftTable[];
extern "C" const unsigned g_liftCount;
#else
static const LiftEntry g_liftTable[] = {{0, 0, 0, 0}}; static const unsigned g_liftCount = 0;
#endif

static std::unordered_map<uint32_t, LiftFn> g_lifted;
static std::unordered_map<uintptr_t, const KernelImport*> g_kfn;
static bool g_on = false, g_trace = false, g_stats = false;
static std::vector<std::pair<uint32_t, uint32_t>> g_codeRegions;   // executable XBE sections: write-protected during the self-test
static std::vector<std::pair<uint32_t, uint32_t>> g_dataRegions;   // writable XBE sections (for the self-test's snapshot/restore)
static std::unordered_map<uint32_t, int> g_skip;   // XBOX_LIFT_SKIP=hex,hex: functions kept native (known-bad lifts)
static volatile int32_t g_nLifted = 0, g_nKernel = 0, g_nNative = 0;
static std::unordered_map<uint32_t, long> g_nativeCount; static std::mutex g_nativeMx;

extern "C" volatile long long g_liftIcount = 0;
extern "C" uintptr_t g_guestBase = 0;
// Guest-visible address of a kernel import. Identity builds use the host function pointer (native code can call it); otherwise a synthetic address that LiftDispatch maps back.
uint32_t KernelImportAddr(const KernelImport* k) {
#ifdef GUEST_IDENTITY
  return (uint32_t)(uintptr_t)k->fn;
#else
  return 0xFFC00000u + (uint32_t)(k - g_kernelImports) * 8;
#endif
}
static void DumpAllRings();
bool LiftEnabled() { return g_on; }
// The translated code: linked into this executable (HAVE_LIFT) or loaded at startup from a module built separately (XBOX_LIFT_MODULE=path).
static const LiftEntry* g_tab = nullptr; static unsigned g_tabCount = 0;
extern "C" void LiftDispatch(LiftCpu* c, uint32_t target); extern "C" void LiftUnsupported(uint32_t addr, const char* what); extern "C" void LiftHere(uint32_t addr);
static bool LoadLiftModule(const char* path) {
  void* lib = plat::LoadLib(path); if (!lib) { Log("[lift] cannot load module %s", path); return false; }
  typedef int (*InitFn)(const HostApi*, const LiftEntry**, unsigned*);
  InitFn init = (InitFn)plat::LibSym(lib, "reforged_module_init"); if (!init) { Log("[lift] %s has no reforged_module_init", path); return false; }
  static HostApi api = {};
  api.abi = LIFT_ABI_VERSION; api.guestBase = g_guestBase; api.dispatch = LiftDispatch; api.unsupported = LiftUnsupported; api.here = LiftHere; api.rdtsc = LiftRdtsc;
  api.xadd32 = LiftXadd32; api.cmpxchg32 = LiftCmpxchg32; api.xchg32 = LiftXchg32; api.lock32 = LiftLock32;
  api.mmioRead = MmioRead; api.mmioWrite = MmioWrite; api.portIn = PortIn; api.portOut = PortOut;
  const LiftEntry* t = nullptr; unsigned n = 0;
  if (init(&api, &t, &n) != 0) { Log("[lift] module %s was built for a different runtime ABI", path); return false; }
  g_tab = t; g_tabCount = n; Log("[lift] module %s: %u translated functions", path, n); return true;
}
// Dispatch overrides: guest addresses whose code is replaced by a host stdcall function (the DSOUND/XInput HLE). On iOS there are no byte patches, so this is the mechanism.
static std::unordered_map<uint32_t, KernelImport*> g_over; void LiftAddOverride(uint32_t addr, void* fn, int argBytes, uint32_t ptrMask = 0);
uint32_t LiftSynthAddr(void* fn, int argBytes, uint32_t ptrMask) {   // a fresh synthetic guest address (never executable memory) that dispatches to a host function
  static std::mutex mx; static uint32_t next = 0xFFE00000u; std::lock_guard<std::mutex> l(mx); uint32_t a = next; next += 16; LiftAddOverride(a, fn, argBytes, ptrMask); return a;
}
void LiftAddOverride(uint32_t addr, void* fn, int argBytes, uint32_t ptrMask) {
  KernelImport* k = new KernelImport{0, "hook", 's', fn, argBytes / 4, ptrMask};   // fn == nullptr: a stub that returns 0 (S_OK) and pops its arguments
  g_over[addr] = k;
  g_lifted.erase(addr);   // an override must win over the lifted body (the 32-bit build gets this from the byte patch that disables the lifted copy)
}
static void LiftSelfTest();
static thread_local bool t_quiet = false;
extern "C" bool LiftQuiet() { return t_quiet; }

// Disable lifted functions whose bytes in memory differ from the file (the host patches the image: hooks, hardware-wait shortcuts, fs: reads).
void LiftInit(const char* xbePath) {
  if (!getenv("XBOX_LIFT")) return;
  if (const char* m = getenv("XBOX_LIFT_MODULE")) LoadLiftModule(m);
  if (!g_tab) { g_tab = g_liftTable; g_tabCount = g_liftCount; }
  FILE* f = fopen(xbePath, "rb"); if (!f) { Log("[lift] cannot open %s", xbePath); return; }
  std::vector<uint8_t> d; { fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); d.resize(n); fread(d.data(), 1, n, f); fclose(f); }
  uint32_t base = *(uint32_t*)&d[0x104], nsec = *(uint32_t*)&d[0x11C], sa = *(uint32_t*)&d[0x120];
  struct Sec { uint32_t v, vs, raw, rs; }; std::vector<Sec> secs;
  for (uint32_t i = 0; i < nsec; i++) { const uint32_t* h = (const uint32_t*)&d[sa - base + i * 56]; secs.push_back({h[1], h[2], h[3], h[4]}); if ((h[0] & 1) && !(h[0] & 4)) g_dataRegions.push_back({h[1], h[2]}); if (h[0] & 4) g_codeRegions.push_back({h[1], h[2]}); }
  g_dataRegions.push_back({base, 0x2000});   // the image headers are writable memory too (small guest addresses land here)
  // Same view of the image as the lifter: byte patches applied; fs: read patches (replaced by stub calls) are ignored (the lifted code reads the real fs: through LiftCpu::fsbase).
  const char* dir = getenv("XBOX_TITLE_DIR"); if (!dir) dir = "work/title"; char pf[300];
  snprintf(pf, sizeof pf, "%s/patches.txt", dir);
  if (FILE* pp = fopen(pf, "r")) { char line[512]; while (fgets(line, sizeof line, pp)) { if (line[0] == '#') continue; unsigned va; char hex[400]; if (sscanf(line, "%x %399s", &va, hex) != 2) continue;
    for (auto& s : secs) if (va >= s.v && va < s.v + s.rs) { for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) { unsigned b; sscanf(hex + i, "%2x", &b); d[s.raw + va - s.v + i / 2] = (uint8_t)b; } break; } } fclose(pp); }
  std::vector<std::pair<uint32_t, uint32_t>> fsSites;
  snprintf(pf, sizeof pf, "%s/fs_patches.txt", dir);
  if (FILE* fp = fopen(pf, "r")) { unsigned va, size, r, w, disp; while (fscanf(fp, "%x %u %u %u %x", &va, &size, &r, &w, &disp) == 5) fsSites.push_back({va, size}); fclose(fp); }
  if (const char* sk = getenv("XBOX_LIFT_SKIP")) { const char* q = sk; while (*q) { unsigned v = 0; if (sscanf(q, "%x", &v) == 1) g_skip[v] = 1; while (*q && *q != ',') q++; if (*q == ',') q++; } }
  unsigned holeLo = 0, holeHi = 0; if (const char* h = getenv("XBOX_LIFT_HOLE")) sscanf(h, "%u,%u", &holeLo, &holeHi);
  int patched = 0; unsigned lo = 0, hi = g_tabCount; if (const char* r = getenv("XBOX_LIFT_RANGE")) sscanf(r, "%u,%u", &lo, &hi);   // bisection aid: only table entries [lo,hi) run lifted
  for (unsigned i = 0; i < g_tabCount; i++) {
    const LiftEntry& e = g_tab[i]; bool same = i >= lo && i < hi;
    for (auto& s : secs) if (same && e.addr >= s.v && e.addr < s.v + s.rs) {
      uint32_t n = e.hi > e.addr ? e.hi - e.addr : 16; if (e.addr + n > s.v + s.rs) n = s.v + s.rs - e.addr;
      for (uint32_t k = 0; k < n && same; k++) {
        uint32_t a = e.addr + k; bool ign = false; for (auto& fs : fsSites) if (a >= fs.first && a < fs.first + fs.second) { ign = true; break; }
        if (!ign && *(const uint8_t*)G(a) != d[s.raw + a - s.v]) same = false;
      } break;
    }
    if (same && g_skip.count(e.addr)) same = false;
    if (same && g_over.count(e.addr)) same = false;
    if (same && i >= holeLo && i < holeHi && e.addr != 0x2d212e) same = false;   // bisection: disable table indices [holeLo,holeHi) (the thread root stays lifted)
    if (same) g_lifted[e.addr] = e.fn; else { patched++; if (patched <= 80 && getenv("XBOX_LIFT_STATS")) Log("[lift] disabled (differs from the file or filtered): f_%08x", e.addr); }
  }
  for (int i = 0; i < g_kernelImportCount; i++) if (g_kernelImports[i].fn) g_kfn[KernelImportAddr(&g_kernelImports[i])] = &g_kernelImports[i];
  if (getenv("XBOX_LIFT_WATCH")) plat::ThreadSpawn([](void*) -> uint32_t { int secs = atoi(getenv("XBOX_LIFT_WATCH")); for (;;) { plat::SleepMs(secs * 1000); Log("[watch] ---- thread trails ----"); DumpAllRings(); } });
  g_on = true; g_trace = getenv("XBOX_LIFT_TRACE") != nullptr; g_stats = getenv("XBOX_LIFT_STATS") != nullptr;
  if (g_stats) plat::ThreadSpawn([](void*) -> uint32_t { long lastL = 0, lastK = 0, lastN = 0; for (;;) { plat::SleepMs(10000); long l = g_nLifted, k = g_nKernel, n = g_nNative; { static long long li = 0; long long ic = g_liftIcount; Log("[lift] guest instructions: %.1f M per second", (ic - li) / 10e6); li = ic; }
      Log("[lift] stats (10 s): %ld lifted calls, %ld kernel calls, %ld native calls", l - lastL, k - lastK, n - lastN); lastL = l; lastK = k; lastN = n; std::vector<std::pair<long, uint32_t>> top; { std::lock_guard<std::mutex> lk(g_nativeMx); for (auto& e : g_nativeCount) top.push_back({e.second, e.first}); } std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; }); char b[600] = ""; for (size_t i = 0; i < top.size() && i < 12; i++) { char t[32]; snprintf(t, sizeof t, " %x:%ld", top[i].second, top[i].first); strcat(b, t); } Log("[lift] hottest native targets:%s", b); if (FILE* nf = fopen("work/lift/native_targets.txt", "w")) { std::lock_guard<std::mutex> lk(g_nativeMx); for (auto& e : g_nativeCount) fprintf(nf, "%08x\n", e.first); fclose(nf); } } });
  if (getenv("XBOX_LIFT_SELFTEST")) { plat::Obj* th = plat::ThreadStart([](void*) -> uint32_t { LiftSelfTest(); return 0; }, nullptr, 8 << 20, false, nullptr); plat::Wait(th, plat::kInfinite); }   // own thread: a big committed stack for the trial frames
  Log("[lift] %u lifted functions available (%d disabled: patched in memory)", (unsigned)g_lifted.size(), patched);
}

extern "C" void LiftUnsupported(uint32_t addr, const char* what) { Log("[lift] unsupported at 0x%x: %s", addr, what); }
extern "C" void LiftHere(uint32_t addr) { Log("[lift] at 0x%x", addr); }
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
extern "C" uint64_t LiftRdtsc() { return __rdtsc(); }
#else
extern "C" uint64_t LiftRdtsc() { return plat::PerfCounter() * 733 / 1000; }   // guest cycles at the original CPU's 733 MHz
#endif
extern "C" uint32_t LiftXadd32(uint32_t a, uint32_t v) { return (uint32_t)plat::AtomicAdd((volatile int32_t*)G(a), (int32_t)v); }
extern "C" uint32_t LiftCmpxchg32(uint32_t a, uint32_t* expected, uint32_t v) { return (uint32_t)plat::AtomicCas((volatile int32_t*)G(a), (int32_t)*expected, (int32_t)v); }
extern "C" void LiftXchg32(uint32_t a, uint32_t* v) { *v = (uint32_t)plat::AtomicExchange((volatile int32_t*)G(a), (int32_t)*v); }
extern "C" uint32_t LiftLock32(int op, uint32_t a, uint32_t v) {   // returns the old value
  volatile int32_t* p = (volatile int32_t*)G(a);
  switch (op) { case 0: return (uint32_t)plat::AtomicAdd(p, (int32_t)v); case 1: return (uint32_t)plat::AtomicAdd(p, -(int32_t)v);
                case 2: return (uint32_t)plat::AtomicAnd(p, (int32_t)v); case 3: return (uint32_t)plat::AtomicOr(p, (int32_t)v); default: return (uint32_t)plat::AtomicXor(p, (int32_t)v); }
}

// Kernel imports are host functions with stdcall/fastcall conventions: pull the arguments off the guest stack and call them.
// Host functions take real pointers for pointer arguments; guest code passes 32-bit guest addresses. The bridge converts the arguments named by KernelImport::ptrMask,
// and passes everything as uintptr_t (on x86 that is the same width as before; on x64 the callee reads the low 32 bits of integer arguments).
typedef uintptr_t UA;
typedef uint32_t(PLAT_STDCALL* S0)(); typedef uint32_t(PLAT_STDCALL* S1)(UA); typedef uint32_t(PLAT_STDCALL* S2)(UA, UA); typedef uint32_t(PLAT_STDCALL* S3)(UA, UA, UA);
typedef uint32_t(PLAT_STDCALL* S4)(UA, UA, UA, UA); typedef uint32_t(PLAT_STDCALL* S5)(UA, UA, UA, UA, UA);
typedef uint32_t(PLAT_STDCALL* S6)(UA, UA, UA, UA, UA, UA); typedef uint32_t(PLAT_STDCALL* S7)(UA, UA, UA, UA, UA, UA, UA);
typedef uint32_t(PLAT_STDCALL* S8)(UA, UA, UA, UA, UA, UA, UA, UA);
typedef uint32_t(PLAT_STDCALL* S9)(UA, UA, UA, UA, UA, UA, UA, UA, UA);
typedef uint32_t(PLAT_STDCALL* S10)(UA, UA, UA, UA, UA, UA, UA, UA, UA, UA);
typedef uint32_t(PLAT_FASTCALL* F1)(UA); typedef uint32_t(PLAT_FASTCALL* F2)(UA, UA); typedef uint32_t(PLAT_FASTCALL* F3)(UA, UA, UA);
typedef uint64_t(PLAT_STDCALL* Q0)();
// printf-style formatting of a guest call: `ga` points at the guest stack arguments (format string address first, then the variadic values; 32-bit slots, doubles take two).
static void GuestFormat(const uint32_t* ga, char* out, size_t cap) {
  const char* f = ga[0] ? (const char*)G(ga[0]) : ""; unsigned ai = 1; size_t o = 0; out[0] = 0;
  auto put = [&](const char* s) { while (*s && o + 1 < cap) out[o++] = *s++; out[o] = 0; };
  while (*f && o + 1 < cap) {
    if (*f != '%') { out[o++] = *f++; out[o] = 0; continue; }
    char spec[24]; unsigned sl = 0; spec[sl++] = *f++;
    if (*f == '%') { ++f; put("%"); continue; }
    while (*f && strchr("-+ #0123456789.lhzjt", *f) && sl < sizeof spec - 4) { if (*f != 'l' && *f != 'h' && *f != 'z' && *f != 'j' && *f != 't') spec[sl++] = *f; ++f; }
    char conv = *f ? *f++ : 0; char buf[400];
    switch (conv) {
      case 'd': case 'i': spec[sl++] = 'd'; spec[sl] = 0; snprintf(buf, sizeof buf, spec, (int)ga[ai++]); put(buf); break;
      case 'u': case 'x': case 'X': case 'o': spec[sl++] = conv; spec[sl] = 0; snprintf(buf, sizeof buf, spec, (unsigned)ga[ai++]); put(buf); break;
      case 'c': spec[sl++] = 'c'; spec[sl] = 0; snprintf(buf, sizeof buf, spec, (int)ga[ai++]); put(buf); break;
      case 'p': snprintf(buf, sizeof buf, "0x%08x", ga[ai++]); put(buf); break;
      case 's': { uint32_t a = ga[ai++]; spec[sl++] = 's'; spec[sl] = 0; snprintf(buf, sizeof buf, spec, a ? (const char*)G(a) : "(null)"); put(buf); break; }
      case 'f': case 'e': case 'g': case 'E': case 'G': { uint64_t bits = (uint64_t)ga[ai] | ((uint64_t)ga[ai + 1] << 32); ai += 2; double dv; memcpy(&dv, &bits, 8); spec[sl++] = conv; spec[sl] = 0; snprintf(buf, sizeof buf, spec, dv); put(buf); break; }
      default: break;
    }
  }
}
static void KBridge(LiftCpu* c, const KernelImport* k) {
  if (!k->fn) { c->eax = 0; c->esp += 4 + 4 * k->argc; return; }
  const uint32_t* ga = (const uint32_t*)G(c->esp + 4); uint32_t r = 0; uint64_t r64 = 0; bool wide = false; int pops = 0;
  UA a[10]; for (int i = 0; i < 10; i++) a[i] = ((k->ptrMask >> i) & 1) ? (ga[i] ? (UA)G(ga[i]) : 0) : (UA)ga[i];   // pointer arguments become host pointers
  if (k->conv == 'c') {   // variadic cdecl (DbgPrint): the caller pops the arguments
    r = 0; pops = 0;
    if (!strcmp(k->name, "DbgPrint")) { char line[1024]; GuestFormat(ga, line, sizeof line); size_t n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0; Log("[DbgPrint] %s", line); }
  }
  else if (k->conv == 'f') {
    uint32_t ecx = c->ecx, edx = c->edx; int n = k->argc; pops = n > 2 ? n - 2 : 0; 
    switch (n) { case 1: r = ((F1)k->fn)((k->ptrMask & 1) && ecx ? (UA)G(ecx) : (UA)ecx); break; case 2: r = ((F2)k->fn)((k->ptrMask & 1) && ecx ? (UA)G(ecx) : (UA)ecx, edx); break; case 3: r = ((F3)k->fn)(ecx, edx, a[0]); break;
                 default: Log("[lift] fastcall import %s with %d args", k->name, n); }
  } else {
    int n = k->argc; pops = n;
    if (!strcmp(k->name, "KeQueryPerformanceCounter") || !strcmp(k->name, "KeQueryPerformanceFrequency")) { r64 = ((Q0)k->fn)(); wide = true; }
    else switch (n) {
      case 0: r = ((S0)k->fn)(); break; case 1: r = ((S1)k->fn)(a[0]); break; case 2: r = ((S2)k->fn)(a[0], a[1]); break; case 3: r = ((S3)k->fn)(a[0], a[1], a[2]); break;
      case 4: r = ((S4)k->fn)(a[0], a[1], a[2], a[3]); break; case 5: r = ((S5)k->fn)(a[0], a[1], a[2], a[3], a[4]); break;
      case 6: r = ((S6)k->fn)(a[0], a[1], a[2], a[3], a[4], a[5]); break; case 7: r = ((S7)k->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
      case 8: r = ((S8)k->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break; case 9: r = ((S9)k->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]); break;
      case 10: r = ((S10)k->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]); break;
      default: Log("[lift] import %s with %d args", k->name, n);
    }
  }
  if (wide) { c->eax = (uint32_t)r64; c->edx = (uint32_t)(r64 >> 32); } else c->eax = r;
  c->esp += 4 + 4 * pops;   // return address + callee-popped arguments
}

// per-thread trail of the last lifted functions entered (crash diagnostics) and the native call in progress
struct Ring { uint32_t r[64]; unsigned pos; uint32_t tid; };
static std::mutex g_ringMx; static std::vector<Ring*> g_rings;
static Ring* ThreadRing() { static thread_local Ring* t = nullptr; if (!t) { t = new Ring{}; t->tid = plat::ThreadId(); std::lock_guard<std::mutex> l(g_ringMx); g_rings.push_back(t); } return t; }
#define t_ring (ThreadRing()->r)
#define t_ringPos (ThreadRing()->pos)
static thread_local uint32_t t_inNative = 0;
#ifndef LIFT_NO_NATIVE
// Run unlifted original code: switch to the guest stack, load the registers, call, store them back. The target travels in fs:[0x1C] (TEB.EnvironmentPointer, unused)
// because every general register carries guest state at the call. pcr+0x800/0x804 hold the host esp and LiftCpu pointer (old values saved on the host stack for nesting).
__declspec(naked) static void PLAT_CDECL NativeCallAsm(uint32_t* regs, uint32_t target) {
  __asm {
    push ebx
    push esi
    push edi
    push ebp
    mov ebx, [esp + 20]
    mov eax, [esp + 24]
    mov fs:[0x1C], eax
    mov eax, fs:[0x14]
    push dword ptr [eax + 0x800]
    push dword ptr [eax + 0x804]
    push dword ptr fs:[4]
    push dword ptr fs:[8]
    mov ecx, [eax + 0x808]
    mov fs:[4], ecx
    mov ecx, [eax + 0x80C]
    mov fs:[8], ecx
    mov [eax + 0x800], esp
    mov [eax + 0x804], ebx
    mov ecx, [ebx + 4]
    mov edx, [ebx + 8]
    mov esp, [ebx + 16]
    add esp, 4                 // drop the return-address slot the lifted caller pushed: `call` below pushes the real one
    mov ebp, [ebx + 20]
    mov esi, [ebx + 24]
    mov edi, [ebx + 28]
    mov eax, [ebx + 0]
    mov ebx, [ebx + 12]
    call dword ptr fs:[0x1C]
    push ecx
    mov ecx, fs:[0x14]
    mov ecx, [ecx + 0x804]
    mov [ecx + 0], eax
    pop dword ptr [ecx + 4]
    mov [ecx + 8], edx
    mov [ecx + 12], ebx
    mov [ecx + 16], esp
    mov [ecx + 20], ebp
    mov [ecx + 24], esi
    mov [ecx + 28], edi
    mov eax, fs:[0x14]
    mov esp, [eax + 0x800]
    pop dword ptr fs:[8]
    pop dword ptr fs:[4]
    pop dword ptr [eax + 0x804]
    pop dword ptr [eax + 0x800]
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret
  }
}
static void NativeCall(LiftCpu* c, uint32_t target) {
  uint32_t prevNative = t_inNative; t_inNative = target;
  // Native code expects a sane x87: leave MMX state (lifted MMX code may not have run its emms yet) and use the control word the lifted code last set.
  _mm_empty(); unsigned short cw = c->fcw; __asm { fldcw cw }
  if (g_trace) Log("[lift] -> native 0x%x (esp %x fcw %04x)", target, c->esp, cw);
  NativeCallAsm(&c->eax, target);
  __asm { fnstcw cw }
  if (cw != c->fcw) { static int n = 0; if (n++ < 20) Log("[lift] native 0x%x changed the x87 control word %04x -> %04x", target, c->fcw, cw); }
  c->fcw = cw; t_inNative = prevNative;
}

#else
// No native fallback (portable builds): every guest function must be lifted or overridden.
static void NativeCall(LiftCpu* c, uint32_t target) {
  { FILE* mf = fopen("work/lift/native_targets.txt", "a"); if (mf) { fprintf(mf, "%08x\n", target); fclose(mf); } }
  Log("[lift] FATAL: guest function 0x%x is not lifted and there is no native fallback", target); fflush(nullptr); abort();
}
#endif
static void DumpAllRings() {
  std::lock_guard<std::mutex> l(g_ringMx);
  for (Ring* r : g_rings) {   // oldest first, runs of the same function collapsed as "addr*count"
    char b[900] = ""; uint32_t prev = 0xFFFFFFFFu; int run = 0; auto flush = [&]() { if (run) { char t[32]; snprintf(t, sizeof t, run > 1 ? " %x*%d" : " %x", prev, run); strcat(b, t); } };
    for (int i = 0; i < 64; i++) { uint32_t v = r->r[(r->pos + i) & 63]; if (!v) continue; if (v == prev) run++; else { flush(); prev = v; run = 1; } }
    flush(); Log("[watch] tid %lu last:%s", r->tid, b);
  }
}
extern "C" void LiftDumpRing() { if (!g_on) return; char b[900] = ""; for (int i = 0; i < 24; i++) { char t[16]; snprintf(t, sizeof t, " %x", t_ring[(t_ringPos - 24 + i) & 63]); strcat(b, t); } Log("[lift] last functions entered (oldest first):%s  (inside native call to 0x%x)", b, t_inNative); }

extern "C" void LiftDispatch(LiftCpu* c, uint32_t target) {
  t_ring[t_ringPos++ & 63] = target;
  { static const uint32_t logFn = getenv("XBOX_LIFT_LOGFN") ? (uint32_t)strtoul(getenv("XBOX_LIFT_LOGFN"), nullptr, 16) : 0; static std::atomic<int> n{0}; if (logFn && target == logFn && n++ < (getenv("XBOX_LIFT_LOGFN_N") ? atoi(getenv("XBOX_LIFT_LOGFN_N")) : 40)) { const uint32_t* a = (const uint32_t*)G(c->esp); Log("[logfn] 0x%x called from 0x%x args: %x %x %x %x %x %x  [edi=%x ebp=%x ebp+10=%x]", target, a[0], a[1], a[2], a[3], a[4], a[5], a[6], c->edi, c->ebp, *(const uint32_t*)G(c->ebp + 0x10)); } }
  auto it = g_lifted.find(target);
  if (it != g_lifted.end()) { if (g_trace) LiftHere(target); if (g_stats) plat::AtomicAdd(&g_nLifted, 1); it->second(c); return; }
  if (!g_over.empty()) { auto o = g_over.find(target); if (o != g_over.end()) { KBridge(c, o->second); return; } }
  auto k = g_kfn.find((uintptr_t)target);
  if (k != g_kfn.end()) { if (g_stats) plat::AtomicAdd(&g_nKernel, 1); KBridge(c, k->second); return; }
  if (g_stats) plat::AtomicAdd(&g_nNative, 1);
  if (target >= 0x10000 && target < 0x600000) { std::lock_guard<std::mutex> lk(g_nativeMx); static int n = 0; if (++g_nativeCount[target] == 1) { if (n++ < 400) Log("[lift] native fallback for 0x%x", target); if (g_stats) { if (FILE* nf = fopen("work/lift/native_targets.txt", "a")) { fprintf(nf, "%08x\n", target); fclose(nf); } } } }
  NativeCall(c, target);
}

// Host -> guest: call a guest function with stdcall arguments on the current thread (thread routines, DPCs, ISRs, callbacks).
uint32_t GuestCall(uint32_t fn, int n, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
#ifdef LIFT_NO_NATIVE
  if (false) {
#else
  if (!g_on || !g_lifted.count(fn)) {
#endif   // not lifted: the original code runs natively, exactly as without XBOX_LIFT
    if (g_on) { static std::mutex mx; std::lock_guard<std::mutex> lk(mx); static std::unordered_map<uint32_t, int> seen; if (!seen[fn]++) Log("[lift] GuestCall runs 0x%x natively (not lifted)", fn); }
    typedef uint32_t(PLAT_STDCALL * F)(uint32_t, uint32_t, uint32_t, uint32_t);
    switch (n) { case 0: return ((S0)(uintptr_t)fn)(); case 1: return ((S1)(uintptr_t)fn)(a0); case 2: return ((S2)(uintptr_t)fn)(a0, a1); case 3: return ((S3)(uintptr_t)fn)(a0, a1, a2); default: return ((S4)(uintptr_t)fn)(a0, a1, a2, a3); }
  }
  // Lifted C frames live on the host stack, so the guest stack must be separate memory (1 MB per thread, allocated on first use). Nested calls (a kernel callback
  // made from inside lifted code) continue below the outer call's esp.
  static thread_local uint32_t t_gtop = 0; static thread_local LiftCpu* t_cur = nullptr;
  if (!t_gtop) {
    const uint32_t size = 1 << 20; uint8_t* m = (uint8_t*)GuestVirtualAlloc(size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    t_gtop = GA(m) + size; uint8_t* pcr = G(XPcrGet());
    *(uint32_t*)(pcr + 0x808) = t_gtop; *(uint32_t*)(pcr + 0x80C) = GA(m);
  }
  LiftCpu c; memset(&c, 0, sizeof c);
  c.fsbase = XPcrGet(); c.fcw = 0x37F;
  uint32_t esp = (t_cur ? t_cur->esp - 0x100 : t_gtop - 0x100) & ~0xFu;
  uint32_t args[4] = {a0, a1, a2, a3};
  for (int i = n - 1; i >= 0; i--) { esp -= 4; *(uint32_t*)G(esp) = args[i]; }
  esp -= 4; *(uint32_t*)G(esp) = 0;   // return address (never used)
  c.esp = esp; c.ebp = 0;
  LiftCpu* outer = t_cur; t_cur = &c;
  LiftDispatch(&c, fn);
  t_cur = outer;
  return c.eax;
}

#ifndef LIFT_NO_NATIVE
// ---- differential self-test: lifted vs original code on random inputs (XBOX_LIFT_SELFTEST=1) ----
static uint32_t g_rng = 12345;
static uint32_t Rnd() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8 | g_rng << 24; }
static std::vector<uint8_t> g_golden, g_postN;
static void DataSave(std::vector<uint8_t>& dst) { size_t o = 0; for (auto& r : g_dataRegions) { memcpy(&dst[o], (const void*)(uintptr_t)r.first, r.second); o += r.second; } }
static void DataLoad(const std::vector<uint8_t>& src) { size_t o = 0; for (auto& r : g_dataRegions) { memcpy((void*)(uintptr_t)r.first, &src[o], r.second); o += r.second; } }
struct Snap { uint32_t r[8]; uint8_t mem[4096]; uint8_t stk[2048]; bool faulted; uint32_t code; uint32_t base; uint32_t at; uint32_t addr; };
static int StFilt(EXCEPTION_POINTERS* ep, Snap* o) { o->code = ep->ExceptionRecord->ExceptionCode; o->at = (uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress; o->addr = ep->ExceptionRecord->NumberParameters > 1 ? (uint32_t)ep->ExceptionRecord->ExceptionInformation[1] : 0; return EXCEPTION_EXECUTE_HANDLER; }
static uint8_t* g_scratch = nullptr;   // one committed page between no-access guard regions: runaway stores fault instead of trashing the process 
static volatile LONG g_stRun = 0; static volatile DWORD g_stTid = 0;
static void PLAT_STDCALL StBail() { RaiseException(0xE0DEAD01, 0, 0, nullptr); }
static DWORD WINAPI StWatchdog(void*) {   // a random input can send a function into a very long loop: after ~300 ms redirect the thread into an exception that the trial's __try catches
  LONG last = -1; int stuck = 0;
  for (;;) {
    Sleep(100); LONG r = g_stRun; DWORD tid = g_stTid;
    if (tid && r == last) { if (++stuck >= 3) { HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, tid); if (h) { SuspendThread(h); CONTEXT cx; cx.ContextFlags = CONTEXT_CONTROL; GetThreadContext(h, &cx); cx.Eip = (DWORD)(uintptr_t)&StBail; SetThreadContext(h, &cx); ResumeThread(h); CloseHandle(h); } stuck = 0; } }
    else { stuck = 0; last = r; }
  }
}
// The trial's guest stack is a local array in RunInner, which is called from the __try in RunOne: SEH unwinding needs esp to be below the frame that holds the handler.
static __declspec(noinline) void RunInner(const LiftEntry& e, bool native, const uint32_t* init, const uint8_t* stk0, LiftCpu* c, uint8_t* stkOut, uint32_t* baseOut) {
  uint8_t sb[4096]; memset(sb, 0, 2048); memcpy(sb + 2048, stk0, 2048);
  c->esp = (uint32_t)(uintptr_t)(sb + 2048); *baseOut = c->esp; c->fsbase = XPcrGet(); c->fcw = 0x37F;
  if (getenv("XBOX_LIFT_SELFTEST_V")) { Log("[selftest]   run %s", native ? "native" : "lifted"); fflush(nullptr); }
  if (native) { _asm { fninit } NativeCall(c, e.addr); } else { _asm { fninit } e.fn(c); }
  memcpy(stkOut, sb + 2048, 2048);
}
static void RunOne(const LiftEntry& e, bool native, const uint32_t* init, const uint8_t* mem0, const uint8_t* stk0, Snap* out) {
  memcpy(g_scratch, mem0, 4096);
  LiftCpu c; memset(&c, 0, sizeof c); memcpy(&c.eax, init, 32);
  out->faulted = false; out->code = 0; memset(out->stk, 0, sizeof out->stk);
  t_quiet = true; InterlockedIncrement(&g_stRun); g_stTid = plat::ThreadId();
  out->base = 0; __try { RunInner(e, native, init, stk0, &c, out->stk, &out->base); } __except (StFilt(GetExceptionInformation(), out)) { out->faulted = true; }
  g_stTid = 0; t_quiet = false;
  if (getenv("XBOX_LIFT_SELFTEST_V")) { Log("[selftest]   after %s: fault=%d code=%08x TEB base=%08x limit=%08x", native ? "native" : "lifted", (int)out->faulted, out->code, __readfsdword(4), __readfsdword(8)); fflush(nullptr); }
  memcpy(out->r, &c.eax, 32); memcpy(out->mem, g_scratch, 4096);
  for (int k = 0; k < 8; k++) if (out->base && out->r[k] + 0x1000 >= out->base && out->r[k] < out->base + 0x1000) out->r[k] = 0x7F000000u + (out->r[k] - out->base);   // stack addresses compared relative to the initial esp
}
void XThreadInit(uint32_t);
static void LiftSelfTest() {
  { size_t n = 0; for (auto& r : g_dataRegions) n += r.second; g_golden.resize(n); g_postN.resize(n); DataSave(g_golden); }
  for (auto& r : g_codeRegions) { DWORD old; VirtualProtect((void*)(uintptr_t)r.first, r.second, PAGE_EXECUTE_READ, &old); }   // stray stores must fault, not rewrite code
  { uint8_t* r = (uint8_t*)VirtualAlloc(nullptr, 0x30000, MEM_RESERVE, PAGE_NOACCESS); g_scratch = (uint8_t*)VirtualAlloc(r + 0x10000, 4096, MEM_COMMIT, PAGE_READWRITE); }
  CreateThread(nullptr, 0, StWatchdog, nullptr, 0, nullptr);
  XThreadInit(0); { uint8_t* pcr = G(XPcrGet()); *(uint32_t*)(pcr + 0x808) = __readfsdword(4); *(uint32_t*)(pcr + 0x80C) = __readfsdword(8); }   // bounds unchanged: the guest stack is inside them
  int tested = 0, bad = 0, skipped = 0; int trials = getenv("XBOX_LIFT_SELFTEST") && atoi(getenv("XBOX_LIFT_SELFTEST")) > 1 ? atoi(getenv("XBOX_LIFT_SELFTEST")) : 24;
  unsigned from = getenv("XBOX_LIFT_SELFTEST_FROM") ? (unsigned)atoi(getenv("XBOX_LIFT_SELFTEST_FROM")) : 0;   // restartable: the driver script resumes after a function that killed the process
  for (unsigned i = from; i < g_tabCount; i++) {
    const LiftEntry& e = g_tab[i]; if (!(e.flags & 1) || !g_lifted.count(e.addr)) continue;
    if (const char* only = getenv("XBOX_LIFT_SELFTEST_ONLY")) { unsigned o = 0; sscanf(only, "%x", &o); if (o != e.addr) continue; }
    if (FILE* pf = fopen("work/lift/st_pos.txt", "w")) { fprintf(pf, "%u %x\n", i, e.addr); fclose(pf); }
    if (getenv("XBOX_LIFT_SELFTEST_V")) { Log("[selftest] f_%08x", e.addr); fflush(nullptr); }
    bool fail = false; int ok = 0; char why[200] = "";
    for (int t = 0; t < trials && !fail; t++) {
      uint8_t mem0[4096], stk0[2048]; for (auto& b : mem0) b = (uint8_t)Rnd(); 
      if (t % 3 == 0) for (int k = 0; k < 4096; k += 4) if (Rnd() % 3) *(uint32_t*)(mem0 + k) = Rnd() % 64;   // small numbers: loop counts, indexes
      uint32_t init[8]; const uint32_t base = (uint32_t)(uintptr_t)g_scratch;
      static uint32_t wild = 0; if (!wild) wild = (uint32_t)(uintptr_t)VirtualAlloc(nullptr, 1 << 20, MEM_RESERVE, PAGE_NOACCESS);   // reserved, never committed: any access faults
      auto pick = [&]() -> uint32_t { switch (Rnd() % 8) { case 0: return Rnd() % 16; case 1: return base + (Rnd() % 1024) * 2; case 2: return base + (Rnd() % 900); case 3: return wild + (Rnd() % 0x40000) * 4; case 4: { static const uint32_t sp[] = {0, 1, 0xFFFFFFFFu, 0xFFu, 0x100u, 0x7FFFu, 2, 3}; return sp[Rnd() % 8]; } default: return Rnd() % 4096; } };
      for (int k = 0; k < 8; k++) init[k] = pick();
      for (int k = 0; k < 512; k++) *(uint32_t*)(stk0 + k * 4) = k == 0 ? 0 : (k < 64 ? pick() : Rnd() % 4096);   // [esp]=return address, then arguments, then stack contents above
      init[4] = 0;   // esp is set by RunInner
      if (getenv("XBOX_LIFT_SELFTEST_V")) { Log("[selftest]  trial %d eax=%x ecx=%x edx=%x ebx=%x esi=%x edi=%x ebp=%x", t, init[0], init[1], init[2], init[3], init[6], init[7], init[5]); fflush(nullptr); }
      Snap a, b; RunOne(e, true, init, mem0, stk0, &a); DataSave(g_postN); DataLoad(g_golden); RunOne(e, false, init, mem0, stk0, &b);
      char dwhy[160] = ""; bool dataDiff = false; { static std::vector<uint8_t> post; post.resize(g_golden.size()); DataSave(post); dataDiff = memcmp(post.data(), g_postN.data(), post.size()) != 0; if (dataDiff) { size_t k = 0; while (post[k] == g_postN[k]) k++; snprintf(dwhy, sizeof dwhy, "global data differs at +0x%x native=%02x lifted=%02x", (unsigned)k, g_postN[k], post[k]); } DataLoad(g_golden); }

      if (a.faulted && b.faulted) continue;
      if (a.code == 0xE0DEAD01 || b.code == 0xE0DEAD01) continue;   // ran away: not comparable
      if (a.faulted && !b.faulted && (a.at < e.addr || a.at >= e.hi)) continue;   // native `ret`/jump left the function (a stray seed, not a real function): not comparable
      if (a.faulted != b.faulted) { if (a.code == EXCEPTION_INT_DIVIDE_BY_ZERO || a.code == EXCEPTION_INT_OVERFLOW || b.code == EXCEPTION_INT_DIVIDE_BY_ZERO) continue; snprintf(why, sizeof why, "fault mismatch native=%d(%08x at %x addr %x) lifted=%d(%08x at %x addr %x) inputs eax=%x ecx=%x edx=%x ebx=%x esi=%x edi=%x ebp=%x", a.faulted, a.code, a.at, a.addr, b.faulted, b.code, b.at, b.addr, init[0], init[1], init[2], init[3], init[6], init[7], init[5]); fail = true; break; }
      static const char* rn[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
      for (int k = 0; k < 8 && !fail; k++) if (a.r[k] != b.r[k]) { snprintf(why, sizeof why, "%s native=%08x lifted=%08x (inputs eax=%x ecx=%x edx=%x ebx=%x esi=%x edi=%x ebp=%x)", rn[k], a.r[k], b.r[k], init[0], init[1], init[2], init[3], init[6], init[7], init[5]); fail = true; }
      if (!fail && memcmp(a.mem, b.mem, 4096)) { int d = 0; while (a.mem[d] == b.mem[d]) d++; snprintf(why, sizeof why, "memory differs at scratch+0x%x native=%02x lifted=%02x", d, a.mem[d], b.mem[d]); fail = true; }
      if (dataDiff && !a.faulted && !b.faulted) { snprintf(why, sizeof why, "%s", dwhy); fail = true; break; }
      int so = (int)(a.r[4] - 0x7F000000u); if (so < 4 || so > 2044) so = 4;   // the native epilogue scribbles on popped argument slots below the final esp
      if (!fail && memcmp(a.stk + so, b.stk + so, 2048 - so))   /* slot 0: return address (a host address natively) */ { { int d = so; while (a.stk[d] == b.stk[d]) d++; snprintf(why, sizeof why, "stack differs at esp+0x%x native=%02x lifted=%02x (final esp rel: %x %x)", d, a.stk[d], b.stk[d], a.r[4], b.r[4]); fail = true; } }
      ok++;
    }
    if (ok == 0 && !fail) { skipped++; continue; }
    tested++; if (fail) { bad++; Log("[selftest] MISMATCH f_%08x: %s", e.addr, why); if (FILE* rf = fopen("work/lift/st_report.txt", "a")) { fprintf(rf, "MISMATCH f_%08x: %s\n", e.addr, why); fclose(rf); } }
  }
  for (auto& r : g_codeRegions) { DWORD old; VirtualProtect((void*)(uintptr_t)r.first, r.second, PAGE_EXECUTE_READWRITE, &old); }
  Log("[selftest] %d functions compared, %d mismatches, %d skipped (always faulted)", tested, bad, skipped);
}

#else
static void LiftSelfTest() {}
#endif
