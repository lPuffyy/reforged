// Additional kernel exports needed by the Xbox Dashboard (and other homebrew/system XBEs): read/write locks, SMC tray state, a few misc services.
#include "kernel.h"
#include "guestmem.h"
#include <cstring>
#include <map>
#include <mutex>

#define KFN(name, ...) extern "C" uint32_t PLAT_STDCALL K_##name(__VA_ARGS__)

namespace {
std::recursive_mutex g_rwMx; std::map<uint32_t, std::recursive_mutex*> g_rw;   // ERWLOCK (guest memory) -> host lock
std::recursive_mutex* Lock(uint32_t p) { std::lock_guard<std::recursive_mutex> l(g_rwMx); auto& m = g_rw[p]; if (!m) m = new std::recursive_mutex; return m; }
}
KFN(ExInitializeReadWriteLock, uint32_t lock) { if (lock) memset(G(lock), 0, 16); Lock(lock); return 0; }
KFN(ExAcquireReadWriteLockExclusive, uint32_t lock) { Lock(lock)->lock(); return 0; }
KFN(ExAcquireReadWriteLockShared, uint32_t lock) { Lock(lock)->lock(); return 0; }   // shared acquires are serialised too: simple and safe
KFN(ExReleaseReadWriteLock, uint32_t lock) { Lock(lock)->unlock(); return 0; }
KFN(HalReadSMCTrayState, uint32_t* state, uint32_t* count) { if (state) *state = 0x10; if (count) *count = 1; return 0; }   // tray closed, no media (the dashboard just wants a sane answer)
KFN(HalEnableSecureTrayEject) { return 0; }
KFN(FscSetCacheSize, uint32_t size) { return 0; }
KFN(ExReadWriteRefurbInfo, uint32_t info, uint32_t size, uint32_t write) { if (!write && info && size) memset(G(info), 0, size); return 0; }
void EepromSave(uint32_t index, uint32_t value);
KFN(ExSaveNonVolatileSetting, uint32_t index, uint32_t type, uint32_t value, uint32_t len) { Log("[eeprom] ExSaveNonVolatileSetting index=%u type=%u len=%u", index, type, len); if (type == 4 && len == 4 && value) EepromSave(index, *(uint32_t*)(uintptr_t)value); return 0; }
KFN(NtClearEvent, uint32_t h) { plat::EventReset((plat::Obj*)HandleGet(h)); return 0; }
KFN(NtSignalAndWaitForSingleObjectEx, uint32_t sig, uint32_t wait, uint32_t mode, uint32_t alertable, int64_t* timeout) {
  uint32_t ms = !timeout ? plat::kInfinite : (*timeout < 0 ? (uint32_t)(-(*timeout) / 10000) : 0); uint32_t r = plat::SignalAndWait((plat::Obj*)HandleGet(sig), (plat::Obj*)HandleGet(wait), ms);
  return r == plat::kWaitOk ? 0 : r == plat::kWaitTimeout ? STATUS_TIMEOUT : 0x101; }
KFN(RtlUpcaseUnicodeChar, uint32_t c) { return (uint32_t)towupper((wint_t)(c & 0xFFFF)); }
KFN(Unknown91, uint32_t a) { static int n = 0; if (n++ < 3) Log("[kernel] Unknown91 called (ordinal 91, stubbed)"); return 0; }
KFN(Unknown124, uint32_t a, uint32_t b) { static int n = 0; if (n++ < 3) Log("[kernel] Unknown124 called (ordinal 124, stubbed)"); return 0; }

extern "C" uint32_t PLAT_FASTCALL K_InterlockedDecrement(uint32_t p) { return (uint32_t)(plat::AtomicAdd((volatile int32_t*)G(p), -1) - 1); }
extern "C" uint32_t PLAT_FASTCALL K_InterlockedIncrement(uint32_t p) { return (uint32_t)(plat::AtomicAdd((volatile int32_t*)G(p), 1) + 1); }
extern "C" uint32_t PLAT_FASTCALL K_InterlockedExchange(uint32_t p, uint32_t v) { return (uint32_t)plat::AtomicExchange((volatile int32_t*)G(p), (int32_t)v); }
KFN(KeGetCurrentThread) { uint8_t* pcr = G(XPcrGet()); return pcr ? *(uint32_t*)(pcr + 0x28) : 0; }   // the KTHREAD built by XThreadInit (its +0x28 = TlsData)
KFN(KeInitializeEvent, uint32_t ev, uint32_t type, uint32_t state) { uint8_t* e = G(ev); memset(e, 0, 0x10); e[0] = (uint8_t)type; e[2] = 4; *(int32_t*)(e + 4) = state ? 1 : 0; return 0; }
KFN(KeInitializeSemaphore, uint32_t sem, uint32_t count, uint32_t limit) { uint8_t* e = G(sem); memset(e, 0, 0x14); e[0] = 5; e[2] = 5; *(int32_t*)(e + 4) = count; *(int32_t*)(e + 0x10) = limit; return 0; }
KFN(Unknown320, uint32_t a, uint32_t b) { return 0; }   // ordinal 320 (name unknown): called with (buffer, length); a no-op is enough for the Insignia assistant
