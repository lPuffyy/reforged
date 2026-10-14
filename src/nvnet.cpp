// nForce MAC (NVNET) model at 0xFEF00000: register file, PHY/MDIO, TX/RX descriptor rings (physical addresses = offsets into the guest RAM mapping) and the
// level-triggered interrupt (vector 0x34). Frames the guest transmits go to the network backend (net.cpp); frames from the backend are queued here and copied into
// the guest's RX ring by a dedicated thread, which also runs the guest ISR (it must not run inside the MMIO handler: the ISR touches this device through MMIO).
// Register meanings follow the hardware as documented by xemu's nvnet (read as reference); the implementation is our own.
#include "kernel.h"
#include "guestmem.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

uint8_t* KernelRamBase();
void NetBackendTx(const uint8_t* frame, uint32_t len);   // net.cpp: a frame the guest transmitted
void NetBackendLinkUp();                                 // net.cpp: the guest enabled the NIC

namespace {
struct Desc { uint32_t buf; uint16_t length, flags; };
static bool DmaOk(const void* p, size_t n);
constexpr uint16_t TX_LASTPACKET = 1 << 0, TX_VALID = 1 << 15, RX_AVAIL = 1 << 15, RX_DESCVALID = 1 << 0, RX_BIT4 = 1 << 4;
constexpr uint32_t R_IRQ_STATUS = 0x000, R_IRQ_MASK = 0x004, R_TXCTL = 0x084, R_RXCTL = 0x094, R_MACA = 0x0A8, R_MACB = 0x0AC, R_TXRING = 0x100, R_RXRING = 0x104, R_RINGSIZE = 0x108,
                   R_TXCUR = 0x11C, R_RXCUR = 0x120, R_REG5 = 0x130, R_TXNEXT = 0x134, R_RXNEXT = 0x138, R_TXRXCTL = 0x144, R_MIISTATUS = 0x180, R_ADAPTER = 0x188, R_MDIOADDR = 0x190, R_MDIODATA = 0x194;
constexpr uint32_t IRQ_RX = 2, IRQ_TX = 0x10, IRQ_MII = 0x40;

std::recursive_mutex g_mx; uint32_t g_regs[0x100]; uint16_t g_phy[6]; bool g_init = false;
std::vector<uint8_t> g_txAssemble; std::deque<std::vector<uint8_t>> g_rxQueue;
HANDLE g_evt = nullptr; bool g_thread = false; int g_log = 80, g_frameLog = 300; bool g_logFrames = getenv("XBOX_NET_LOG") != nullptr;

uint32_t& Rg(uint32_t off) { return g_regs[off >> 2]; }
void PhyInit() {
  g_phy[0] = 0x1100; g_phy[1] = 0x782D; g_phy[2] = 0x0143; g_phy[3] = 0xBC70; g_phy[4] = 0x01E1; g_phy[5] = 0x45E1;   // BMCR (FD+autoneg), BMSR (link up), ids, ANAR, ANLPAR
}
uint8_t* Dma(uint32_t a) { return a < 0x4000000u ? KernelRamBase() + a : G(a & 0x7FFFFFFFu); }   // >= 64 MB: host pointer (MmGetPhysicalAddress sets bit 31 for non-RAM memory)
Desc* DescAt(uint32_t phys) { return (Desc*)(KernelRamBase() + phys); }
uint32_t TxRingSize() { return (Rg(R_RINGSIZE) & 0xFFFF) + 1; }
uint32_t RxRingSize() { return (Rg(R_RINGSIZE) >> 16) + 1; }
bool LinkUp() { return (g_phy[1] & 4) != 0; }

void Irq() { if (g_evt) SetEvent(g_evt); }
void SetStatus(uint32_t bits) { Rg(R_IRQ_STATUS) |= bits; Irq(); }
void ResetRings() { Rg(R_TXCUR) = Rg(R_TXNEXT) = Rg(R_TXRING); Rg(R_RXCUR) = Rg(R_RXNEXT) = Rg(R_RXRING); }

uint32_t CurTx() { uint32_t base = Rg(R_TXRING), max = base + TxRingSize() * 8, cur = Rg(R_TXNEXT); if (cur < base || cur + 8 > max) cur = base; Rg(R_TXCUR) = cur; return cur; }
uint32_t CurRx() { uint32_t base = Rg(R_RXRING), max = base + RxRingSize() * 8, cur = Rg(R_RXNEXT); if (cur < base || cur + 8 > max) cur = base; Rg(R_RXCUR) = cur; return cur; }
void AdvanceTx() { uint32_t base = Rg(R_TXRING), max = base + TxRingSize() * 8, n = Rg(R_TXCUR) + 8; Rg(R_TXNEXT) = n >= max ? base : n; }
void AdvanceRx() { uint32_t base = Rg(R_RXRING), max = base + RxRingSize() * 8, n = Rg(R_RXCUR) + 8; Rg(R_RXNEXT) = n >= max ? base : n; }

void LogFrame(const char* dir, const uint8_t* f, uint32_t len) {
  if (!g_logFrames || g_frameLog <= 0) return; --g_frameLog;
  Log("[net] %s %u bytes: dst %02x:%02x:%02x:%02x:%02x:%02x src %02x:%02x:%02x:%02x:%02x:%02x type %02x%02x", dir, len, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], len > 13 ? f[12] : 0, len > 13 ? f[13] : 0);
}

// Guest kicked the transmitter: walk the TX ring, assemble descriptors into frames and hand them to the backend
void ServiceTx() {
  { static const bool dbg = getenv("XBOX_NVNET_DBG") != nullptr; if (dbg) { uint32_t c = CurTx(); Desc* d = DescAt(c); Log("[nvnet] kick: txctl=%x txrx=%x link=%d cur=0x%x flags=0x%04x len=%u", Rg(R_TXCTL), Rg(R_TXRXCTL), (int)LinkUp(), c, d->flags, (unsigned)d->length + 1); } }
  if (!(Rg(R_TXCTL) & 1) || (Rg(R_TXRXCTL) & 4) || !LinkUp()) return;
  bool sent = false;
  for (uint32_t i = 0; i < TxRingSize(); i++) {
    uint32_t cur = CurTx(); Desc* d = DescAt(cur); uint32_t len = (uint32_t)d->length + 1;
    if (!(d->flags & TX_VALID)) break;
    { static int dl = 0; if (g_logFrames && dl++ < 12) { const uint8_t* b = Dma(d->buf); static const uint8_t zero20[24] = {}; if (IsBadReadPtr(b, 20)) b = zero20; Log("[nvnet] TX desc @0x%x buf=0x%x len=%u flags=0x%04x data %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x", cur, d->buf, len, d->flags, b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15],b[16],b[17],b[18],b[19]); } }
    if (len <= 1600 && DmaOk(Dma(d->buf), len)) { size_t o = g_txAssemble.size(); g_txAssemble.resize(o + len); memcpy(&g_txAssemble[o], Dma(d->buf), len); }   // buffer addresses >= 64 MB are host pointers (guest ptr == host ptr)
    bool last = d->flags & TX_LASTPACKET;
    if (last) { LogFrame("TX", g_txAssemble.data(), (uint32_t)g_txAssemble.size()); NetBackendTx(g_txAssemble.data(), (uint32_t)g_txAssemble.size()); g_txAssemble.clear(); sent = true; }
    d->flags &= ~(TX_VALID | 0x4F8);   // clear VALID and the error bits
    AdvanceTx();
    if (last) break;
  }
  if (sent) SetStatus(IRQ_TX);
}

// A stale descriptor (the guest freed or reused the ring after stopping the NIC) can hold any address: only touch committed memory.
static bool DmaOk(const void* p, size_t n) { MEMORY_BASIC_INFORMATION mi; return VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (const uint8_t*)p + n <= (const uint8_t*)mi.BaseAddress + mi.RegionSize; }
// Copy queued frames into free RX descriptors
void DeliverRx() {
  while (!g_rxQueue.empty()) {
    if (!(Rg(R_RXCTL) & 1) || (Rg(R_TXRXCTL) & 4) || !LinkUp()) return;
    uint32_t cur = CurRx(); Desc* d = DescAt(cur);
    if (!(d->flags & RX_AVAIL)) return;   // no free buffer: keep the frame queued
    std::vector<uint8_t>& f = g_rxQueue.front();
    if (f.size() < 60) f.resize(60);   // like the wire: minimum frame size, then the 4-byte FCS the hardware leaves in the buffer (drivers subtract it from the length)
    if (!g_rxQueue.front().empty()) f.resize(f.size() + 4);
    uint32_t n = (uint32_t)f.size(); if (n > (uint32_t)d->length + 1u) n = (uint32_t)d->length + 1u;
    if (!DmaOk(Dma(d->buf), n)) { Log("[nvnet] RX descriptor @0x%x has a bad buffer 0x%x: frame dropped", cur, d->buf); g_rxQueue.pop_front(); continue; }
    memcpy(Dma(d->buf), f.data(), n); d->length = (uint16_t)n; d->flags = RX_BIT4 | RX_DESCVALID;
    LogFrame("RX", f.data(), (uint32_t)f.size());
    AdvanceRx(); SetStatus(IRQ_RX); g_rxQueue.pop_front();
  }
}

DWORD WINAPI NicThread(void*) {
  XThreadInit(0);   // the guest ISR runs here
  for (;;) {
    WaitForSingleObject(g_evt, 50);
    { std::lock_guard<std::recursive_mutex> l(g_mx); DeliverRx(); }
    for (int i = 0; i < 64; i++) {
      bool pending; { std::lock_guard<std::recursive_mutex> l(g_mx); pending = (Rg(R_IRQ_STATUS) & Rg(R_IRQ_MASK)) != 0; }
      if (!pending) break;
      KernelDeliverInterrupt(0x34); Sleep(0);
    }
  }
}
void Start() { if (!g_thread) { g_thread = true; g_evt = CreateEventA(nullptr, FALSE, FALSE, nullptr); CreateThread(nullptr, 0, NicThread, nullptr, 0, nullptr); } }
}  // namespace

// Backend -> guest: queue a received Ethernet frame (any thread)
void NvnetReceive(const uint8_t* frame, uint32_t len) {
  if (len < 14 || len > 1600) return;
  { std::lock_guard<std::recursive_mutex> l(g_mx); if (g_rxQueue.size() > 256) g_rxQueue.pop_front(); g_rxQueue.emplace_back(frame, frame + len); }
  if (g_evt) SetEvent(g_evt);
}
uint32_t NvnetMac(uint8_t* mac) { std::lock_guard<std::recursive_mutex> l(g_mx); uint32_t a = Rg(R_MACA), b = Rg(R_MACB); for (int i = 0; i < 4; i++) mac[i] = (uint8_t)(a >> (8 * i)); mac[4] = (uint8_t)b; mac[5] = (uint8_t)(b >> 8); return a | b; }

uint32_t NvnetRead(uint32_t off) {
  std::lock_guard<std::recursive_mutex> l(g_mx); if (!g_init) { g_init = true; PhyInit(); }
  uint32_t v = off < 0x400 ? Rg(off & ~3u) : 0;
  switch (off) {
    case R_TXRXCTL: v |= 0x8; break;                  // TX_RX_CONTROL: DMA idle
    case 0x088: case 0x098: v = 0; break;             // transmitter / receiver status: not busy
    case R_ADAPTER: v |= 0x4; break;                  // ADAPTER_CONTROL: link up
  }
  static int n = 0; if (g_log > 0 && n++ < 40 && getenv("XBOX_NET_REGLOG")) { --g_log; Log("[nvnet] read  0x%03x -> 0x%08x", off, v); }
  return v;
}

void NvnetWrite(uint32_t off, uint32_t v) {
  std::lock_guard<std::recursive_mutex> l(g_mx); if (!g_init) { g_init = true; PhyInit(); }
  Start();
  if (getenv("XBOX_NET_REGLOG") && g_log > 0) { --g_log; Log("[nvnet] write 0x%03x <- 0x%08x", off, v); }
  switch (off) {
    case R_IRQ_STATUS: case R_MIISTATUS: Rg(off) &= ~v; return;
    case R_IRQ_MASK: Rg(off) = v; if (v) Irq(); return;
    case R_TXRXCTL: {
      Rg(off) = (Rg(off) & (v | 0x8)) | (v & ~0x8u); Rg(off) &= ~0u;
      if (v & 1) { ServiceTx(); DeliverRx(); }                         // KICK
      if (v & 0x10) { ResetRings(); g_txAssemble.clear(); }            // RESET
      if (v & 2) { Rg(R_IRQ_STATUS) = 0; }                             // BIT1: forcedeth clears the interrupt status this way
      else if (v == 0) Rg(R_REG5) = 0x80000000u;                       // the driver polls REG5 bit 31 after clearing the control register
      return;
    }
    case R_TXCTL: case R_RXCTL: Rg(off) = v; if ((v & 1) && off == R_RXCTL) { NetBackendLinkUp(); Irq(); } return;
    case R_MDIOADDR: {
      uint32_t reg = v & 0x1F, phy = (v >> 5) & 0x1F; Rg(off) = v;
      if (v & 0x400) { if (phy == 1 && reg < 6) g_phy[reg] = (uint16_t)Rg(R_MDIODATA); } else Rg(R_MDIODATA) = (phy == 1 && reg < 6) ? g_phy[reg] : 0xFFFFFFFFu;
      Rg(off) &= ~0x8000u; return;                                     // INUSE clears at once
    }
  }
  if (off < 0x400) Rg(off) = v;
}
