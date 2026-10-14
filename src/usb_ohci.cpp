// OHCI USB host controller model (USB0 at 0xFED00000, USB1 at 0xFED08000) with one Xbox gamepad (XID device class 0x58) on USB0's first root-hub port.
// Titles that bring their own USB stack (nxdk homebrew such as Insignia's Setup Assistant) enumerate it like a real controller; the HLE XInput path used by
// retail titles never touches this. Implemented from the public OHCI 1.0a and Xbox XID descriptions: root hub ports, HCCA, control/interrupt endpoints,
// done-queue write-back and the interrupt. Bulk and isochronous lists are ignored (no such device).
#include "kernel.h"
#include "guestmem.h"
#include <windows.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <mutex>
#include <vector>

uint8_t* KernelRamBase();
void InputXidReport(uint8_t* r20);   // input_hle.cpp

namespace {
std::recursive_mutex g_mx;
// Register file of controller 0 (controller 1 only reports an empty root hub).
struct Hc {
  uint32_t control = 0, cmdStatus = 0, intStatus = 0, intEnable = 0, hcca = 0, periodCur = 0, ctrlHead = 0, ctrlCur = 0, bulkHead = 0, bulkCur = 0, done = 0;
  uint32_t fmInterval = 0x27782EDF, fmRemaining = 0, fmNumber = 0, periodicStart = 0, lsThreshold = 0x628, rhDescA = 0x01000204, rhDescB = 0, rhStatus = 0;
  uint32_t port[4] = {};
} g_hc[2];
bool g_started = false;

uint8_t* Mem(uint32_t a) { return a < 0x4000000u ? KernelRamBase() + a : G(a & 0x7FFFFFFFu); }
// Descriptors come from guest memory that the guest may be rewriting; a stale pointer reads as zero (end of chain) instead of faulting.
bool Mapped(const void* p) { MEMORY_BASIC_INFORMATION mi; return VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (uint8_t*)p + 4 <= (uint8_t*)mi.BaseAddress + mi.RegionSize; }
uint32_t R32(uint32_t a) { uint32_t* p = (uint32_t*)Mem(a); return Mapped(p) ? *p : 0; }
void W32(uint32_t a, uint32_t v) { uint32_t* p = (uint32_t*)Mem(a); if (Mapped(p)) *p = v; }

// ---- the gamepad ----
const uint8_t kDevDesc[18] = {0x12, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00, 0x40, 0x5E, 0x04, 0x02, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01};
const uint8_t kCfgDesc[32] = {0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32, 0x09, 0x04, 0x00, 0x00, 0x02, 0x58, 0x42, 0x00, 0x00,
                              0x07, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04, 0x07, 0x05, 0x02, 0x03, 0x20, 0x00, 0x04};
const uint8_t kXidDesc[8] = {0x08, 0x42, 0x00, 0x01, 0x01, 0x02, 0x14, 0x06};
struct Dev { bool present = true; uint8_t addr = 0; uint8_t cfg = 0; std::vector<uint8_t> resp; size_t respPos = 0; bool statusPending = false; uint8_t newAddr = 0; bool setAddr = false;
             uint32_t lastReport[5] = {}; DWORD lastIn = 0; } g_dev;

void Setup(const uint8_t* s) {
  uint8_t type = s[0], req = s[1]; uint16_t value = s[2] | (s[3] << 8), len = s[6] | (s[7] << 8);
  g_dev.resp.clear(); g_dev.respPos = 0; g_dev.setAddr = false;
  if (type == 0x80 && req == 0x06) {            // GET_DESCRIPTOR
    uint8_t dt = value >> 8;
    if (dt == 1) g_dev.resp.assign(kDevDesc, kDevDesc + 18);
    else if (dt == 2) g_dev.resp.assign(kCfgDesc, kCfgDesc + 32);
  } else if (type == 0xC1 && req == 0x06 && (value >> 8) == 0x42) g_dev.resp.assign(kXidDesc, kXidDesc + 8);   // XID descriptor
  else if (type == 0xC1 && req == 0x01) {       // GET_CAPABILITIES
    if ((value >> 8) == 1) { g_dev.resp.assign(20, 0xFF); g_dev.resp[0] = 0; g_dev.resp[1] = 0x14; g_dev.resp[3] = 0; }
    else { g_dev.resp.assign(6, 0xFF); g_dev.resp[0] = 0; g_dev.resp[1] = 6; }
  } else if (type == 0xA1 && req == 0x01) { g_dev.resp.assign(20, 0); g_dev.resp[1] = 0x14; }   // GET_REPORT
  else if (type == 0x00 && req == 0x05) { g_dev.newAddr = (uint8_t)value; g_dev.setAddr = true; }  // SET_ADDRESS
  else if (type == 0x00 && req == 0x09) g_dev.cfg = (uint8_t)value;                                  // SET_CONFIGURATION
  if (g_dev.resp.size() > len) g_dev.resp.resize(len);
}

// Retire a TD: condition code cc, current buffer pointer cbp; link it into the HCCA done queue.
void Retire(uint32_t ed, uint32_t td, uint32_t cc, uint32_t cbp, bool toggle) {
  uint32_t t0 = R32(td); t0 = (t0 & 0x0FFFFFFF) | (cc << 28); W32(td, t0);   // condition code (the data toggle travels in the ED's carry bit)
  W32(td + 4, cbp);
  uint32_t next = R32(td + 8) & ~0xFu;
  uint32_t head = (R32(ed + 8) & 1u) | (toggle ? 2u : 0u);   // keep halted bit; carry = toggle
  W32(ed + 8, next | head);
  // done queue
  uint32_t hcca = g_hc[0].hcca; if (!hcca) return;
  W32(td + 8, g_hc[0].done); g_hc[0].done = td;
  g_hc[0].intStatus |= 2;   // WritebackDoneHead (the guest reads HCCA.DoneHead when it sees this)
  W32(hcca + 0x84, g_hc[0].done | ((g_hc[0].intStatus & ~2u & g_hc[0].intEnable) ? 1u : 0u));
}

void ServiceEd(uint32_t ed, bool periodic) {
  for (int guard = 0; guard < 16; guard++) {
    uint32_t e0 = R32(ed), tail = R32(ed + 4) & ~0xFu, headw = R32(ed + 8), head = headw & ~0xFu;
    if ((e0 & (1u << 14)) || (headw & 1) || head == tail) return;   // skip / halted / empty
    uint32_t fa = e0 & 0x7F, en = (e0 >> 7) & 0xF, dir = (e0 >> 11) & 3; bool toggle = (headw & 2) != 0;
    if (fa != g_dev.addr && !(fa == 0 && !g_dev.setAddr)) { if (!(g_dev.statusPending && fa == 0)) return; }
    uint32_t td = head, t0 = R32(td), cbp = R32(td + 4), be = R32(td + 12), dp = (t0 >> 19) & 3; if (dir != 0) dp = dir == 1 ? 1 : 2; else dp = dp;
    uint32_t len = cbp ? be - cbp + 1 : 0;
    if (en == 0) {   // control endpoint
      if (dp == 0) { uint8_t s[8]; if (cbp && len >= 8) memcpy(s, Mem(cbp), 8); else memset(s, 0, 8); Setup(s); g_dev.statusPending = false; Retire(ed, td, 0, 0, true); }
      else if (dp == 2) {
        if (g_dev.resp.empty() || g_dev.respPos >= g_dev.resp.size()) { g_dev.statusPending = true; if (g_dev.setAddr) { g_dev.addr = g_dev.newAddr; g_dev.setAddr = false; } Retire(ed, td, 0, 0, true); }   // status stage of an OUT request
        else { uint32_t n = (uint32_t)std::min<size_t>(len, g_dev.resp.size() - g_dev.respPos); if (n && cbp) memcpy(Mem(cbp), &g_dev.resp[g_dev.respPos], n); g_dev.respPos += n; Retire(ed, td, 0, (n >= len) ? 0 : cbp + n, true); }
      } else {   // OUT: data stage or status stage of an IN request
        if (g_dev.setAddr) { g_dev.addr = g_dev.newAddr; g_dev.setAddr = false; }
        Retire(ed, td, 0, 0, true);
      }
      continue;
    }
    if (!periodic) return;
    if (en == 1 && dp == 2) {   // interrupt IN: gamepad report
      uint8_t rep[20]; InputXidReport(rep); uint32_t sig[5]; memcpy(sig, rep, 20);
      DWORD now = GetTickCount();
      if (memcmp(sig, g_dev.lastReport, 20) == 0 && now - g_dev.lastIn < 60) return;   // NAK: nothing new
      memcpy(g_dev.lastReport, sig, 20); g_dev.lastIn = now;
      uint32_t n = std::min<uint32_t>(len, 20); if (cbp) memcpy(Mem(cbp), rep, n); Retire(ed, td, 0, (n >= len) ? 0 : cbp + n, !toggle);
    } else if (en == 2 && dp == 1) Retire(ed, td, 0, 0, !toggle);   // interrupt OUT: rumble report, discarded
    else return;
  }
}

void FrameBody() {
  Hc& h = g_hc[0];
  if (((h.control >> 6) & 3) != 2) return;   // not operational
  h.fmNumber = (h.fmNumber + 1) & 0xFFFF;
  if (h.hcca) W32(h.hcca + 0x80, h.fmNumber);
  if ((h.control & (1u << 4)) && h.ctrlHead) { for (uint32_t ed = h.ctrlHead, n = 0; ed && n < 8; ed = R32(ed + 12) & ~0xFu, n++) ServiceEd(ed, false); }
  if ((h.control & (1u << 2)) && h.hcca) { for (uint32_t ed = R32(h.hcca + 4 * (h.fmNumber & 31)) & ~0xFu, n = 0; ed && n < 8; ed = R32(ed + 12) & ~0xFu, n++) ServiceEd(ed, true); }
}
// The guest rewrites ED/TD chains while we walk them; a stale link can point anywhere, so a bad pointer just ends this frame.
void Frame() { __try { FrameBody(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

DWORD WINAPI UsbThread(void*) {
  XThreadInit(0);   // the guest ISR runs on this thread
  for (;;) {
    Sleep(1);
    bool fire = false;
    { std::lock_guard<std::recursive_mutex> l(g_mx); Frame(); Hc& h = g_hc[0]; fire = (h.intEnable & 0x80000000u) && (h.intStatus & h.intEnable & 0x7F); }
    if (fire) KernelDeliverInterrupt(0x31);   // USB0 = IRQ 1
  }
}
void Start() { if (!g_started) { g_started = true; CreateThread(nullptr, 0, UsbThread, nullptr, 0, nullptr); } }

void PortSetConnected(Hc& h) { if (&h == &g_hc[0] && g_dev.present) { h.port[0] |= 1 | (1u << 8) | (1u << 16); h.intStatus |= (1u << 6); } }   // CCS, PPS, CSC; RootHubStatusChange
}  // namespace

uint32_t UsbRead(uint32_t off) {
  std::lock_guard<std::recursive_mutex> l(g_mx); Start();
  int c = (off >> 15) & 1; Hc& h = g_hc[c]; uint32_t reg = off & 0x7FFF;
  switch (reg) {
    case 0x00: return 0x110;
    case 0x04: return h.control;
    case 0x08: return h.cmdStatus;
    case 0x0C: return h.intStatus;
    case 0x10: case 0x14: return h.intEnable;
    case 0x18: return h.hcca;
    case 0x1C: return h.periodCur;
    case 0x20: return h.ctrlHead;
    case 0x24: return h.ctrlCur;
    case 0x28: return h.bulkHead;
    case 0x2C: return h.bulkCur;
    case 0x30: return h.done;
    case 0x34: return h.fmInterval;
    case 0x38: return h.fmRemaining;
    case 0x3C: return h.fmNumber;
    case 0x40: return h.periodicStart;
    case 0x44: return h.lsThreshold;
    case 0x48: return h.rhDescA;
    case 0x4C: return h.rhDescB;
    case 0x50: return h.rhStatus;
    default: if (reg >= 0x54 && reg < 0x64) return h.port[(reg - 0x54) >> 2]; return 0;
  }
}

void UsbWrite(uint32_t off, uint32_t v) {
  std::lock_guard<std::recursive_mutex> l(g_mx); Start();
  int c = (off >> 15) & 1; Hc& h = g_hc[c]; uint32_t reg = off & 0x7FFF;
  switch (reg) {
    case 0x04: { uint32_t old = h.control; h.control = v; if (((v >> 6) & 3) == 2 && ((old >> 6) & 3) != 2) PortSetConnected(h); break; }   // entering operational: the device shows up
    case 0x08: if (v & 1) { uint32_t keep = h.hcca; h = Hc(); (void)keep; h.control = 0; h.cmdStatus = 0; } else h.cmdStatus |= v & ~1u; break;   // HostControllerReset
    case 0x0C: h.intStatus &= ~v; if ((v & 2) && c == 0) { h.done = 0; if (h.hcca) W32(h.hcca + 0x84, 0); } break;
    case 0x10: h.intEnable |= v; break;
    case 0x14: h.intEnable &= ~v; break;
    case 0x18: h.hcca = v; break;
    case 0x1C: h.periodCur = v; break;
    case 0x20: h.ctrlHead = v; break;
    case 0x24: h.ctrlCur = v; break;
    case 0x28: h.bulkHead = v; break;
    case 0x2C: h.bulkCur = v; break;
    case 0x30: break;
    case 0x34: h.fmInterval = v; break;
    case 0x40: h.periodicStart = v; break;
    case 0x44: h.lsThreshold = v; break;
    case 0x48: h.rhDescA = (h.rhDescA & ~0x0000FFFFu) | (v & 0x0000FFFFu) ; h.rhDescA = (h.rhDescA & 0xFFFF0000u) | 0x0204; break;
    case 0x4C: h.rhDescB = v; break;
    case 0x50: break;
    default:
      if (reg >= 0x54 && reg < 0x64) {
        uint32_t& p = h.port[(reg - 0x54) >> 2];
        p &= ~(v & 0x001F0000u);                 // write-1-to-clear the change bits
        if (v & 1) p &= ~2u;                      // ClearPortEnable
        if (v & 2) { if (p & 1) p |= 2u; }        // SetPortEnable
        if (v & 4) p |= 4u;                       // SetPortSuspend
        if (v & 8) p &= ~4u;                      // ClearSuspendStatus (resume)
        if (v & 0x10) { if (p & 1) { p |= 2u | (1u << 20); p &= ~0x10u; } }   // SetPortReset completes at once: enabled, PortResetStatusChange
        if (v & 0x100) p |= 0x100;                // SetPortPower
        if (v & 0x200) p &= ~0x100u;              // ClearPortPower
      }
  }
}
