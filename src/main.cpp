// Native 32-bit host for original Xbox titles: maps the XBE at its own addresses, patches the kernel thunk table, runs the entry point.
#include "kernel.h"
#include "guestmem.h"
#include "xbe.h"
#include "plat.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void LiftDumpRing(); extern "C" bool LiftQuiet();
#ifdef _WIN32
#ifndef LIFT_NO_NATIVE
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
  if (LiftQuiet()) return EXCEPTION_CONTINUE_SEARCH;
  auto* r = ep->ExceptionRecord; auto* c = ep->ContextRecord;
  static volatile LONG once = 0;
  if (r->ExceptionCode == EXCEPTION_SINGLE_STEP || r->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION || r->ExceptionCode == EXCEPTION_BREAKPOINT || (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && ((r->ExceptionInformation[1] >= 0xFD000000u && r->ExceptionInformation[1] < 0xFE000000u) || (r->ExceptionInformation[1] >= 0xFED00000u && r->ExceptionInformation[1] < 0xFED10000u) || (r->ExceptionInformation[1] >= 0xFE800000u && r->ExceptionInformation[1] < 0xFE880000u) || (r->ExceptionInformation[1] >= 0xFEC00000u && r->ExceptionInformation[1] < 0xFEC01000u) || (r->ExceptionInformation[1] >= 0xFEF00000u && r->ExceptionInformation[1] < 0xFEF01000u)))) return EXCEPTION_CONTINUE_SEARCH;  // NV2A register window (mmio.cpp)
  if (r->ExceptionCode == EXCEPTION_BREAKPOINT || r->ExceptionCode == 0x406D1388 || r->ExceptionCode == 0x40010006 || r->ExceptionCode == 0x4001000A || r->ExceptionCode == 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;
  static thread_local bool inCrash = false;   // IsBadReadPtr below faults internally: don't log those probes recursively
  if (inCrash) return EXCEPTION_CONTINUE_SEARCH;
  struct Guard { Guard() { inCrash = true; } ~Guard() { inCrash = false; } } guard;
  LiftDumpRing();
  Log("[crash] x87: cw=%04x sw=%04x tw=%04x", (unsigned)(c->FloatSave.ControlWord & 0xFFFF), (unsigned)(c->FloatSave.StatusWord & 0xFFFF), (unsigned)(c->FloatSave.TagWord & 0xFFFF));
  Log("[crash] exception 0x%08lx at eip=0x%08lx (access %s 0x%08lx) tid=%lu", r->ExceptionCode, c->Eip,
      r->NumberParameters >= 2 ? (r->ExceptionInformation[0] == 0 ? "read" : r->ExceptionInformation[0] == 1 ? "write" : "exec") : "-",
      r->NumberParameters >= 2 ? (unsigned long)r->ExceptionInformation[1] : 0ul, GetCurrentThreadId());
  Log("[crash] stack limit=%08lx base=%08lx", (unsigned long)__readfsdword(8), (unsigned long)__readfsdword(4));
  Log("[crash] eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx", c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
  uint32_t* sp = (uint32_t*)c->Esp;
  if (!IsBadReadPtr(sp, 64)) { for (int i = 0; i < 12; i++) Log("[crash]   [esp+%02x] = %08x", i * 4, sp[i]); }
  if (!IsBadReadPtr(sp, 4) && sp[0] >= 0x12000 && sp[0] < 0x379c4c && !IsBadReadPtr((void*)(uintptr_t)(sp[0] - 16), 16)) { char cb[80] = ""; const uint8_t* q = (const uint8_t*)(uintptr_t)(sp[0] - 16); for (int i = 0; i < 16; i++) { char t[8]; sprintf(t, "%02x ", q[i]); strcat(cb, t); } Log("[crash] code bytes before return address 0x%x: %s", sp[0], cb); }
  if (c->Eip == 0 && !IsBadReadPtr(sp, 4) && sp[0] >= 0x12005 && !IsBadReadPtr((void*)(uintptr_t)(sp[0] - 5), 5) && *(uint8_t*)(uintptr_t)(sp[0] - 5) == 0xE8) {   // jumped to 0 right after a direct call: inspect the call target
    uint32_t tgt = sp[0] + *(int32_t*)(uintptr_t)(sp[0] - 4); MEMORY_BASIC_INFORMATION mbi{}; VirtualQuery((void*)(uintptr_t)tgt, &mbi, sizeof mbi);
    Log("[crash] direct call target 0x%x: state=0x%lx protect=0x%lx base=%p size=0x%zx first bytes: %s", tgt, mbi.State, mbi.Protect, mbi.BaseAddress, mbi.RegionSize, IsBadReadPtr((void*)(uintptr_t)tgt, 16) ? "unreadable" : [&] { static char hb[64]; const uint8_t* q = (const uint8_t*)(uintptr_t)tgt; for (int i = 0; i < 16; i++) sprintf(hb + i * 3, "%02x ", q[i]); return hb; }());
  }
  { const uint32_t* bp = (const uint32_t*)c->Ebp; if (c->Ebp && !IsBadReadPtr(bp, 40)) Log("[crash] [ebp+0..]: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", bp[0], bp[1], bp[2], bp[3], bp[4], bp[5], bp[6], bp[7], bp[8], bp[9]); }
  if (const char* peek = getenv("XBOX_PEEK")) {  // "addr:count,addr:count" (hex): dump guest memory words at a crash
    char buf[256]; strncpy(buf, peek, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char* tok = strtok(buf, ","); tok; tok = strtok(nullptr, ",")) {
      unsigned a = 0, n = 4; sscanf(tok, "%x:%u", &a, &n); char line[300] = ""; uint32_t* w = (uint32_t*)(uintptr_t)a;
      for (unsigned i = 0; i < n && i < 16 && !IsBadReadPtr(w + i, 4); i++) { char t[16]; sprintf(t, " %08x", w[i]); strcat(line, t); }
      Log("[peek] 0x%x:%s", a, line);
    }
  }
  { // frame-pointer chain (return addresses)
    uint32_t* fp = (uint32_t*)c->Ebp; char b[200] = "";
    for (int i = 0; i < 8 && fp && !IsBadReadPtr(fp, 8); i++) { char t[16]; sprintf(t, " %x", fp[1]); strcat(b, t); fp = (uint32_t*)(uintptr_t)fp[0]; }
    Log("[crash]   ebp chain returns:%s", b);
    char sb[300] = ""; int n = 0; uint32_t* sp2 = (uint32_t*)c->Esp;
    for (int i = 0; i < 400 && n < 12 && !IsBadReadPtr(sp2 + i, 4); i++) if (sp2[i] >= 0x12000 && sp2[i] < 0x379c4c) { char t[16]; sprintf(t, " %x", sp2[i]); strcat(sb, t); ++n; }
    Log("[crash]   stack code addresses:%s", sb);
  }
  { // SEH chain of the faulting thread (the Xbox CRT builds Windows-compatible frames)
    uint32_t* f = (uint32_t*)__readfsdword(0);
    for (int i = 0; i < 8 && (uintptr_t)f != 0xFFFFFFFFu && !IsBadReadPtr(f, 8); i++) { Log("[crash]   seh frame %p next=%08x handler=%08x", f, f[0], f[1]); f = (uint32_t*)(uintptr_t)f[0]; }
  }
  if (InterlockedExchange(&once, 1) == 0) { fflush(stdout); }
  return EXCEPTION_CONTINUE_SEARCH;
}
#else
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {   // portable: just report the exception; there is no x86 register context
  auto* r = ep->ExceptionRecord; if (r->ExceptionCode == EXCEPTION_BREAKPOINT || r->ExceptionCode == EXCEPTION_SINGLE_STEP || (r->ExceptionCode & 0xF0000000u) == 0x40000000u) return EXCEPTION_CONTINUE_SEARCH;
  LiftDumpRing(); Log("[crash] exception 0x%08lx at %p", r->ExceptionCode, r->ExceptionAddress); return EXCEPTION_CONTINUE_SEARCH;
}
#endif
#else
static void PortableCrash(const char* what, uintptr_t addr, uintptr_t ip) { LiftDumpRing(); Log("[crash] %s at address %p (ip %p)", what, (void*)addr, (void*)ip); fflush(stdout); }
#endif


// Debug aid: XBOX_PROF=1 samples every thread's EIP ~1000x/s and writes work/prof.txt ("tid-class rva count") at exit; resolve RVAs with tools/prof_resolve.py + work/build/host.map.
#include <map>
#include <vector>
static std::map<std::pair<uint32_t, uint32_t>, unsigned> g_prof;   // (tid, eip>>4) -> samples
static volatile bool g_profStop = false;

// Debug aid: with XBOX_DUMPTHREADS=1, creating work/dump_threads.flag makes every thread log its EIP and the code-looking words on its stack (who is waiting on what).
void KernelSetTrace(bool on);
static uint32_t DumpThread(void*) {
#ifndef LIFT_NO_NATIVE
  DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
#endif
  for (;;) {
    plat::SleepMs(500);
    if (FILE* t = fopen("work/trace_on.flag", "rb")) { fclose(t); remove("work/trace_on.flag"); KernelSetTrace(true); Log("[threads] kernel trace ON"); }
    if (FILE* t = fopen("work/trace_off.flag", "rb")) { fclose(t); remove("work/trace_off.flag"); KernelSetTrace(false); Log("[threads] kernel trace OFF"); }
    FILE* f = fopen("work/dump_threads.flag", "rb"); if (!f) continue; fclose(f); remove("work/dump_threads.flag");
#ifndef LIFT_NO_NATIVE
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te{sizeof te}; Log("[threads] ---- dump ----");
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID); if (!h) continue;
      if (SuspendThread(h) != (DWORD)-1) {
        CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(h, &c)) { char sb[900] = ""; const uint32_t* sp = (const uint32_t*)c.Esp; for (int i = 0; i < 600 && strlen(sb) < 800; i++) { if (IsBadReadPtr(sp + i, 4)) break; uint32_t w = sp[i]; if (w >= 0x10000 && w < 0x1A00000) { char t[16]; sprintf(t, " %x", w); strcat(sb, t); } }
          Log("[threads] tid %lu eip=%08lx esp=%08lx stack:%s", te.th32ThreadID, c.Eip, c.Esp, sb); }
        ResumeThread(h);
      }
      CloseHandle(h);
    }
    CloseHandle(snap);
#endif
  }
  return 0;
}

#ifndef LIFT_NO_NATIVE
static uint32_t ProfThread(void*) {
  DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId(); std::vector<DWORD> tids; DWORD lastScan = 0;
  while (!g_profStop) {
    if (GetTickCount() - lastScan > 250) { lastScan = GetTickCount(); tids.clear(); HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te{sizeof te}; for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) if (te.th32OwnerProcessID == pid && te.th32ThreadID != me) tids.push_back(te.th32ThreadID); CloseHandle(snap); }
    for (DWORD tid : tids) { HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid); if (!h) continue; if (SuspendThread(h) != (DWORD)-1) { CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL; if (GetThreadContext(h, &c)) g_prof[{tid, c.Eip >> 4}]++; ResumeThread(h); } CloseHandle(h); }
    Sleep(1);
  }
  return 0;
}
static void ProfDump() {
  g_profStop = true; Sleep(20); FILE* f = fopen("work/prof.txt", "w"); if (!f) return; DWORD base = (DWORD)(uintptr_t)GetModuleHandleA(nullptr);
  for (auto& kv : g_prof) fprintf(f, "%lu %08lx %lu\n", kv.first.first, (unsigned long)((kv.first.second << 4) - base), (unsigned long)kv.second);
  fclose(f);
}

// Debug aid: print where every thread of the process is executing (XBOX_SAMPLE=1 prints this once a second).
static void DumpThreads(const char* why) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te{sizeof te};
  if (snap == INVALID_HANDLE_VALUE) return;
  DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
    if (!h) continue;
    if (SuspendThread(h) != (DWORD)-1) {
      CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      if (GetThreadContext(h, &c)) {
        char chain[256] = ""; int n = 0; uint32_t* sp = (uint32_t*)c.Esp;
        for (int i = 0; i < 512 && n < 8 && !IsBadReadPtr(sp + i, 4); i++) if (sp[i] >= 0x12000 && sp[i] < 0x58000 * 0x10) { char t[16]; sprintf(t, " %x", sp[i]); strcat(chain, t); ++n; }
        Log("[threads] %s tid=%lu eip=%08lx esp=%08lx eax=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx xbe-stack:%s", why, te.th32ThreadID, c.Eip, c.Esp, c.Eax, c.Ecx, c.Edx, c.Esi, c.Edi, chain);
      }
      ResumeThread(h);
    }
    CloseHandle(h);
  }
  CloseHandle(snap);
}
#else
static uint32_t ProfThread(void*) { return 0; }
static void ProfDump() {}
static void DumpThreads(const char*) {}   // thread sampling reads x86 register contexts
#endif

void Nv2aDump();
void LaunchPageInit();
void KernelSetImageName(const char* hostXbe);
bool LiftEnabled(); void LiftInit(const char*); uint32_t KernelImportAddr(const KernelImport* k);
static uint32_t EntryThread(void* p) {
  typedef void(PLAT_STDCALL * Entry)();
  XThreadInit(0);
  Log("[boot] jumping to the XBE entry point 0x%x (tid %u)", (unsigned)(uintptr_t)p, plat::ThreadId());
  if (LiftEnabled()) GuestCall((uint32_t)(uintptr_t)p, 0); else ((Entry)p)();
  Log("[boot] entry point returned (tid %u)", plat::ThreadId());
  return 0;
}

// Last-chance handler: no Windows "application error" dialog; log the fault and exit so unattended runs never block on a message box.
#ifdef _WIN32
#ifndef LIFT_NO_NATIVE
static LONG WINAPI FatalFilter(EXCEPTION_POINTERS* ep) {
  Log("[fatal] module base 0x%08lx, eip rva 0x%lx", (unsigned long)(uintptr_t)GetModuleHandleA(nullptr), (unsigned long)(ep->ContextRecord->Eip - (uintptr_t)GetModuleHandleA(nullptr)));
  Log("[fatal] unhandled exception 0x%08lx at eip=0x%08lx (access address 0x%08lx)", ep->ExceptionRecord->ExceptionCode, ep->ContextRecord->Eip, ep->ExceptionRecord->NumberParameters >= 2 ? (unsigned long)ep->ExceptionRecord->ExceptionInformation[1] : 0ul);
  fflush(stdout); TerminateProcess(GetCurrentProcess(), 3); return EXCEPTION_EXECUTE_HANDLER;
}
#else
static LONG WINAPI FatalFilter(EXCEPTION_POINTERS* ep) {
  { auto* r = ep->ExceptionRecord; Log("[fatal] unhandled exception 0x%08lx at %p (module base %p, rva 0x%llx) access=%llu addr=%p", r->ExceptionCode, r->ExceptionAddress, (void*)GetModuleHandleA(nullptr), (unsigned long long)((uintptr_t)r->ExceptionAddress - (uintptr_t)GetModuleHandleA(nullptr)), r->NumberParameters > 0 ? (unsigned long long)r->ExceptionInformation[0] : 0ull, r->NumberParameters > 1 ? (void*)r->ExceptionInformation[1] : nullptr); }
  fflush(stdout); TerminateProcess(GetCurrentProcess(), 3); return EXCEPTION_EXECUTE_HANDLER;
}
#endif
#endif

// Exit without running static destructors: the device and audio threads started during init are still running and would race them.
[[noreturn]] static void Quit(int code) { fflush(stdout); fflush(stderr); _Exit(code); }

int main(int argc, char** argv) {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  SetUnhandledExceptionFilter(FatalFilter);
#endif
  const char* iso = argc > 1 ? argv[1] : "work/title/game.iso";
  const char* xbe = argc > 2 ? argv[2] : "work/title/default.xbe";
  GuestMemInit();
  KernelInit();
  MmioInit();
  DisplayInit();
  PrivInit();
#ifdef _WIN32
  AddVectoredExceptionHandler(1, CrashFilter);
#else
  plat::InstallCrashHandler(PortableCrash);
#endif
  if (strcmp(iso, "none") != 0 && !DiscOpen(iso)) { Log("cannot open disc image %s", iso); Quit(1); }   // "none": no disc in the drive (e.g. running the dashboard)
  XbeInfo info{}; char err[256];
  extern bool KernelContigOk();
  bool loaded = KernelContigOk() ? XbeLoad(xbe, &info, err, sizeof err) : (snprintf(err, sizeof err, "cannot map physical RAM at 0x80000000"), false);
  if (!loaded) {
    // With ASLR the low 5.5 MB are usually free, but the OS sometimes drops a mapping there: start over (a fresh process re-rolls the layout).
    int attempt = getenv("XBOX_ATTEMPT") ? atoi(getenv("XBOX_ATTEMPT")) : 0;
    Log("XBE load failed (attempt %d): %s", attempt, err);
#if defined(_WIN32) && defined(GUEST_IDENTITY)
    if (attempt < 30) {   // identity builds need fixed low addresses: start over (a fresh process re-rolls the layout)
      char num[16]; sprintf(num, "%d", attempt + 1); SetEnvironmentVariableA("XBOX_ATTEMPT", num);
      STARTUPINFOA si{sizeof si}; PROCESS_INFORMATION pi{};
      if (CreateProcessA(nullptr, GetCommandLineA(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) { WaitForSingleObject(pi.hProcess, INFINITE); DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); return (int)code; }
    }
#endif
    Quit(1);
  }
  // Patch the kernel thunk table: each entry 0x80000000|ordinal -> address of our implementation / exported variable.
  uint32_t* thunk = (uint32_t*)G(info.kernelThunk); int n = 0, missing = 0;
  for (; *thunk; ++thunk, ++n) {
    int ord = *thunk & 0xFFFF; const KernelImport* ki = nullptr;
    for (int i = 0; i < g_kernelImportCount; i++) if (g_kernelImports[i].ordinal == ord) ki = &g_kernelImports[i];
    if (!ki) { Log("[boot] import ordinal %d has no entry in the kernel table", ord); ++missing; continue; }
    *thunk = ki->conv == 'v' ? GA(KernelVariable(ord)) : KernelImportAddr(ki);
  }
  Log("[boot] patched %d kernel imports (%d unknown)", n, missing);
  XThreadSetTls(info.tlsAddr);
  // Per-title config (fs: patch list, byte patches, DSOUND entry points) lives in XBOX_TITLE_DIR (default work/title; the dashboard uses work/dash)
  { const char* dir = getenv("XBOX_TITLE_DIR") ? getenv("XBOX_TITLE_DIR") : "work/title"; char f[260];
    sprintf(f, "%s/fs_patches.txt", dir); XThreadPatch(f); sprintf(f, "%s/patches.txt", dir); PatchesApply(f); sprintf(f, "%s/dsound_hle.txt", dir); DsoundHleInstall(f); }
  KernelSetImageName(xbe);
  LaunchPageInit();
  BpInit();
  LiftInit(xbe);
  plat::ThreadStart(EntryThread, (void*)(uintptr_t)info.entry, 4 << 20, false, nullptr);
  // The entry thread usually just starts the title thread and returns; run until the title exits (HalReturnToFirmware etc.) or the time limit.
  int seconds = argc > 3 ? atoi(argv[3]) : 30;
  if (getenv("XBOX_PROF")) plat::ThreadSpawn(ProfThread);
  if (getenv("XBOX_DUMPTHREADS")) plat::ThreadSpawn(DumpThread);
  for (int i = 0; i < seconds; i++) { plat::SleepMs(1000); if (getenv("XBOX_SAMPLE") && i % 3 == 2) DumpThreads("sample"); }
  if (getenv("XBOX_PROF")) ProfDump();
  DumpThreads("final");
  PrivDump();
  Nv2aDump();
  Log("[boot] time limit reached, exiting");
  fflush(stdout);
  _Exit(0);   // other threads are still running: do not run static destructors
}
