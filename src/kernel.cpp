// Core kernel services for the Xbox title: memory, objects, threads, time, files (disc + host-backed hard disk partitions).
#include "kernel.h"
#include "guestmem.h"
#include "plat.h"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#define KFN(name, ...) extern "C" uint32_t PLAT_STDCALL K_##name(__VA_ARGS__)
#define KFF(name, ...) extern "C" uint32_t PLAT_FASTCALL K_##name(__VA_ARGS__)

static FILE* g_log = nullptr;
static bool g_trace = getenv("XBOX_TRACE") != nullptr;
void KernelSetTrace(bool on) { g_trace = on; }   // main.cpp debug thread: work/trace_on.flag / trace_off.flag
static bool g_logFs = getenv("XBOX_LOG_FS") != nullptr;   // log reads/writes on host (HDD) files, not just opens
#define KT(name) do { if (g_trace) Log("[k] %s tid=%u", #name, plat::ThreadId()); } while (0)
static std::mutex g_logMutex;
void Log(const char* fmt, ...) {
  std::lock_guard<std::mutex> l(g_logMutex);
  va_list ap; va_start(ap, fmt);
  if (g_log) { vfprintf(g_log, fmt, ap); fputc('\n', g_log); fflush(g_log); }
  va_end(ap);
  va_start(ap, fmt); vprintf(fmt, ap); putchar('\n'); fflush(stdout); va_end(ap);
}
uint32_t KernelUnimplemented(int ordinal, const char* name) {
  static std::mutex m; static std::map<int, int> seen;
  std::lock_guard<std::mutex> l(m);
  if (seen[ordinal]++ < 3) Log("[kernel] UNIMPLEMENTED %s (ordinal %d) called from tid %u", name, ordinal, plat::ThreadId());
  return STATUS_NOT_IMPLEMENTED;
}

// ---- data exports ----
static std::map<int, void*> g_vars;
void* KernelVariable(int ordinal) {
  auto it = g_vars.find(ordinal);
  if (it != g_vars.end()) return it->second;
  void* p = G(GuestAlloc(0x1000));   // kernel data exports are read by the guest: they live in guest memory
  g_vars[ordinal] = p; return p;
}
static volatile uint32_t* g_tickCount;

// ---- memory ----
plat::Shm* g_ramMap; static uint8_t* g_contig; static uint32_t g_contigSize = 64u << 20, g_contigTop = 0x100000;  // first MB reserved
static std::map<uint32_t, uint32_t> g_contigAllocs;
static std::mutex g_memMutex;
static uint32_t g_contigHigh = 0x100000;   // highest offset ever handed out (above it the RAM is still pristine zero)
static std::map<uint32_t, uint32_t> g_contigFree;   // offset -> size of freed ranges below g_contigTop
static std::vector<std::pair<uint32_t, uint32_t>> g_contigQuarantine;   // freed (offset, size) not reusable until memory runs out
static void ContigRelease(uint32_t off, uint32_t size) {   // return a range to the free map, coalescing; g_memMutex held
  auto nx = g_contigFree.lower_bound(off);
  if (nx != g_contigFree.end() && off + size == nx->first) { size += nx->second; nx = g_contigFree.erase(nx); }
  if (nx != g_contigFree.begin()) { auto pv = std::prev(nx); if (pv->first + pv->second == off) { off = pv->first; size += pv->second; g_contigFree.erase(pv); } }
  if (off + size == g_contigTop) g_contigTop = off; else g_contigFree[off] = size;
}
static uint32_t ContigTry(uint32_t size, uint32_t align) {   // g_memMutex held
  for (auto it = g_contigFree.begin(); it != g_contigFree.end(); ++it) {   // first fit in the freed ranges
    uint32_t a = (it->first + align - 1) & ~(align - 1);
    if (a + size <= it->first + it->second) {
      uint32_t off = it->first, len = it->second; g_contigFree.erase(it);
      if (a > off) g_contigFree[off] = a - off;
      if (a + size < off + len) g_contigFree[a + size] = off + len - (a + size);
      g_contigAllocs[GA(g_contig + a)] = size;
      memset(g_contig + a, 0, size);   // fresh RAM is zero and drivers rely on it
      return GA(g_contig + a);
    }
  }
  uint32_t a = (g_contigTop + align - 1) & ~(align - 1);
  if (a + size > g_contigSize - 0x100000) return 0;   // the top 1 MB is GPU instance memory
  if (a < g_contigHigh) memset(g_contig + a, 0, std::min(size, g_contigHigh - a));
  g_contigTop = a + size; if (g_contigTop > g_contigHigh) g_contigHigh = g_contigTop;
  g_contigAllocs[GA(g_contig + a)] = size;
  return GA(g_contig + a);
}
static uint32_t ContigAlloc(uint32_t size, uint32_t align) {
  std::lock_guard<std::mutex> l(g_memMutex);
  if (align < 0x1000) align = 0x1000;
  size = (size + 0xFFF) & ~0xFFFu;
  uint32_t r = ContigTry(size, align);
  if (!r && !g_contigQuarantine.empty()) {   // out of RAM: recycle what the title freed (the title may still touch a just-freed block, so reuse is a last resort)
    for (auto& q : g_contigQuarantine) ContigRelease(q.first, q.second);
    g_contigQuarantine.clear(); r = ContigTry(size, align);
  }
  if (!r) Log("[mm] contiguous allocation of 0x%x failed (top=0x%x)", size, g_contigTop);
  return r;
}
static void ContigFree(uint32_t p) {
  std::lock_guard<std::mutex> l(g_memMutex);
  auto it = g_contigAllocs.find(p); if (it == g_contigAllocs.end()) return;
  uint32_t off = p - GA(g_contig);
  if (off < 0x200000) return;   // the first D3D pushbuffer / notifier blocks: the GPU may still be reading them when the title frees them
  g_contigQuarantine.push_back({off, it->second}); g_contigAllocs.erase(it);
}
KFN(MmAllocateContiguousMemory, uint32_t size) { KT(MmAllocateContiguousMemory); return ContigAlloc(size, 0x1000); }
KFN(MmAllocateContiguousMemoryEx, uint32_t size, uint32_t lo, uint32_t hi, uint32_t align, uint32_t prot) { KT(MmAllocateContiguousMemoryEx); { static int n = 0; if (n++ < 20) Log("[mm] MmAllocateContiguousMemoryEx(size=0x%x lo=0x%x hi=0x%x align=0x%x prot=0x%x)", size, lo, hi, align, prot); } return ContigAlloc(size, align); }
KFN(MmFreeContiguousMemory, uint32_t p) { KT(MmFreeContiguousMemory); ContigFree(p); return 0; }
KFN(MmPersistContiguousMemory, uint32_t p, uint32_t size, uint32_t persist) { KT(MmPersistContiguousMemory); return 0; }
// contiguous RAM (mapped at 0x80000000) -> its offset; any other host memory (guest ptr == host ptr, e.g. a pool buffer handed to the NIC) -> p | 0x80000000 so DMA users can tell it apart from a RAM offset
KFN(MmGetPhysicalAddress, uint32_t p) { KT(MmGetPhysicalAddress); return (p >= 0x80000000u) ? (p & 0x03FFFFFFu) : (p >= 0x04000000u ? p : (p | 0x80000000u)); }
KFN(MmQueryAllocationSize, uint32_t p) { KT(MmQueryAllocationSize); auto it = g_contigAllocs.find(p); return it == g_contigAllocs.end() ? 0 : it->second; }
KFN(MmAllocateSystemMemory, uint32_t size, uint32_t prot, uint32_t) { KT(MmAllocateSystemMemory); return GA(GuestVirtualAlloc(size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)); }
KFN(MmFreeSystemMemory, uint32_t p, uint32_t size) { KT(MmFreeSystemMemory); plat::VmFree(G(p), 0, MEM_RELEASE); return 0; }
KFN(MmCreateKernelStack, uint32_t size, uint32_t) { KT(MmCreateKernelStack); uint8_t* p = (uint8_t*)GuestVirtualAlloc(size + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); return GA(p + size + 0x1000); }
KFN(MmDeleteKernelStack, uint32_t base, uint32_t) { KT(MmDeleteKernelStack); return 0; }
KFN(MmLockUnlockBufferPages, uint32_t, uint32_t, uint32_t) { KT(MmLockUnlockBufferPages); return 0; }
KFN(MmLockUnlockPhysicalPage, uint32_t, uint32_t) { KT(MmLockUnlockPhysicalPage); return 0; }
KFN(MmSetAddressProtect, uint32_t p, uint32_t size, uint32_t prot) { KT(MmSetAddressProtect); plat::VmProtectWin(G(p), size, prot & 0xFF); return 0; }
KFN(MmQueryStatistics, uint32_t* st) { KT(MmQueryStatistics);
  uint32_t v[9] = {36, 0x4000, 0x2000, 0, 0, 0, 0, 0, 0};
  memcpy(st, v, 36); return 0;
}
KFN(ExAllocatePoolWithTag, uint32_t size, uint32_t tag) { KT(ExAllocatePoolWithTag); return GuestAlloc(size); }
KFN(ExFreePool, uint32_t p) { KT(ExFreePool); GuestFree(p); return 0; }
KFN(ExQueryPoolBlockSize, uint32_t p) { KT(ExQueryPoolBlockSize); return GuestAllocSize(p); }
KFN(NtAllocateVirtualMemory, uint32_t* base, uint32_t zero, uint32_t* size, uint32_t type, uint32_t prot) { KT(NtAllocateVirtualMemory);
  void* p = *base ? plat::VmAlloc(G(*base), *size, type, (prot & 0xFF) ? (prot & 0xFF) : PAGE_READWRITE) : GuestVirtualAlloc(*size, type, (prot & 0xFF) ? (prot & 0xFF) : PAGE_READWRITE);
  if (!p) { Log("[mm] NtAllocateVirtualMemory FAILED base=0x%x size=0x%x type=0x%x prot=0x%x", *base, *size, type, prot); return STATUS_NO_MEMORY; }
  *base = GA(p); *size = (*size + 0xFFF) & ~0xFFFu; return 0;
}
KFN(NtFreeVirtualMemory, uint32_t* base, uint32_t* size, uint32_t type) { KT(NtFreeVirtualMemory);
  plat::VmFree(G(*base), *size, type);
  return 0;
}
KFN(NtQueryVirtualMemory, uint32_t addr, uint32_t* mbi) { KT(NtQueryVirtualMemory);   // guest layout: BaseAddress, AllocationBase, AllocationProtect, RegionSize, State, Protect, Type (7 x 32 bit)
  plat::VmInfo vi; if (!plat::VmQuery(G(addr), &vi)) return STATUS_INVALID_PARAMETER;
  uint32_t base = GA(vi.base); mbi[0] = base; mbi[1] = base; mbi[2] = PAGE_READWRITE; mbi[3] = (uint32_t)vi.size;
  mbi[4] = vi.state == 0 ? MEM_FREE : vi.state == 1 ? MEM_RESERVE : MEM_COMMIT; mbi[5] = vi.state != 2 ? PAGE_NOACCESS : (vi.prot & plat::kProtWrite) ? PAGE_READWRITE : PAGE_READONLY; mbi[6] = vi.state == 0 ? 0 : MEM_PRIVATE; return 0;
}

// ---- strings / rtl ----
// ANSI_STRING as the guest lays it out: 32-bit buffer address (never a host pointer).
struct AnsiString { uint16_t len, max; uint32_t bufA; char* buf() const { return bufA ? (char*)G(bufA) : nullptr; } };
KFN(RtlInitAnsiString, AnsiString* d, const char* s) { KT(RtlInitAnsiString); d->bufA = s ? GA(s) : 0; d->len = s ? (uint16_t)strlen(s) : 0; d->max = s ? d->len + 1 : 0; return 0; }
KFN(RtlEqualString, AnsiString* a, AnsiString* b, uint32_t ci) { KT(RtlEqualString);
  if (a->len != b->len) return 0;
  return (ci ? _strnicmp(a->buf(), b->buf(), a->len) : strncmp(a->buf(), b->buf(), a->len)) == 0;
}
KFN(RtlCompareMemoryUlong, uint32_t* src, uint32_t len, uint32_t pat) { KT(RtlCompareMemoryUlong); uint32_t i = 0; while (i < len / 4 && src[i] == pat) ++i; return i * 4; }
// NTSTATUS -> Win32 error, as ntdll's table does for the codes the XAPI/title code actually tests (missing files must read as ERROR_FILE_NOT_FOUND, not "damaged")
static uint32_t NtToDos(uint32_t st) {
  switch (st) {
    case 0: return 0;
    case 0xC0000034: case 0xC000000F: return 2;     // OBJECT_NAME_NOT_FOUND, NO_SUCH_FILE -> ERROR_FILE_NOT_FOUND
    case 0xC000003A: case 0xC0000039: return 3;     // OBJECT_PATH_NOT_FOUND / PATH_INVALID -> ERROR_PATH_NOT_FOUND
    case 0xC0000022: case 0xC00000BA: case 0xC0000024: return 5;   // ACCESS_DENIED, FILE_IS_A_DIRECTORY -> ERROR_ACCESS_DENIED
    case 0xC0000008: return 6;                       // INVALID_HANDLE
    case 0x80000006: return 18;                      // NO_MORE_FILES
    case 0xC0000013: return 21;                      // NO_MEDIA_IN_DEVICE -> ERROR_NOT_READY
    case 0xC0000043: case 0xC0000056: return 32;     // SHARING_VIOLATION
    case 0xC0000011: return 38;                      // END_OF_FILE -> ERROR_HANDLE_EOF
    case 0xC00000BB: return 50;                      // NOT_SUPPORTED
    case 0xC000000D: case 0xC00000F2: return 87;     // INVALID_PARAMETER
    case 0xC000007F: return 112;                     // DISK_FULL
    case 0xC0000033: return 123;                     // OBJECT_NAME_INVALID
    case 0xC0000023: return 122;                     // BUFFER_TOO_SMALL -> ERROR_INSUFFICIENT_BUFFER
    case 0xC0000002: return 120;                     // NOT_IMPLEMENTED -> ERROR_CALL_NOT_IMPLEMENTED
    case 0xC0000101: return 145;                     // DIRECTORY_NOT_EMPTY
    case 0xC0000035: return 183;                     // OBJECT_NAME_COLLISION -> ERROR_ALREADY_EXISTS
    case 0x80000005: return 234;                     // BUFFER_OVERFLOW -> ERROR_MORE_DATA
    case 0x00000102: return 258;                     // TIMEOUT -> WAIT_TIMEOUT
    case 0x00000103: return 997;                     // PENDING -> ERROR_IO_PENDING
    case 0xC0000005: return 998;                     // ACCESS_VIOLATION -> ERROR_NOACCESS
    case 0xC0000001: case 0xC0000010: return 31;     // UNSUCCESSFUL / INVALID_DEVICE_REQUEST -> ERROR_GEN_FAILURE
    default: return 317;                             // ERROR_MR_MID_NOT_FOUND, what ntdll returns for unknown codes
  }
}
KFN(RtlNtStatusToDosError, uint32_t st) { KT(RtlNtStatusToDosError); if (st) { static std::map<uint32_t, int> seenSt; if (seenSt[st]++ < 3) Log("[rtl] RtlNtStatusToDosError(0x%08x) from %p -> %u", st, PLAT_RETADDR(), NtToDos(st)); } return NtToDos(st); }
// RTL_CRITICAL_SECTION is 28 bytes on Xbox; the host CRITICAL_SECTION pointer lives in the last dword.
static std::recursive_mutex* CS(uint32_t* g) { return (std::recursive_mutex*)HandleGet(g[6]); }
KFN(RtlInitializeCriticalSection, uint32_t* g) { KT(RtlInitializeCriticalSection); auto* c = new std::recursive_mutex; g[6] = HandleNew(c); g[1] = (uint32_t)-1; g[2] = 0; return 0; }
KFN(RtlEnterCriticalSection, uint32_t* g) { KT(RtlEnterCriticalSection); if (!g[6]) K_RtlInitializeCriticalSection(g); CS(g)->lock(); return 0; }
KFN(RtlLeaveCriticalSection, uint32_t* g) { KT(RtlLeaveCriticalSection); CS(g)->unlock(); return 0; }
extern "C" uint32_t PLAT_CDECL K_DbgPrint(const char* fmt, ...) {
  char buf[512]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  size_t n = strlen(buf); while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
  Log("[DbgPrint] %s", buf); return 0;
}

// ---- time ----
static uint64_t Qpc() { return plat::PerfCounter(); }
static uint64_t Qpf() { return plat::PerfFreq(); }
extern "C" uint64_t PLAT_STDCALL K_KeQueryPerformanceCounter(void) { return (uint64_t)((double)Qpc() * 733333333.0 / (double)Qpf()); }
extern "C" uint64_t PLAT_STDCALL K_KeQueryPerformanceFrequency(void) { return 733333333ull; }
KFN(KeQuerySystemTime, uint64_t* t) { KT(KeQuerySystemTime); *t = plat::SystemTime100ns(); return 0; }
KFN(KeStallExecutionProcessor, uint32_t us) { KT(KeStallExecutionProcessor); uint64_t end = Qpc() + Qpf() * us / 1000000; while (Qpc() < end) {} return 0; }
KFN(NtYieldExecution) { KT(NtYieldExecution); plat::YieldThread(); return 0; }
// ---- IRQL emulation: the title (XNet above all) relies on DISPATCH_LEVEL excluding DPCs/timers and on device-level raises excluding the ISR. We model that with two locks:
// raising a thread to >= DISPATCH takes g_dispatchMx (DPCs and timer routines run holding it); the ISR / KeSynchronizeExecution lock (g_isrMutex) is NOT tied to raises: D3D raises IRQL while waiting on the GPU pusher, whose software-method ISR needs that lock.
// Waits release the dispatch lock while blocked (illegal on real hardware at DISPATCH, so only a safety net).
static std::recursive_mutex g_dispatchMx; static std::recursive_mutex g_isrMutex;
static thread_local uint32_t t_irql = 0; static thread_local bool t_inIsr = false;   // ISRs run at device IRQL and preempt DISPATCH-level code: they must neither take nor release the dispatch lock
static bool g_irqlLocks = getenv("XBOX_NOIRQL") == nullptr;   // XBOX_NOIRQL=1: IRQL levels are tracked but no locks are taken (old behaviour)
static bool g_irqlLog = getenv("XBOX_IRQLLOG") != nullptr;
static uint32_t IrqlRaise(uint32_t n) { n &= 0xFF; uint32_t old = t_irql; if (n >= 2 && old < 2 && !t_inIsr && g_irqlLocks) g_dispatchMx.lock(); if (n > old) t_irql = n; return old; }
static void IrqlLower(uint32_t n) { n &= 0xFF; uint32_t old = t_irql; if (n > old) return; if (old >= 2 && n < 2 && !t_inIsr && g_irqlLocks) g_dispatchMx.unlock(); t_irql = n; }
struct DispatchYield { bool held; DispatchYield() : held(t_irql >= 2 && !t_inIsr && g_irqlLocks) { if (held) g_dispatchMx.unlock(); } ~DispatchYield() { if (held) g_dispatchMx.lock(); } };
KFF(KfRaiseIrql, uint32_t irql) { KT(KfRaiseIrql); if (g_irqlLog) { static int c = 0; if (c++ < 80) Log("[irql] tid=%u KfRaiseIrql(%u) from %p (now %u)", plat::ThreadId(), irql, PLAT_RETADDR(), t_irql); } return IrqlRaise(irql); }
KFF(KfLowerIrql, uint32_t irql) { KT(KfLowerIrql); if (g_irqlLog) { static int c = 0; if (c++ < 80) Log("[irql] tid=%u KfLowerIrql(%u) from %p (now %u)", plat::ThreadId(), irql, PLAT_RETADDR(), t_irql); } IrqlLower(irql); return 0; }
KFN(KeRaiseIrqlToDpcLevel) { KT(KeRaiseIrqlToDpcLevel); if (g_irqlLog) { static int c = 0; if (c++ < 80) Log("[irql] tid=%u KeRaiseIrqlToDpcLevel from %p (now %u)", plat::ThreadId(), PLAT_RETADDR(), t_irql); } return IrqlRaise(2); }
KFN(KeSaveFloatingPointState, uint32_t) { KT(KeSaveFloatingPointState); return 0; }
KFN(KeRestoreFloatingPointState, uint32_t) { KT(KeRestoreFloatingPointState); return 0; }

// ---- dispatcher objects: events/semaphores/mutants/timers passed as kernel structs (KEVENT inline in guest memory) ----
static std::map<uint32_t, plat::Obj*> g_kobjs;
static plat::Obj* KObject(uint32_t addr) {
  std::lock_guard<std::mutex> l(g_memMutex);
  auto it = g_kobjs.find(addr);
  if (it != g_kobjs.end()) return it->second;
  uint8_t type = *(uint8_t*)G(addr); int32_t signal = *(int32_t*)G(addr + 4);
  plat::Obj* h = nullptr;
  switch (type) {
    case 0: h = plat::EventNew(true, signal != 0); break;   // NotificationEvent
    case 1: h = plat::EventNew(false, signal != 0); break;  // SynchronizationEvent
    case 2: h = plat::MutexNew(false); break;
    case 5: h = plat::SemaphoreNew(signal, 0x7FFFFFFF); break;
    case 8: h = plat::EventNew(true, false); break;   // NotificationTimer: signalled by TimerThread
    case 9: h = plat::EventNew(false, false); break;  // SynchronizationTimer
    default: Log("[kernel] unknown dispatcher object type %u at 0x%x", type, addr); h = plat::EventNew(true, false);
  }
  g_kobjs[addr] = h; return h;
}
static uint32_t TimeoutMs(const int64_t* t) {
  if (!t) return plat::kInfinite;
  if (*t < 0) return (uint32_t)((-*t) / 10000);
  return 0;
}
KFN(KeReleaseSemaphore, uint32_t sem, uint32_t inc, uint32_t adj, uint32_t wait) { KT(KeReleaseSemaphore); int32_t prev = 0; plat::SemaphoreRelease(KObject(sem), (int32_t)adj, &prev); return (uint32_t)prev; }
KFN(KeSetEvent, uint32_t ev, uint32_t inc, uint32_t wait) { KT(KeSetEvent); plat::EventSet(KObject(ev)); return 0; }
KFN(KeWaitForSingleObject, uint32_t obj, uint32_t reason, uint32_t mode, uint32_t alertable, int64_t* timeout) { KT(KeWaitForSingleObject); DispatchYield dy_;
  uint32_t r = plat::Wait(KObject(obj), TimeoutMs(timeout));
  return r == plat::kWaitOk ? 0 : r == plat::kWaitTimeout ? STATUS_TIMEOUT : 0xC0;
}
KFN(KeDelayExecutionThread, uint32_t mode, uint32_t alertable, int64_t* t) { KT(KeDelayExecutionThread); DispatchYield dy_; plat::SleepMs(TimeoutMs(t) == plat::kInfinite ? 0x7FFFFFFF : TimeoutMs(t)); return 0; }
KFN(KeInitializeTimerEx, uint32_t timer, uint32_t type) { KT(KeInitializeTimerEx);
  uint8_t* t = (uint8_t*)G(timer); t[0] = type == 0 ? 8 : 9; *(int32_t*)(t + 4) = 0; return 0;
}
// Timers and DPCs run on one helper thread that polls at 1 ms.
struct TimerEntry { uint32_t timer, dpc; uint64_t due, period; };  // due in 100 ns units of KeQuerySystemTime-style absolute time
static std::vector<TimerEntry> g_timers; static std::vector<uint32_t> g_dpcQueue;
static std::mutex g_timerMutex;
static uint64_t NowHns() { return plat::SystemTime100ns(); }
static void RunDpc(uint32_t dpc) { uint32_t* d = (uint32_t*)G(dpc); if (d[3]) { uint32_t old = IrqlRaise(2); GuestCall(d[3], 4, dpc, d[4], d[5], d[6]); IrqlLower(old); } }
static uint32_t TimerThread(void*) {
  XThreadInit(0);
  for (;;) {
    plat::SleepMs(1);
    std::vector<uint32_t> run; std::vector<uint32_t> signal;
    {
      std::lock_guard<std::mutex> l(g_timerMutex);
      uint64_t now = NowHns();
      for (size_t i = 0; i < g_timers.size();) {
        if (g_timers[i].due <= now) {
          signal.push_back(g_timers[i].timer); if (g_timers[i].dpc) run.push_back(g_timers[i].dpc);
          if (g_timers[i].period) { g_timers[i].due = now + g_timers[i].period; ++i; } else g_timers.erase(g_timers.begin() + i);
        } else ++i;
      }
      run.insert(run.end(), g_dpcQueue.begin(), g_dpcQueue.end()); g_dpcQueue.clear();
    }
    for (uint32_t t : signal) plat::EventSet(KObject(t));
    for (uint32_t d : run) RunDpc(d);
  }
}
KFN(KeCancelTimer, uint32_t timer) { KT(KeCancelTimer);
  std::lock_guard<std::mutex> l(g_timerMutex);
  for (size_t i = 0; i < g_timers.size(); i++) if (g_timers[i].timer == timer) { g_timers.erase(g_timers.begin() + i); return 1; }
  return 0;
}
KFN(KeSetTimerEx, uint32_t timer, uint32_t lo, uint32_t hi, uint32_t periodMs, uint32_t dpc) { KT(KeSetTimerEx);
  int64_t t = ((int64_t)hi << 32) | lo; uint64_t due = t < 0 ? NowHns() + (uint64_t)(-t) : (uint64_t)t;
  plat::EventReset(KObject(timer));
  std::lock_guard<std::mutex> l(g_timerMutex);
  bool was = false;
  for (size_t i = 0; i < g_timers.size(); i++) if (g_timers[i].timer == timer) { g_timers.erase(g_timers.begin() + i); was = true; break; }
  g_timers.push_back({timer, dpc, due, (uint64_t)periodMs * 10000});
  return was;
}
KFN(KeSetTimer, uint32_t timer, uint32_t lo, uint32_t hi, uint32_t dpc) { KT(KeSetTimer); return K_KeSetTimerEx(timer, lo, hi, 0, dpc); }
KFN(KeInsertQueueDpc, uint32_t dpc, uint32_t a1, uint32_t a2) { KT(KeInsertQueueDpc);
  uint32_t* d = (uint32_t*)G(dpc); d[5] = a1; d[6] = a2;
  std::lock_guard<std::mutex> l(g_timerMutex); g_dpcQueue.push_back(dpc); return 1;
}
KFN(KeRemoveQueueDpc, uint32_t dpc) { KT(KeRemoveQueueDpc);
  std::lock_guard<std::mutex> l(g_timerMutex);
  for (size_t i = 0; i < g_dpcQueue.size(); i++) if (g_dpcQueue[i] == dpc) { g_dpcQueue.erase(g_dpcQueue.begin() + i); return 1; }
  return 0;
}

// ---- handles ----
enum HKind { H_FILE, H_DIR };
struct HFile { HKind kind; bool disc; DiscEntry de; uint64_t pos; plat::File* host; std::string path; bool rawDisk; bool deleteOnClose; std::vector<std::string> names; size_t dirPos; bool dirInit; };
static std::map<uint32_t, HFile*> g_files;
static uint32_t g_nextFileHandle = 0x00010004;
static uint32_t NewFileHandle(HFile* f) { std::lock_guard<std::mutex> l(g_memMutex); uint32_t h = g_nextFileHandle; g_nextFileHandle += 4; g_files[h] = f; return h; }
static HFile* GetFile(uint32_t h) { std::lock_guard<std::mutex> l(g_memMutex); auto it = g_files.find(h); return it == g_files.end() ? nullptr : it->second; }
KFN(NtClose, uint32_t h) { KT(NtClose);
  {
    std::lock_guard<std::mutex> l(g_memMutex);
    auto it = g_files.find(h);
    if (it != g_files.end()) {
      HFile* f = it->second;
      if (f->host) plat::FileClose(f->host);
      if (f->deleteOnClose && !f->disc) { if (f->kind == H_DIR) plat::RemoveDir(f->path.c_str()); else plat::RemoveFile(f->path.c_str()); }
      delete f; g_files.erase(it); return 0;
    }
  }
  if (void* hh = HandleRelease(h)) plat::ObjClose((plat::Obj*)hh);
  return 0;
}
KFN(NtDuplicateObject, uint32_t src, uint32_t* dst, uint32_t opts) { KT(NtDuplicateObject);
  plat::Obj* d = plat::ObjDup((plat::Obj*)HandleGet(src)); if (!d) return STATUS_INVALID_PARAMETER;
  *dst = HandleNew(d); return 0;
}
struct ObjAttr { uint32_t root; uint32_t nameA; uint32_t attrs; AnsiString* name() const { return nameA ? (AnsiString*)G(nameA) : nullptr; } };
KFN(NtCreateEvent, uint32_t* h, ObjAttr*, uint32_t type, uint32_t state) { KT(NtCreateEvent); *h = HandleNew(plat::EventNew(type == 0, state != 0)); return 0; }
KFN(NtSetEvent, uint32_t h, uint32_t* prev) { KT(NtSetEvent); plat::EventSet((plat::Obj*)HandleGet(h)); if (prev) *prev = 0; return 0; }
KFN(NtCreateSemaphore, uint32_t* h, ObjAttr*, int32_t init, int32_t max) { KT(NtCreateSemaphore); plat::Obj* hs = plat::SemaphoreNew(init, max); *h = HandleNew(hs); if (g_trace) Log("[k]   create sem init=%d max=%d -> h=0x%x os=%p", init, max, *h, (void*)hs); return 0; }
KFN(NtReleaseSemaphore, uint32_t h, int32_t count, int32_t* prev) { KT(NtReleaseSemaphore); if (g_trace) Log("[k]   release h=0x%x count=%d tid=%u", h, count, plat::ThreadId()); int32_t p = 0; bool ok = plat::SemaphoreRelease((plat::Obj*)HandleGet(h), count, &p); if (g_trace) Log("[k]   release ok=%d prev=%d os=%p", (int)ok, (int)p, HandleGet(h)); if (prev) *prev = p; return 0; }
KFN(NtCreateMutant, uint32_t* h, ObjAttr*, uint32_t owned) { KT(NtCreateMutant); *h = HandleNew(plat::MutexNew(owned != 0)); return 0; }
KFN(NtReleaseMutant, uint32_t h, int32_t* prev) { KT(NtReleaseMutant); plat::MutexRelease((plat::Obj*)HandleGet(h)); if (prev) *prev = 0; return 0; }
KFN(NtResumeThread, uint32_t h, uint32_t* prev) { KT(NtResumeThread); uint32_t p = plat::ThreadResume((plat::Obj*)HandleGet(h)); if (prev) *prev = p; return 0; }
static uint32_t WaitResult(uint32_t r) { return r == plat::kWaitOk ? 0 : r == plat::kWaitTimeout ? STATUS_TIMEOUT : 0; }
KFN(NtWaitForSingleObjectEx, uint32_t h, uint32_t mode, uint32_t alertable, int64_t* timeout) { KT(NtWaitForSingleObjectEx); DispatchYield dy_; if (g_trace) Log("[k]   waiting h=0x%x os=%p timeout=%s tid=%u", h, HandleGet(h), timeout ? "finite" : "inf", plat::ThreadId()); uint32_t r = WaitResult(plat::Wait((plat::Obj*)HandleGet(h), TimeoutMs(timeout))); if (g_trace) Log("[k]   wait h=0x%x -> 0x%x tid=%u", h, r, plat::ThreadId()); return r; }
KFN(NtWaitForSingleObject, uint32_t h, uint32_t alertable, int64_t* timeout) { KT(NtWaitForSingleObject); return K_NtWaitForSingleObjectEx(h, 1, alertable, timeout); }
KFN(NtWaitForMultipleObjectsEx, uint32_t count, uint32_t* handles, uint32_t type, uint32_t mode, uint32_t alertable, int64_t* timeout) { KT(NtWaitForMultipleObjectsEx); DispatchYield dy_;
  plat::Obj* hs[64]; if (count > 64) count = 64; for (uint32_t i = 0; i < count; i++) hs[i] = (plat::Obj*)HandleGet(handles[i]);
  uint32_t r = plat::WaitMany(hs, count, type == 0, TimeoutMs(timeout));
  return r < count ? r : WaitResult(r);
}

// ---- threads ----
struct ThreadStart { uint32_t routine, ctx1, ctx2, tlsExtra; };
static uint32_t XboxThreadMain(void* p) {
  ThreadStart ts = *(ThreadStart*)p; delete (ThreadStart*)p;
  XThreadInit(ts.tlsExtra);
  GuestCall(ts.routine, 2, ts.ctx1, ts.ctx2);
  return 0;
}
KFN(PsCreateSystemThreadEx, uint32_t* handle, uint32_t extra, uint32_t stackSize, uint32_t tlsSize, uint32_t* tid, uint32_t ctx1, uint32_t ctx2, uint32_t suspended, uint32_t debugger, uint32_t routine) { KT(PsCreateSystemThreadEx);
  auto* ts = new ThreadStart{routine, ctx1, ctx2, tlsSize};
  uint32_t id = 0;
  plat::Obj* h = plat::ThreadStart(XboxThreadMain, ts, stackSize < 0x100000 ? 0x100000 : stackSize, suspended != 0, &id);
  if (!h) return STATUS_NO_MEMORY;
  if (handle) *handle = HandleNew(h);
  if (tid) *tid = id;
  Log("[ps] thread %u created: routine=0x%x ctx=0x%x,0x%x stack=0x%x suspended=%u", id, routine, ctx1, ctx2, stackSize, suspended);
  return 0;
}
KFN(PsTerminateSystemThread, uint32_t status) { KT(PsTerminateSystemThread); plat::ThreadExit(status); return 0; }
KFN(KeSetBasePriorityThread, uint32_t thread, uint32_t prio) { KT(KeSetBasePriorityThread); return 0; }

// ---- files ----
static std::string g_hddRoot = getenv("XBOX_HDD") && *getenv("XBOX_HDD") ? getenv("XBOX_HDD") : "work/title/hdd";   // XBOX_HDD: a private virtual HDD per instance (two consoles on one PC)
struct Resolved { bool ok; bool disc; std::string rel; std::string host; };
static std::map<std::string, std::string> g_links;  // lower-case link name -> target device path
static Resolved ResolvePathS(std::string p, uint32_t root);
static Resolved ResolvePath(const AnsiString* name, uint32_t root) { return ResolvePathS(std::string(name->buf(), name->len), root); }
static Resolved ResolvePathS(std::string p, uint32_t root) {
  for (char& c : p) if (c == '/') c = '\\';
  auto starts = [&](const char* s) { size_t n = strlen(s); return p.size() >= n && _strnicmp(p.c_str(), s, n) == 0; };
  if (starts("\\??\\")) p.erase(0, 4);
  if (p.size() >= 2 && p[1] == ':') {  // drive letter: use the symbolic link the title (or the kernel) created for it
    std::string key = "\\??\\"; key += (char)tolower((unsigned char)p[0]); key += ':';
    auto it = g_links.find(key);
    if (it != g_links.end()) p = it->second + p.substr(2);
  }
  Resolved r{true, false, "", ""};
  auto drive = [&](char d) -> std::string {  // Xbox drive letters -> partition folder
    switch (toupper(d)) { case 'E': return "Partition1"; case 'C': return "Partition2"; case 'X': return "Partition3"; case 'Y': return "Partition4"; case 'Z': return "Partition5"; case 'T': return "Partition1/TDATA"; case 'U': return "Partition1/UDATA"; default: return ""; }
  };
  std::string rest;
  if (starts("\\Device\\CdRom0")) { r.disc = true; rest = p.substr(14); }
  else if (p.size() >= 2 && p[1] == ':' && toupper(p[0]) == 'D') { r.disc = true; rest = p.substr(2); }
  else if (p.size() >= 2 && p[1] == ':' && !drive(p[0]).empty()) { r.host = g_hddRoot + "/" + drive(p[0]); rest = p.substr(2); }
  else if (starts("\\Device\\Harddisk0\\Partition")) {
    size_t n = strlen("\\Device\\Harddisk0\\Partition"); size_t e = p.find('\\', n);
    r.host = g_hddRoot + "/Partition" + p.substr(n, e == std::string::npos ? std::string::npos : e - n); rest = e == std::string::npos ? "" : p.substr(e);
  } else { r.ok = false; return r; }
  for (char& c : rest) if (c == '\\') c = '/';
  r.rel = rest;
  if (!r.disc) r.host += rest;
  return r;
}
struct IoStatus { uint32_t status, info; };
static uint32_t OpenCommon2(uint32_t* h, ObjAttr* oa, IoStatus* io, uint32_t access, uint32_t disposition, uint32_t options);
static uint32_t OpenCommon(uint32_t* h, ObjAttr* oa, IoStatus* io, uint32_t access, uint32_t disposition, uint32_t options) {
  uint32_t r = OpenCommon2(h, oa, io, access, disposition, options);
  static bool logOpens = getenv("XBOX_LOG_OPENS") != nullptr || g_trace;
  if (logOpens) Log("[fs] open '%.*s' access=0x%x disp=%u opts=0x%x -> 0x%08x", oa->name() ? oa->name()->len : 0, oa->name() ? oa->name()->buf() : "", access, disposition, options, r);
  return r;
}
static uint32_t OpenCommon2(uint32_t* h, ObjAttr* oa, IoStatus* io, uint32_t access, uint32_t disposition, uint32_t options) {
  Resolved r = ResolvePath(oa->name(), oa->root);
  if (!r.ok) { Log("[fs] unknown device path '%.*s'", oa->name()->len, oa->name()->buf()); io->status = STATUS_OBJECT_PATH_NOT_FOUND; return io->status; }
  // \Device\Harddisk0\partition0 is the raw disk (config sectors): back it with a sparse file that reads as zeros past its end.
  bool raw = r.ok && !r.disc && r.rel.empty() && !(options & 1) && r.host.size() > 9 && r.host.rfind("Partition") == r.host.size() - 10;
  HFile* f = new HFile{}; f->disc = r.disc; f->path = r.disc ? r.rel : r.host; f->rawDisk = raw;
  if (r.disc) {
    if (!DiscFind(r.rel.c_str(), &f->de)) { Log("[fs] open FAILED disc:%s", r.rel.c_str()); delete f; io->status = STATUS_OBJECT_NAME_NOT_FOUND; return io->status; }
    f->kind = f->de.dir ? H_DIR : H_FILE; f->host = nullptr;
  } else {
    bool isDir = false; plat::PathExists(r.host.c_str(), &isDir);
    bool write = (access & (0x40000000u | 0x2 | 0x4 | 0x10000)) != 0;
    plat::Disp disp = plat::Disp::OpenExisting;  // FILE_SUPERSEDE(0) FILE_OPEN(1) FILE_CREATE(2) FILE_OPEN_IF(3) FILE_OVERWRITE(4) FILE_OVERWRITE_IF(5)
    switch (disposition) { case 0: disp = plat::Disp::CreateAlways; break; case 1: disp = plat::Disp::OpenExisting; break; case 2: disp = plat::Disp::CreateNew; break; case 3: disp = plat::Disp::OpenAlways; break; case 4: disp = plat::Disp::TruncateExisting; break; case 5: disp = plat::Disp::CreateAlways; break; }
    if (raw) { r.host += ".img"; isDir = false; options &= ~1u; write = true; disp = plat::Disp::OpenAlways; }
    if (isDir || (options & 1)) {  // FILE_DIRECTORY_FILE
      if (!isDir) { if (disposition == 1) { delete f; io->status = STATUS_OBJECT_NAME_NOT_FOUND; return io->status; } plat::MakeDir(r.host.c_str()); }
      f->kind = H_DIR; f->host = nullptr;
    } else {
      bool pathMissing = false; f->host = plat::FileOpen(r.host.c_str(), write, disp, &pathMissing);
      if (!f->host) { delete f; io->status = pathMissing ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND; return io->status; }
      f->kind = H_FILE;
    }
  }
  *h = NewFileHandle(f); io->status = 0; io->info = disposition == 2 ? 2 : 1; return 0;
}
KFN(NtCreateFile, uint32_t* h, uint32_t access, ObjAttr* oa, IoStatus* io, uint64_t* alloc, uint32_t attrs, uint32_t share, uint32_t disposition, uint32_t options) { KT(NtCreateFile); return OpenCommon(h, oa, io, access, disposition, options); }
KFN(NtOpenFile, uint32_t* h, uint32_t access, ObjAttr* oa, IoStatus* io, uint32_t share, uint32_t options) { KT(NtOpenFile); return OpenCommon(h, oa, io, access, 1, options); }
KFN(NtReadFile, uint32_t h, uint32_t event, uint32_t apc, uint32_t apcCtx, IoStatus* io, uint8_t* buf, uint32_t len, int64_t* off) { KT(NtReadFile);
  HFile* f = GetFile(h);
  if (!f || f->kind != H_FILE) { io->status = STATUS_INVALID_PARAMETER; return io->status; }
  uint64_t pos = off && *off >= 0 ? (uint64_t)*off : f->pos;
  if ((g_trace || g_logFs) && !f->disc) Log("[fs] NtReadFile %s pos=0x%llx len=0x%x", f->path.c_str(), (unsigned long long)pos, len);
  uint32_t got = 0;
  if (f->disc) {
    if (pos >= f->de.size) { io->status = STATUS_END_OF_FILE; io->info = 0; return io->status; }
    got = (uint32_t)std::min<uint64_t>(len, f->de.size - pos);
    if (!DiscRead(f->de.offset + pos, buf, got)) { io->status = STATUS_UNSUCCESSFUL; return io->status; }
  } else {
    int64_t rn = plat::FileReadAt(f->host, buf, len, pos); uint32_t n = rn < 0 ? 0 : (uint32_t)rn;
    if (rn < 0) { io->status = STATUS_UNSUCCESSFUL; return io->status; }
    if (f->rawDisk && n < len) { memset(buf + n, 0, len - n); n = len; }
    if (n == 0 && len) { io->status = STATUS_END_OF_FILE; io->info = 0; return io->status; }
    got = n;
  }
  f->pos = pos + got; io->status = 0; io->info = got;
  if (event) plat::EventSet((plat::Obj*)HandleGet(event));
  return 0;
}
KFN(NtWriteFile, uint32_t h, uint32_t event, uint32_t apc, uint32_t apcCtx, IoStatus* io, uint8_t* buf, uint32_t len, int64_t* off) { KT(NtWriteFile);
  HFile* f = GetFile(h);
  if (!f || f->kind != H_FILE || f->disc) { io->status = STATUS_INVALID_PARAMETER; return io->status; }
  uint64_t pos = off && *off >= 0 ? (uint64_t)*off : f->pos;
  int64_t wn = plat::FileWriteAt(f->host, buf, len, pos); if (wn < 0) { io->status = STATUS_UNSUCCESSFUL; return io->status; } uint32_t n = (uint32_t)wn;
  f->pos = pos + n; io->status = 0; io->info = n;
  if (g_logFs) Log("[fs] NtWriteFile %s pos=0x%llx len=0x%x wrote=%u", f->path.c_str(), (unsigned long long)pos, len, n);
  if (event) plat::EventSet((plat::Obj*)HandleGet(event));
  return 0;
}
KFN(NtFlushBuffersFile, uint32_t h, IoStatus* io) { KT(NtFlushBuffersFile); HFile* f = GetFile(h); if (f && f->host) plat::FileFlush(f->host); io->status = 0; return 0; }
static uint64_t FileSize(HFile* f) { if (f->disc) return f->de.size; return f->host ? plat::FileSize(f->host) : 0; }
KFN(NtQueryInformationFile, uint32_t h, IoStatus* io, uint8_t* info, uint32_t len, uint32_t cls) { KT(NtQueryInformationFile);
  HFile* f = GetFile(h);
  if (!f) { io->status = STATUS_INVALID_PARAMETER; return io->status; }
  uint64_t size = f->kind == H_FILE ? FileSize(f) : 0;
  switch (cls) {
    case 5: { uint64_t v[2] = {(size + 0xFFF) & ~0xFFFull, size}; memcpy(info, v, 16); uint32_t links = 1; memcpy(info + 16, &links, 4); info[20] = 0; info[21] = f->kind == H_DIR; io->info = 24; break; }  // FileStandardInformation
    case 14: memcpy(info, &f->pos, 8); io->info = 8; break;                                          // FilePositionInformation
    case 34: {                                                                                       // FileNetworkOpenInformation
      uint64_t v[6] = {0, 0, 0, 0, (size + 0xFFF) & ~0xFFFull, size}; memset(info, 0, 56); memcpy(info, v, 48);
      uint32_t attr = f->kind == H_DIR ? 0x10 : 0x80; if (f->disc) attr |= 1; memcpy(info + 48, &attr, 4); io->info = 56; break; }
    default: Log("[fs] NtQueryInformationFile class %u not implemented", cls); io->status = STATUS_NOT_IMPLEMENTED; return io->status;
  }
  io->status = 0; return 0;
}
KFN(NtSetInformationFile, uint32_t h, IoStatus* io, uint8_t* info, uint32_t len, uint32_t cls) { KT(NtSetInformationFile);
  HFile* f = GetFile(h);
  if (!f) { io->status = STATUS_INVALID_PARAMETER; return io->status; }
  if (cls == 14) { memcpy(&f->pos, info, 8); io->status = 0; return 0; }
  if (cls == 4) { io->status = 0; return 0; }                                                 // FileBasicInformation (times/attributes): ignored
  if (cls == 13) { f->deleteOnClose = len >= 1 && info[0] != 0; io->status = 0; return 0; }   // FileDispositionInformation
  if (cls == 19) { io->status = 0; return 0; }                                                // FileAllocationInformation: ignored
  if (cls == 10 && !f->disc) {                                                                // FileRenameInformation: {BOOLEAN replace; HANDLE root; PSTRING name}
    uint32_t* ri = (uint32_t*)info; AnsiString* nm = (AnsiString*)G(ri[2]);
    Resolved rr = ResolvePath(nm, 0);
    if (!rr.ok || rr.disc) { io->status = STATUS_OBJECT_NAME_NOT_FOUND; return io->status; }
    if (f->host) { plat::FileClose(f->host); f->host = nullptr; }
    bool ok = plat::RenameFile(f->path.c_str(), rr.host.c_str()); f->path = rr.host;
    if (f->kind == H_FILE) f->host = plat::FileOpen(f->path.c_str(), true, plat::Disp::OpenExisting, nullptr);
    io->status = ok ? 0 : STATUS_UNSUCCESSFUL; return io->status;
  }
  if (cls == 20 && !f->disc) { uint64_t sz; memcpy(&sz, info, 8); if (f->host) plat::FileTruncate(f->host, sz); io->status = 0; return 0; }
  Log("[fs] NtSetInformationFile class %u not implemented", cls); io->status = STATUS_NOT_IMPLEMENTED; return io->status;
}
KFN(NtQueryFullAttributesFile, ObjAttr* oa, uint8_t* info) { KT(NtQueryFullAttributesFile);
  IoStatus io{}; uint32_t h;
  if (OpenCommon(&h, oa, &io, 0x80, 1, 0) != 0) return io.status;
  K_NtQueryInformationFile(h, &io, info, 56, 34); K_NtClose(h); return 0;
}
KFN(NtQueryVolumeInformationFile, uint32_t h, IoStatus* io, uint8_t* info, uint32_t len, uint32_t cls) { KT(NtQueryVolumeInformationFile);
  if (g_trace) Log("[fs] NtQueryVolumeInformationFile class=%u len=%u", cls, len);
  memset(info, 0, len < 64 ? len : 64);
  if (cls == 3 && len >= 24) {  // FileFsSizeInformation: TotalAllocationUnits, AvailableAllocationUnits (64-bit), SectorsPerAllocationUnit, BytesPerSector
    HFile* hf = GetFile(h); size_t pp = hf ? hf->path.find("Partition") : std::string::npos;
    int pn = pp != std::string::npos && pp + 9 < hf->path.size() ? hf->path[pp + 9] - '0' : 1;
    static const uint64_t sizes[] = {0x80000, 0x131f00000ull, 0x1f400000, 0x2ee00000, 0x2ee00000, 0x2ee00000};
    // The cluster size comes from the FATX superblock the title wrote when it formatted the partition (offset 8: sectors per cluster).
    uint32_t spc = 32; { char img[260]; snprintf(img, sizeof img, "%s/Partition%d.img", g_hddRoot.c_str(), pn); FILE* fi = fopen(img, "rb"); if (fi) { uint8_t sb[16] = {}; if (fread(sb, 1, 16, fi) == 16 && !memcmp(sb, "FATX", 4)) { uint32_t v; memcpy(&v, sb + 8, 4); if (v && v < 1024) spc = v; } fclose(fi); } }
    uint64_t total = sizes[pn < 0 || pn > 5 ? 1 : pn] / (spc * 512ull), avail = total - 4; memcpy(info, &total, 8); memcpy(info + 8, &avail, 8);
    *(uint32_t*)(info + 16) = spc; *(uint32_t*)(info + 20) = 512;
  }
  io->status = 0; io->info = len; return 0;
}
KFN(NtDeleteFile, ObjAttr* oa) { KT(NtDeleteFile); Resolved r = ResolvePath(oa->name(), oa->root); if (!r.ok || r.disc) return STATUS_OBJECT_NAME_NOT_FOUND; return plat::RemoveFile(r.host.c_str()) ? 0 : STATUS_OBJECT_NAME_NOT_FOUND; }

// ---- symbolic links (drive letters) ----
static std::vector<std::string> g_linkHandles;
static std::string Lw(std::string x) { for (char& c : x) c = (char)tolower((unsigned char)c); return x; }
KFN(NtOpenSymbolicLinkObject, uint32_t* h, ObjAttr* oa) {
  KT(NtOpenSymbolicLinkObject);
  std::string n = Lw(std::string(oa->name()->buf(), oa->name()->len));
  auto it = g_links.find(n);
  if (it == g_links.end()) return STATUS_OBJECT_NAME_NOT_FOUND;
  std::lock_guard<std::mutex> l(g_memMutex); g_linkHandles.push_back(it->second); *h = 0x00200000 + (uint32_t)g_linkHandles.size() * 4; return 0;
}
KFN(NtQuerySymbolicLinkObject, uint32_t h, AnsiString* target, uint32_t* retLen) {
  KT(NtQuerySymbolicLinkObject);
  uint32_t i = (h - 0x00200000) / 4;
  if (i == 0 || i > g_linkHandles.size()) return STATUS_INVALID_PARAMETER;
  const std::string& t = g_linkHandles[i - 1];
  if (target->max < t.size()) return 0xC0000023;  // STATUS_BUFFER_TOO_SMALL
  memcpy(target->buf(), t.data(), t.size()); target->len = (uint16_t)t.size(); if (retLen) *retLen = (uint32_t)t.size(); return 0;
}
KFN(IoCreateSymbolicLink, AnsiString* link, AnsiString* target) { KT(IoCreateSymbolicLink); g_links[Lw(std::string(link->buf(), link->len))] = std::string(target->buf(), target->len); Log("[io] IoCreateSymbolicLink %.*s -> %.*s", link->len, link->buf(), target->len, target->buf()); return 0; }
KFN(IoDeleteSymbolicLink, AnsiString* link) { KT(IoDeleteSymbolicLink); return 0; }

// ---- PCI config space: only the NV2A (bus 1, device 0) is modelled; everything else reads as 'no device' ----
static uint8_t g_nv2aCfg[256];
KFN(HalReadWritePCISpace, uint32_t bus, uint32_t slot, uint32_t reg, uint8_t* buf, uint32_t len, uint32_t write) {
  KT(HalReadWritePCISpace);
  static bool init = false;
  if (!init) {
    init = true; uint32_t* c = (uint32_t*)g_nv2aCfg;
    c[0] = 0x02A010DE;      // vendor 0x10DE, device 0x02A0 (NV2A)
    c[1] = 0x02B00007;      // status/command: memory + bus master enabled
    c[2] = 0x030000A1;      // class: display controller, revision A1
    c[4] = 0xFD000000;      // BAR0: registers
    c[5] = 0xF0000000;      // BAR1: framebuffer
    c[0x0F] = 0x0103;       // interrupt line 3, pin 1
  }
  bool nv2a = bus == 1 && (slot & 31) == 0 && (slot >> 5) == 0;
  if (reg + len > 256) len = reg < 256 ? 256 - reg : 0;
  if (write) { if (nv2a) memcpy(g_nv2aCfg + reg, buf, len); }
  else if (nv2a) memcpy(buf, g_nv2aCfg + reg, len);
  else memset(buf, 0xFF, len);
  return len;
}
// ---- interrupts ----
struct IntrEntry { uint32_t intr, routine, ctx, vector; bool connected; };
static std::vector<IntrEntry> g_intrs;
KFN(MmClaimGpuInstanceMemory, uint32_t bytes, uint32_t* padding) { KT(MmClaimGpuInstanceMemory); if (padding) *padding = 0; return GA(g_contig + g_contigSize - bytes); }
KFN(HalGetInterruptVector, uint32_t level, uint32_t* irql) { KT(HalGetInterruptVector); if (irql) *irql = level; return 0x30 + level; }
KFN(KeInitializeInterrupt, uint32_t* intr, uint32_t routine, uint32_t ctx, uint32_t vector, uint32_t irql, uint32_t mode, uint32_t share) {
  KT(KeInitializeInterrupt); intr[0] = routine; intr[1] = ctx; intr[2] = vector; intr[3] = irql; return 0;
}
KFN(KeConnectInterrupt, uint32_t* intr) {
  KT(KeConnectInterrupt); std::lock_guard<std::recursive_mutex> l(g_isrMutex);
  g_intrs.push_back({GA(intr), intr[0], intr[1], intr[2], true}); Log("[irq] connected vector 0x%x routine 0x%x", intr[2], intr[0]); return 1;
}
KFN(KeDisconnectInterrupt, uint32_t* intr) {
  KT(KeDisconnectInterrupt); std::lock_guard<std::recursive_mutex> l(g_isrMutex);
  for (size_t i = 0; i < g_intrs.size(); i++) if (g_intrs[i].intr == GA(intr)) { g_intrs.erase(g_intrs.begin() + i); return 1; }
  return 0;
}
KFN(KeSynchronizeExecution, uint32_t intr, uint32_t routine, uint32_t ctx) {
  KT(KeSynchronizeExecution); std::lock_guard<std::recursive_mutex> l(g_isrMutex);
  return GuestCall(routine, 1, ctx) & 0xFF;
}
uint8_t* KernelRamBase() { return g_contig; }
void KernelDeliverInterrupt(uint32_t vector) {
  std::lock_guard<std::recursive_mutex> l(g_isrMutex);
  uint32_t saved = t_irql; bool savedIsr = t_inIsr; t_inIsr = true; t_irql = 0x1B;
  for (auto& e : g_intrs) if (e.vector == vector) GuestCall(e.routine, 2, e.intr, e.ctx);
  t_irql = saved; t_inIsr = savedIsr;
}
// ---- video (AV) ----
static uint32_t g_avFramebuffer = 0, g_avMode = 0, g_avFormat = 0, g_avPitch = 0;
uint32_t g_avFramebufferForDisplay, g_avFormatForDisplay, g_avPitchForDisplay;
// Option 6 = get encoder settings (AV pack | video standard | mode flags); titles that list modes themselves (nxdk) need a real value: <title dir>/av_encoder.txt (hex), e.g. 101 = standard AV pack, NTSC-M.
KFN(AvSendTVEncoderOption, uint32_t base, uint32_t option, uint32_t param, uint32_t* result) { KT(AvSendTVEncoderOption);
  static uint32_t enc = [] { uint32_t v = 0; const char* dir = getenv("XBOX_TITLE_DIR"); if (dir) { char f[300]; snprintf(f, sizeof f, "%s/av_encoder.txt", dir); if (FILE* fp = fopen(f, "r")) { fscanf(fp, "%x", &v); fclose(fp); } } return v; }();
  if (result) *result = option == 6 ? enc : 0; return 0; }
KFN(AvSetDisplayMode, uint32_t base, uint32_t step, uint32_t mode, uint32_t format, uint32_t pitch, uint32_t fb) {
  KT(AvSetDisplayMode); g_avMode = mode; g_avFormat = format; g_avPitch = pitch; g_avFramebuffer = fb; g_avFramebufferForDisplay = fb; g_avFormatForDisplay = format; g_avPitchForDisplay = pitch;
  Log("[av] AvSetDisplayMode step=%u mode=0x%x format=0x%x pitch=%u fb=0x%x", step, mode, format, pitch, fb); return 0;
}
// ---- directories ----
KFN(NtQueryDirectoryFile, uint32_t h, uint32_t event, uint32_t apc, uint32_t ctx, IoStatus* io, uint8_t* out, uint32_t len, uint32_t cls, AnsiString* mask, uint32_t restart) {
  KT(NtQueryDirectoryFile);
  HFile* f = GetFile(h);
  if (!f || f->kind != H_DIR) { io->status = STATUS_INVALID_PARAMETER; return io->status; }
  if (!f->dirInit || restart) {
    f->dirInit = true; f->dirPos = 0; f->names.clear();
    std::string pat = mask && mask->len ? std::string(mask->buf(), mask->len) : "*";
    if (f->disc) {  // disc directory: list children from the XDVDFS table
      std::vector<std::string> kids; DiscList(f->path.c_str(), &kids);
      for (auto& k : kids) f->names.push_back(k);
    } else {
      for (auto& n : plat::ListDir(f->path.c_str(), pat.c_str())) f->names.push_back(n);
    }
  }
  if (f->dirPos >= f->names.size()) { io->status = 0x80000006; io->info = 0; return io->status; }  // STATUS_NO_MORE_FILES
  const std::string& name = f->names[f->dirPos++];
  // FILE_DIRECTORY_INFORMATION (class 1): NextEntryOffset, FileIndex, 4 x FILETIME, EndOfFile, AllocationSize, FileAttributes, FileNameLength, FileName[]
  uint32_t need = 64 + (uint32_t)name.size();
  if (len < need) { io->status = 0x80000005; return io->status; }  // STATUS_BUFFER_OVERFLOW
  memset(out, 0, need);
  uint64_t size = 0; uint32_t attr = 0x80;
  if (!f->disc) { plat::StatInfo si; if (plat::PathStat((f->path + "/" + name).c_str(), &si)) { size = si.size; attr = si.isDir ? 0x10 : 0x80; memcpy(out + 8, &si.created, 8); memcpy(out + 16, &si.accessed, 8); memcpy(out + 24, &si.written, 8); memcpy(out + 32, &si.written, 8); } }
  memcpy(out + 40, &size, 8); size = (size + 0xFFF) & ~0xFFFull; memcpy(out + 48, &size, 8);
  memcpy(out + 56, &attr, 4); uint32_t nl = (uint32_t)name.size(); memcpy(out + 60, &nl, 4); memcpy(out + 64, name.data(), nl);
  io->status = 0; io->info = need; return 0;
}
// ---- objects ----
// The "object" of a handle is a small guest-memory stand-in that KObject() maps back to the host handle (so KeSetEvent / KeWaitForSingleObject on it work).
KFN(ObReferenceObjectByHandle, uint32_t h, uint32_t type, uint32_t* obj) { KT(ObReferenceObjectByHandle);
  static const bool legacy = getenv("XBOX_OBREF_LEGACY") != nullptr; if (legacy) { if (obj) *obj = h; return 0; }
  static std::map<uint32_t, uint32_t> cache; std::lock_guard<std::mutex> l(g_memMutex);
  uint32_t& p = cache[h]; if (!p) { p = GuestAlloc(0x40); g_kobjs[p] = (plat::Obj*)HandleGet(h); }
  if (obj) *obj = p; return 0;
}
// ---- devices / network PHY ----
KFN(IoCreateDevice, uint32_t driver, uint32_t extSize, AnsiString* name, uint32_t devType, uint32_t exclusive, uint32_t* out) {
  KT(IoCreateDevice);
  uint32_t devA = GuestAlloc(0x80 + extSize); uint8_t* dev = G(devA);
  if (!dev) return STATUS_NO_MEMORY;
  *(uint16_t*)(dev + 0) = 3; *(uint16_t*)(dev + 2) = (uint16_t)(0x80 + extSize); *(uint32_t*)(dev + 4) = 0; *(uint32_t*)(dev + 8) = driver;
  *(uint32_t*)(dev + 0x18) = devA + 0x80;  // DeviceExtension
  dev[0x1C] = (uint8_t)devType;
  *out = devA; return 0;
}
KFN(PhyInitialize, uint32_t reset, uint32_t param) { KT(PhyInitialize); return 0; }
KFN(PhyGetLinkState, uint32_t mode) { KT(PhyGetLinkState); return 0x0B; }  // link up | 100 Mbps | full duplex
KFF(ObfDereferenceObject, uint32_t obj) { return 0; }
// ---- misc ----
KFN(NtFsControlFile, uint32_t h, uint32_t event, uint32_t apc, uint32_t ctx, IoStatus* io, uint32_t code, uint8_t* in, uint32_t inLen, uint8_t* out, uint32_t outLen) {
  KT(NtFsControlFile);
  HFile* f = GetFile(h);
  Log("[ioctl] NtFsControlFile %s code=0x%08x in=%u out=%u", f ? f->path.c_str() : "?", code, inLen, outLen);
  io->status = 0; io->info = 0; return 0;
}
KFN(NtDeviceIoControlFile, uint32_t h, uint32_t event, uint32_t apc, uint32_t ctx, IoStatus* io, uint32_t code, uint8_t* in, uint32_t inLen, uint8_t* out, uint32_t outLen) {
  KT(NtDeviceIoControlFile);
  HFile* f = GetFile(h);
  Log("[ioctl] NtDeviceIoControlFile %s code=0x%08x in=%u out=%u", f ? f->path.c_str() : "?", code, inLen, outLen);
  if (code == 0x70000 && out && outLen >= 24) {  // IOCTL_DISK_GET_DRIVE_GEOMETRY
    uint64_t bytes = 0x2ee00000ull; uint64_t cyl = bytes / 512 / (63 * 255);
    memset(out, 0, 24); memcpy(out, &cyl, 8); *(uint32_t*)(out + 8) = 12; *(uint32_t*)(out + 12) = 255; *(uint32_t*)(out + 16) = 63; *(uint32_t*)(out + 20) = 512;
    io->status = 0; io->info = 24; return 0;
  }
  if (code == 0x74004 && out && outLen >= 32) {  // IOCTL_DISK_GET_PARTITION_INFO
    static const struct { uint64_t off, len; } parts[] = {{0, 0x80000}, {0xabe80000ull, 0x131f00000ull}, {0x8ca80000ull, 0x1f400000}, {0x80000, 0x2ee00000}, {0x2ee80000ull, 0x2ee00000}, {0x5dc80000ull, 0x2ee00000}};
    int n = 0; if (f && !f->path.empty() && isdigit((unsigned char)f->path.back())) n = f->path.back() - '0'; if (n > 5) n = 5;
    memset(out, 0, 32); memcpy(out, &parts[n].off, 8); memcpy(out + 8, &parts[n].len, 8); *(uint32_t*)(out + 20) = n; out[24] = 0x78; out[26] = 1;
    io->status = 0; io->info = 32; return 0;
  }
  io->status = 0xC0000010; io->info = 0; return io->status;  // STATUS_INVALID_DEVICE_REQUEST
}
KFN(XeLoadSection, uint32_t section) { return 0; }    // the whole image is already mapped
KFN(XeUnloadSection, uint32_t section) { return 0; }
uint32_t EepromQuery(uint32_t index, uint32_t* type, void* value, uint32_t len, uint32_t* resultLen);
KFN(ExQueryNonVolatileSetting, uint32_t index, uint32_t* type, uint32_t* value, uint32_t len, uint32_t* resultLen) { KT(ExQueryNonVolatileSetting);
  uint32_t r = EepromQuery(index, type, value, len, resultLen);
  { static int n = 0; if (n++ < 40) Log("[eeprom] ExQueryNonVolatileSetting index=0x%x len=%u -> 0x%x (from %p)", index, len, r, PLAT_RETADDR()); }
  return r;
}
KFN(HalRegisterShutdownNotification, uint32_t reg, uint32_t add) { KT(HalRegisterShutdownNotification); return 0; }
KFN(KeInitializeDpc, uint32_t* dpc, uint32_t routine, uint32_t ctx) { KT(KeInitializeDpc); memset(dpc, 0, 0x1C); dpc[0] = 0x13; dpc[3] = routine; dpc[4] = ctx; return 0; }
// Title launch: XLaunchNewImage fills *LaunchDataPage (type, title id, "<device path>;<xbe name>", 3072 bytes of launch data at +0x400) and asks the kernel for a
// quick reboot (HalReturnToFirmware(2)). We emulate the reboot by starting a fresh host process for the requested XBE; the page travels in work/launch_page.bin
// (XBOX_LAUNCH_PAGE) and is mapped again as *LaunchDataPage. Per-title config dirs come from work/titles.txt ("<xbe file name> <config dir>"). With no page the
// firmware starts the dashboard.
// XeImageFileName (data export 326, an ANSI_STRING): the running XBE's device path, e.g. \Device\Harddisk0\Partition2\XODash\xonlinedash.xbe (titles derive their own directory from it).
void KernelSetImageName(const char* hostXbe) {
  std::string p = hostXbe; for (char& c : p) if (c == '/') c = '\\';
  std::string root = g_hddRoot; for (char& c : root) if (c == '/') c = '\\';
  std::string dev;
  if (_strnicmp(p.c_str(), root.c_str(), root.size()) == 0) { std::string rest = p.substr(root.size()); if (!rest.empty() && rest[0] == '\\') rest.erase(0, 1); dev = "\\Device\\Harddisk0\\" + rest; }
  else { size_t sl = p.find_last_of('\\'); dev = "\\Device\\Cdrom0\\" + (sl == std::string::npos ? p : p.substr(sl + 1)); }
  AnsiString* a = (AnsiString*)KernelVariable(326); uint32_t bufA = GuestAlloc((uint32_t)dev.size() + 8); memcpy(G(bufA), dev.c_str(), dev.size());
  a->len = (uint16_t)dev.size(); a->max = (uint16_t)(dev.size() + 1); a->bufA = bufA; Log("[xbe] XeImageFileName = %s", dev.c_str());
}
uint32_t KernelContigAlloc(uint32_t size) { return ContigAlloc(size, 0x1000); }
void LaunchPageInit() {
  const char* f = getenv("XBOX_LAUNCH_PAGE"); if (!f || !*f) return;
  FILE* fp = fopen(f, "rb"); if (!fp) return;
  uint32_t page = ContigAlloc(0x1000, 0x1000); if (page && fread((void*)G(page), 1, 0x1000, fp) > 0) { *(uint32_t*)KernelVariable(164) = page; Log("[launch] LaunchDataPage = 0x%x (type %u, title 0x%x, path '%s')", page, *(uint32_t*)G(page), *(uint32_t*)G(page + 4), (const char*)G(page + 8)); }
  fclose(fp);
}
static void LaunchNextTitle() {
  uint32_t page = *(uint32_t*)KernelVariable(164); std::string path = page ? (const char*)G(page + 8) : "";
  if (path.empty()) path = "\\Device\\Harddisk0\\Partition2;xboxdash.xbe";
  size_t semi = path.find(';'); std::string dev = path.substr(0, semi), xbe = semi == std::string::npos ? "" : path.substr(semi + 1);
  std::string full = dev + (xbe.empty() ? "" : "\\" + xbe); Resolved r = ResolvePathS(full, 0);
  Log("[launch] next title: '%s' -> %s", path.c_str(), r.host.c_str());
  if (!r.ok || r.disc) { Log("[launch] cannot resolve the launch path"); return; }
  std::string name = xbe.substr(xbe.find_last_of("\\/") + 1), dir = "work/title";
  if (FILE* tf = fopen("work/titles.txt", "r")) { char a[200], b[200]; while (fscanf(tf, "%199s %199s", a, b) == 2) if (!_stricmp(a, name.c_str())) dir = b; fclose(tf); }
  if (page) { if (FILE* pf = fopen("work/launch_page.bin", "wb")) { fwrite((void*)G(page), 1, 0x1000, pf); fclose(pf); } plat::SetEnv("XBOX_LAUNCH_PAGE", "work/launch_page.bin"); } else plat::SetEnv("XBOX_LAUNCH_PAGE", "");
  plat::SetEnv("XBOX_TITLE_DIR", dir.c_str()); plat::SetEnv("XBOX_ATTEMPT", "0");
  if (plat::SpawnSelf({"none", r.host, "3600"})) { Log("[launch] started %s (config %s)", name.c_str(), dir.c_str()); fflush(stdout); }
  else Log("[launch] this platform cannot start another title in a new process");
}
KFN(HalReturnToFirmware, uint32_t fn) { KT(HalReturnToFirmware); Log("[hal] HalReturnToFirmware(%u) from %p", fn, PLAT_RETADDR()); if (fn == 2 && !getenv("XBOX_NOLAUNCH")) LaunchNextTitle(); fflush(stdout); exit(0); return 0; }
KFN(KeBugCheck, uint32_t code) { KT(KeBugCheck); Log("[kernel] KeBugCheck 0x%x from %p", code, PLAT_RETADDR()); fflush(stdout); exit(1); return 0; }
// The saved framebuffer a previous title (normally the dashboard) left behind; titles that never call AvSetDisplayMode (nxdk homebrew) draw into it.
static uint32_t g_avSaved = 0;
KFN(AvGetSavedDataAddress) { KT(AvGetSavedDataAddress);
  if (!g_avSaved) { g_avSaved = ContigAlloc(640 * 480 * 4, 0x1000); if (g_avSaved) { g_avFramebufferForDisplay = g_avSaved & 0x03FFFFFF; g_avFormatForDisplay = 0x1e; g_avPitchForDisplay = 2560; Log("[av] saved framebuffer 0x%x (640x480 X8R8G8B8)", g_avSaved); } }
  return g_avSaved; }
KFN(AvSetSavedDataAddress, uint32_t a) { KT(AvSetSavedDataAddress); g_avSaved = a; return 0; }

static uint32_t TickThread(void*) { for (;;) { *g_tickCount = (uint32_t)plat::TickMs(); plat::SleepMs(1); } }
void DesSelfTest();
bool KernelContigOk() { return g_contig && GA(g_contig) == 0x80000000u; }
void KernelInit() {
  if (getenv("XBOX_CRYPTO_TEST")) DesSelfTest();
  for (const char* d : {"work", "work/title", "work/title/hdd", "work/title/hdd/Partition1", "work/title/hdd/Partition1/TDATA", "work/title/hdd/Partition1/UDATA", "work/title/hdd/Partition2", "work/title/hdd/Partition3", "work/title/hdd/Partition4", "work/title/hdd/Partition5"}) plat::MakeDir(d);
  { const char* lp = getenv("REFORGED_LOG"); g_log = fopen(lp && *lp ? lp : "reforged.log", "w"); }   // REFORGED_LOG: absolute path (the iOS app keeps the log at the top of its folder while each game has its own working folder)
  if (const char* note = getenv("REFORGED_ENV_NOTE")) Log("[env] %s", note);
  // Physical RAM is a file mapping so the GPU's instance-memory window (PRAMIN, top 1 MB) can alias it at 0xFD700000.
  g_ramMap = plat::ShmCreate(g_contigSize);
  GuestMemInit();
  g_contig = (uint8_t*)plat::ShmMap(g_ramMap, 0, g_contigSize, G(0x80000000u));
  if (!g_contig) { g_contig = (uint8_t*)plat::ShmMap(g_ramMap, 0, g_contigSize, nullptr); Log("[mm] could not map contiguous memory at 0x80000000, using %p", g_contig); }
  g_tickCount = (volatile uint32_t*)KernelVariable(156);
  uint16_t* ver = (uint16_t*)KernelVariable(324); ver[0] = 1; ver[1] = 0; ver[2] = 5838; ver[3] = 1;  // XboxKrnlVersion 1.0.5838.1
  uint32_t* hw = (uint32_t*)KernelVariable(322); hw[0] = 0; hw[1] = 0xD2;                            // XboxHardwareInfo: flags, GpuRevision(NV2A rev), McpRevision
  *(uint32_t*)KernelVariable(40) = 3;  // HalDiskCachePartitionCount: X:, Y:, Z:
  { void EepromInit(); const uint8_t* EepromImage(); const uint8_t* EepromHddKey(); EepromInit(); const uint8_t* ee = EepromImage();   // console identity: XboxHDKey from the EEPROM, the LAN / signature keys derived from it (not secrets of a real console: we generated the EEPROM)
    // Saves (title profiles) are signed with these keys: a title dir containing legacy_keys.txt keeps the old all-zero keys so existing saves stay valid.
    bool legacy = false; { const char* td = getenv("XBOX_TITLE_DIR"); char lf[300]; snprintf(lf, sizeof lf, "%s/legacy_keys.txt", td ? td : "work/title"); if (FILE* lk = fopen(lf, "r")) { fclose(lk); legacy = true; Log("[eeprom] legacy (all-zero) keys for this title"); } }
    if (legacy) { /* KernelVariable memory is zero-initialised */ } else {
    memcpy(KernelVariable(323), EepromHddKey(), 16); for (int i = 0; i < 16; i++) { ((uint8_t*)KernelVariable(353))[i] = EepromHddKey()[i] ^ ee[0x48 + i] ^ 0x4C; ((uint8_t*)KernelVariable(325))[i] = ee[0x48 + i] ^ 0x53; } } }
  static char model[] = "ST380011A", serial[] = "5JV0HDFA";
  uint32_t* ms = (uint32_t*)KernelVariable(41); ms[0] = (sizeof model - 1) | ((sizeof model) << 16); { uint32_t a = GuestAlloc(16); memcpy(G(a), model, sizeof model); ms[1] = a; }   // HalDiskModelNumber (ANSI_STRING)
  uint32_t* ss = (uint32_t*)KernelVariable(42); ss[0] = (sizeof serial - 1) | ((sizeof serial) << 16); { uint32_t a = GuestAlloc(16); memcpy(G(a), serial, sizeof serial); ss[1] = a; } // HalDiskSerialNumber
  plat::ThreadSpawn(TickThread);
  plat::ThreadSpawn(TimerThread);
  plat::BeginHighResTimer();
}
