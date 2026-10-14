// NV2A register window (0xFD000000, 16 MB). The title's D3D library reads and writes the GPU registers directly. The window is committed but
// PAGE_NOACCESS; an access fault makes the page accessible for exactly one instruction (trap flag), so any x86 instruction form works without a decoder:
//  - read: the dword at the faulting address is filled from the register model before the instruction runs,
//  - write: after the instruction the changed bytes are forwarded to the register model.
#include "kernel.h"
#include "guestmem.h"
#include "plat.h"
#ifndef LIFT_NO_NATIVE   // the page-fault trap (native 32-bit builds only) needs the Windows exception API
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

static constexpr uint32_t kBase = 0xFD000000u, kSize = 0x1000000u;
static constexpr uint32_t kUsbBase = 0xFED00000u, kUsbSize = 0x10000u;  // OHCI controllers: USB0 at +0, USB1 at +0x8000
uint32_t UsbRead(uint32_t off);   // usb_ohci.cpp
void UsbWrite(uint32_t off, uint32_t v);
static bool IsUsb(uint32_t a) { return a >= kUsbBase && a < kUsbBase + kUsbSize; }
static constexpr uint32_t kApuBase = 0xFE800000u, kApuSize = 0x80000u;  // MCPX audio processor (voices, DSP memory, FIFOs)
static bool IsApu(uint32_t a) { return a >= kApuBase && a < kApuBase + kApuSize; }
static constexpr uint32_t kAcBase = 0xFEC00000u, kAcSize = 0x1000u;  // AC'97 controller + codec mixer
static bool IsAc(uint32_t a) { return a >= kAcBase && a < kAcBase + kAcSize; }
static uint32_t AcRead(uint32_t off);
static void AcWrite(uint32_t off, uint32_t v);
static constexpr uint32_t kNicBase = 0xFEF00000u, kNicSize = 0x1000u;  // nForce MAC (NVNET)
static bool IsNic(uint32_t a) { return a >= kNicBase && a < kNicBase + kNicSize; }
uint32_t NvnetRead(uint32_t off);
void NvnetWrite(uint32_t off, uint32_t v);
static uint32_t ApuRead(uint32_t off);
static void ApuWrite(uint32_t off, uint32_t v);
static std::mutex g_mmioMutex;           // held from the fault until the single-step completes
static thread_local uint32_t t_pageBase; // page being stepped on this thread
static thread_local bool t_stepping, t_isWrite;
static thread_local uint32_t t_addr, t_before;
static std::mutex g_regLock;
static volatile uint32_t g_regsFlat[0x400000];  // flat backing store for the 16 MB NV2A register window (was a mutexed std::map: hot in profiles); word writes/reads are atomic on x86
static int g_logBudget = 4000;
static std::map<uint64_t, int> g_seen;  // (kind, reg, value) triples already logged
static bool FirstTime(int kind, uint32_t reg, uint32_t v) { return g_seen.emplace(((uint64_t)kind << 56) ^ ((uint64_t)reg << 32) ^ v, 1).second; }

void Nv2aKick(); void Nv2aInit(); void Nv2aVblank();
uint32_t MmioGet(uint32_t reg) { return reg < 0x1000000 ? g_regsFlat[reg >> 2] : 0; }
void MmioSet(uint32_t reg, uint32_t v) { if (reg < 0x1000000) g_regsFlat[reg >> 2] = v; }
static uint32_t g_putHist[64][2], g_putPos = 0;   // last guest DMA_PUT writes with the pusher's GET at that moment (debug)
void MmioDumpPutHistory() { for (int i = 0; i < 64; i++) Log("[user]   put write #%u: put=0x%x get-then=0x%x", g_putPos - 64 + i, g_putHist[(g_putPos + i) & 63][0], g_putHist[(g_putPos + i) & 63][1]); }
static uint32_t g_rdHist[64][3], g_rdPos = 0, g_putSeq = 0;   // last guest reads of USER GET / PGRAPH fence, tagged with the PUT-write count at the time
void MmioDumpReadHistory() { for (int i = 0; i < 64; i++) { uint32_t* e = g_rdHist[(g_rdPos + i) & 63]; Log("[user]   read reg 0x%06x -> 0x%08x (after put write #%u)", e[0], e[1], e[2]); } }
static uint32_t g_pcrtcPending = 0;  // PCRTC_INTR_0 (bit 0 = vblank)
static uint32_t PmcIntr0() {
  uint32_t v = 0;
  if (g_pcrtcPending & MmioGet(0x600140)) v |= 1u << 24;  // PCRTC
  if (MmioGet(0x400100) & MmioGet(0x400140)) v |= 1u << 12;  // PGRAPH
  return v;
}
// Lazy CPU/GPU synchronisation: the guest may run ahead of the pusher after a PUT write; whenever it looks at GPU progress (USER GET, the fence register) it first
// waits until the pusher has caught up, so it never sees a stale GET (free ring space) or fence. XBOX_SYNC_GPU=1 restores the old wait-after-every-PUT lock-step.
static void WaitPusherIdle() {
  if (MmioGet(0x3244) == MmioGet(0x3240)) return;
  g_mmioMutex.unlock();
  uint64_t t0 = plat::TickMs(); while (MmioGet(0x3244) != MmioGet(0x3240) && plat::TickMs() - t0 < 500) plat::SleepMs(0);
  g_mmioMutex.lock();
}
static bool LockStep() { static bool v = getenv("XBOX_SYNC_GPU") != nullptr && !getenv("XBOX_ASYNC_GPU"); return v; }
static uint32_t RegRead(uint32_t reg) {
  uint32_t v;
  static const bool lazy = !LockStep() && !getenv("XBOX_ASYNC_GPU"); if (lazy && (reg == 0x400B10 || (reg >= 0x800000 && (reg & 0xFFFF) >= 0x44 && (reg & 0xFFFF) <= 0x48))) WaitPusherIdle();
  switch (reg) {
    case 0x000000: v = 0x02A000A1; break;                       // NV_PMC_BOOT_0 (NV2A)
    case 0x000100: v = PmcIntr0(); break;                       // NV_PMC_INTR_0
    case 0x600100: v = g_pcrtcPending; break;
    case 0x002400: v = 0x10; break;                             // PFIFO RUNOUT_STATUS: empty
    case 0x003214: case 0x003014: v = 0x10; break;              // PFIFO CACHE1/0 STATUS: LOW_MARK (cache empty)                   // NV_PCRTC_INTR_0
    case 0x009400: case 0x009410: v = (uint32_t)(plat::TickMs() * 1000); break;  // PTIMER_TIME_0 (ns-ish counter)
    default: {
      uint32_t r = reg;
      if (reg >= 0x800000 && (reg & 0xFFFF) >= 0x40 && (reg & 0xFFFF) <= 0x48) r = 0x3240 + ((reg & 0xFFFF) - 0x40);  // USER DMA_PUT/GET/REF -> PFIFO CACHE1
      v = MmioGet(r);
      // The guest may fill the ring right up to the GET it reads and write PUT == GET; the pusher would treat that as "empty" and lose the whole pending ring.
      // Report GET one dword behind so a full ring can never look empty (never crossing a page boundary, which could leave the ring).
      // Idle (GET == PUT) must read exactly, since the title polls for that.
      if (r == 0x3244 && v != MmioGet(0x3240) && (v & 0xFFF) >= 4) v -= 4;
    }
  }
  if (reg == 0x800044 || reg == 0x400B10) { g_rdHist[g_rdPos & 63][0] = reg; g_rdHist[g_rdPos & 63][1] = v; g_rdHist[g_rdPos & 63][2] = g_putPos; ++g_rdPos; }
  if (g_logBudget > 0 && FirstTime(0, reg, v)) { --g_logBudget; Log("[nv2a] read  0x%06x -> 0x%08x", reg, v); }
  return v;
}
static void RegWrite(uint32_t reg, uint32_t v) {
  if (reg == 0x600100) { g_pcrtcPending &= ~v; }              // write-1-to-clear
  else if (reg == 0x400100 && (MmioGet(0x400100) & ~v & (1u << 20)) == 0) { MmioSet(reg, MmioGet(reg) & ~v); MmioSet(0x400108, 0); }   // PGRAPH error interrupt acknowledged: clear its source too
  else if (reg == 0x400100 || reg == 0x002100 || reg == 0x009100 || reg == 0x001100 || reg == 0x680100 || reg == 0x101000) MmioSet(reg, MmioGet(reg) & ~v);  // PGRAPH/PFIFO/PTIMER/PBUS/PRAMDAC/PFB interrupt status: write 1 to clear
  else if (reg >= 0x800000 && (reg & 0xFFFF) >= 0x40 && (reg & 0xFFFF) <= 0x48) {
    { static int ul = getenv("XBOX_USERLOG") ? 200 : 0; if (ul > 0) { --ul; Log("[user] write 0x%06x <- 0x%08x (get=0x%x)", reg, v, MmioGet(0x3244)); } }
    if ((reg & 0xFFFF) == 0x40) { uint32_t oldPut = MmioGet(0x3240), curGet = MmioGet(0x3244); static int ov = 0; if (curGet > oldPut && v > oldPut && v >= curGet && ov++ < 3) { Log("[user] GUEST OVERRAN THE PUSHER: put %x -> %x but get=%x (put write #%u)", oldPut, v, curGet, g_putPos); MmioDumpPutHistory(); MmioDumpReadHistory(); } }
    if ((reg & 0xFFFF) == 0x40) { g_putHist[g_putPos & 63][0] = v; g_putHist[g_putPos & 63][1] = MmioGet(0x3244); ++g_putPos; }
    MmioSet(0x3240 + ((reg & 0xFFFF) - 0x40), v); Nv2aKick();  // USER DMA_PUT/GET/REF
    if ((reg & 0xFFFF) == 0x40 && LockStep()) {   // lock-step CPU and GPU: the guest does not run ahead of the pusher (debugging aid for ring overruns)
      g_mmioMutex.unlock();
      uint64_t t0 = plat::TickMs(); while (MmioGet(0x3244) != MmioGet(0x3240) && plat::TickMs() - t0 < 500) plat::SleepMs(0);
      g_mmioMutex.lock();
    } }
  else if (reg == 0x100410) MmioSet(reg, 0);                  // self-clearing command bit (PFB)
  else { if (reg == 0x600800) { static int fl = 40; if (fl > 0 && getenv("XBOX_FLIPLOG")) { --fl; Log("[flip] PCRTC_START <- 0x%08x", v); } } if (reg == 0x3240 || reg == 0x3244) { static int ul = getenv("XBOX_USERLOG") ? 200 : 0; if (ul > 0) { --ul; Log("[user] PFIFO write 0x%06x <- 0x%08x", reg, v); } } MmioSet(reg, v); if (reg == 0x3240) Nv2aKick(); }
  if (g_logBudget > 0 && FirstTime(1, reg, v)) { --g_logBudget; Log("[nv2a] write 0x%06x <- 0x%08x", reg, v); }
}

// Vblank at 60 Hz: set PCRTC pending and run the GPU ISR when the title enabled interrupts.
static uint32_t VblankThread(void*) {
  XThreadInit(0);
  for (;;) {
    plat::SleepMs(16);
    { static uint32_t last = 0; static int tick = 0; if (++tick % 60 == 0 && getenv("XBOX_KICKLOG")) { Log("[perf] PUT writes/s: %u", g_putPos - last); last = g_putPos; } }
    std::lock_guard<std::mutex> l(g_mmioMutex);  // register model is not thread safe: share the fault lock
    Nv2aVblank();
    if (MmioGet(0x600140) & 1) g_pcrtcPending |= 1;
    bool fire = MmioGet(0x000140) != 0 && PmcIntr0() != 0;
    if (fire) { g_mmioMutex.unlock(); KernelDeliverInterrupt(0x33); g_mmioMutex.lock(); }
  }
}


// APU: generic register store for now (reads return what was written, zero otherwise); logged so the DSOUND init sequence can be studied.
static std::map<uint32_t, uint32_t> g_apu;
static uint32_t ApuRead(uint32_t off) {
  auto it = g_apu.find(off); uint32_t v = it == g_apu.end() ? 0 : it->second;
  if (off == 0x20010) v = 0x100;  // front-end FIFO: plenty of free space
  static std::map<uint32_t, int> seen; if (seen[off]++ < 2) Log("[apu] read  0x%05x -> 0x%08x", off, v);
  return v;
}
static void ApuWrite(uint32_t off, uint32_t v) {
  g_apu[off] = v;
  static int log = 300; if (log > 0) { --log; Log("[apu] write 0x%05x <- 0x%08x", off, v); }
}

// AC'97: bus-master registers at 0x100+ (global control/status), codec mixer registers at 0x000-0x0FF. Everything reads back what was written, except readiness bits.
static std::map<uint32_t, uint32_t> g_ac;
static uint32_t AcRead(uint32_t off) {
  auto it = g_ac.find(off); uint32_t v = it == g_ac.end() ? 0 : it->second;
  if (off == 0x130) v = 0x100 | 0x400;       // GLOB_STA: primary codec ready (+ sample-ready bits)
  if (off == 0x026) v = 0xF;                  // codec power-down status: all sections ready
  static std::map<uint32_t, int> seen; if (seen[off]++ < 2) Log("[ac97] read  0x%03x -> 0x%08x", off, v);
  return v;
}
static void AcWrite(uint32_t off, uint32_t v) {
  if (off == 0x108 || off == 0x118 || off == 0x128 || off == 0x168 || off == 0x178) v &= ~0x02000000u;  // bus-master control: reset-registers bit self-clears
  g_ac[off] = v;
  static int log = 100; if (log > 0) { --log; Log("[ac97] write 0x%03x <- 0x%08x", off, v); }
}

#ifndef LIFT_NO_NATIVE
static LONG WINAPI MmioHandler(EXCEPTION_POINTERS* ep) {
  auto* r = ep->ExceptionRecord; auto* c = ep->ContextRecord;
  if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
    uint32_t a = (uint32_t)r->ExceptionInformation[1];
    if (!IsUsb(a) && !IsApu(a) && !IsAc(a) && !IsNic(a) && (a < kBase || a >= kBase + kSize)) return EXCEPTION_CONTINUE_SEARCH;
    if (t_stepping) { Log("[mmio] NESTED fault at 0x%08x while stepping 0x%08x (eip=0x%08lx tid=%lu)", a, t_addr, c->Eip, GetCurrentThreadId()); }
    g_mmioMutex.lock();
    t_pageBase = a & ~0xFFFu; t_addr = a & ~3u; t_isWrite = r->ExceptionInformation[0] == 1;
    DWORD old; bool prot = VirtualProtect((void*)(uintptr_t)t_pageBase, 0x1000, PAGE_READWRITE, &old); for (int retry = 0; !prot && retry < 50; retry++) { Sleep(0); VirtualAlloc((void*)(uintptr_t)t_pageBase, 0x1000, MEM_COMMIT, PAGE_READWRITE); prot = VirtualProtect((void*)(uintptr_t)t_pageBase, 0x1000, PAGE_READWRITE, &old); }
    if (!prot) Log("[mmio] VirtualProtect failed for page 0x%08x (error %lu), access 0x%08x eip=0x%08lx", t_pageBase, GetLastError(), a, c->Eip);
    uint32_t* p = (uint32_t*)(uintptr_t)t_addr;
    if (!t_isWrite) p[0] = IsUsb(t_addr) ? UsbRead(t_addr - kUsbBase) : IsApu(t_addr) ? ApuRead(t_addr - kApuBase) : IsAc(t_addr) ? AcRead(t_addr - kAcBase) : IsNic(t_addr) ? NvnetRead(t_addr - kNicBase) : RegRead(t_addr - kBase);
    t_before = p[0];
    c->EFlags |= 0x100;  // trap flag: stop after this one instruction
    t_stepping = true;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && t_stepping) {
    t_stepping = false;
    uint32_t* p = (uint32_t*)(uintptr_t)t_addr;
    if (t_isWrite) { if (IsUsb(t_addr)) UsbWrite(t_addr - kUsbBase, p[0]); else if (IsApu(t_addr)) ApuWrite(t_addr - kApuBase, p[0]); else if (IsAc(t_addr)) AcWrite(t_addr - kAcBase, p[0]); else if (IsNic(t_addr)) NvnetWrite(t_addr - kNicBase, p[0]); else RegWrite(t_addr - kBase, p[0]); }  // writes of an unchanged value still matter (command kicks)
    DWORD old; VirtualProtect((void*)(uintptr_t)t_pageBase, 0x1000, PAGE_NOACCESS, &old);
    c->EFlags &= ~0x100u;
    g_mmioMutex.unlock();
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif   // portable builds reach the hardware windows through MmioRead/MmioWrite only

// Explicit register access for lifted code (no page-fault trap): the same device models as the fault handler, entered with the same lock held.
// Addresses >= 0xFD000000 are hardware; the PRAMIN window (0xFD700000..) aliases the top 1 MB of RAM and is handled by the caller's memory map.
static inline bool IsPramin(uint32_t a) { return a >= kBase + 0x700000 && a < kBase + 0x800000; }   // RAM alias: plain memory
extern "C" uint32_t MmioRead(uint32_t a, int size) {
  if (IsPramin(a)) return size == 4 ? *(volatile uint32_t*)G(a) : size == 2 ? *(volatile uint16_t*)G(a) : *(volatile uint8_t*)G(a);
  std::lock_guard<std::mutex> l(g_mmioMutex);
  uint32_t aligned = a & ~3u, v = IsUsb(aligned) ? UsbRead(aligned - kUsbBase) : IsApu(aligned) ? ApuRead(aligned - kApuBase) : IsAc(aligned) ? AcRead(aligned - kAcBase)
                          : IsNic(aligned) ? NvnetRead(aligned - kNicBase) : (aligned >= kBase && aligned < kBase + kSize) ? RegRead(aligned - kBase) : 0;
  if (!IsUsb(aligned) && !IsApu(aligned) && !IsAc(aligned) && !IsNic(aligned) && !(aligned >= kBase && aligned < kBase + kSize)) { static int n = 0; if (n++ < 20) Log("[mmio] explicit read of unmapped 0x%08x (size %d)", a, size); }
  if (size == 4) return v;
  v >>= (a & 3) * 8; return size == 2 ? (v & 0xFFFF) : (v & 0xFF);
}
extern "C" void MmioWrite(uint32_t a, uint32_t v, int size) {
  if (IsPramin(a)) { if (size == 4) *(volatile uint32_t*)G(a) = v; else if (size == 2) *(volatile uint16_t*)G(a) = (uint16_t)v; else *(volatile uint8_t*)G(a) = (uint8_t)v; return; }
  uint32_t aligned = a & ~3u;
  if (size < 4) {   // sub-dword write: merge into the register's current value (reads of these registers are side-effect free in the models)
    uint32_t cur = MmioRead(aligned, 4), sh = (a & 3) * 8, mask = (size == 2 ? 0xFFFFu : 0xFFu) << sh; v = (cur & ~mask) | ((v << sh) & mask);
  }
  std::lock_guard<std::mutex> l(g_mmioMutex);
  if (!IsUsb(aligned) && !IsApu(aligned) && !IsAc(aligned) && !IsNic(aligned) && !(aligned >= kBase && aligned < kBase + kSize)) { static int n = 0; if (n++ < 20) Log("[mmio] explicit write to unmapped 0x%08x <- 0x%x (size %d)", a, v, size); return; }
  if (IsUsb(aligned)) UsbWrite(aligned - kUsbBase, v); else if (IsApu(aligned)) ApuWrite(aligned - kApuBase, v); else if (IsAc(aligned)) AcWrite(aligned - kAcBase, v);
  else if (IsNic(aligned)) NvnetWrite(aligned - kNicBase, v); else if (aligned >= kBase && aligned < kBase + kSize) RegWrite(aligned - kBase, v);
}

void MmioInit() {
  // Registers 0x000000..0x6FFFFF and 0x800000..0xFFFFFF fault into the model; 0x700000..0x7FFFFF (PRAMIN) aliases the top 1 MB of RAM.
#ifdef GUEST_IDENTITY
  bool ok = VirtualAlloc((void*)(uintptr_t)kBase, 0x700000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS) &&
            VirtualAlloc((void*)(uintptr_t)(kBase + 0x800000), 0x800000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
  if (!ok) { Log("[nv2a] cannot map the register window at 0x%08x", kBase); return; }
#endif
  if (!plat::ShmMap(g_ramMap, 0x03F00000, 0x100000, G(kBase + 0x700000))) Log("[nv2a] PRAMIN alias failed");
  // BAR1 (0xF0000000): the GPU's framebuffer aperture is the same physical RAM (UMA), so alias all of it there.
  if (!plat::ShmMap(g_ramMap, 0, 0x4000000, G(0xF0000000u))) Log("[nv2a] framebuffer aperture alias failed");
#ifdef GUEST_IDENTITY
  VirtualAlloc((void*)(uintptr_t)kUsbBase, kUsbSize, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
#endif
#ifdef GUEST_IDENTITY
  VirtualAlloc((void*)(uintptr_t)kApuBase, kApuSize, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
#endif
#ifdef GUEST_IDENTITY
  VirtualAlloc((void*)(uintptr_t)kAcBase, kAcSize, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
#endif
#ifdef GUEST_IDENTITY
  VirtualAlloc((void*)(uintptr_t)kNicBase, kNicSize, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
#endif
#ifndef LIFT_NO_NATIVE
  AddVectoredExceptionHandler(1, MmioHandler);
#endif
  Nv2aInit();
  plat::ThreadSpawn(VblankThread);
  Log("[nv2a] register window mapped at 0x%08x", kBase);
}
