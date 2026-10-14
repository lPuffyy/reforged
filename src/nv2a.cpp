// NV2A command processing: the PFIFO "pusher" walks the title's pushbuffer, the "puller" routes each method to the object bound to its subchannel.
// Only what is needed so far is executed (object binding, semaphores, notifiers); drawing methods are counted and skipped until PGRAPH is implemented.
// Written from the hardware documentation (xemu's hw/xbox/nv2a and envytools were read as reference).
#include "kernel.h"
#include "guestmem.h"
#include "plat.h"
#include <cstdio>
#include <cstring>
#include <map>

uint32_t MmioGet(uint32_t reg);
void MmioSet(uint32_t reg, uint32_t v);
void MmioDumpPutHistory();
void MmioDumpReadHistory();
uint8_t* KernelRamBase();
bool Pgraph3dMethod(uint32_t method, uint32_t param);
void PgraphDump();
void PgraphFlip();
static inline uint8_t* Pramin() { return G(0xFD000000u + 0x700000); }

namespace {
plat::Obj* g_kick;
plat::Obj* g_vblankEvent = plat::EventNew(true, false);  // pulsed by the vblank thread; FLIP_STALL waits for it
uint32_t g_subchClass[8];            // object class bound to each subchannel (0x97 = 3D, 0x62 surface, ...)
uint32_t g_subchInstance[8];         // RAMIN address of that object
uint32_t g_dmaSemaphore;             // RAMIN address of the semaphore DMA object
uint32_t g_semaphoreOffset;
uint64_t g_methodCount[8];
std::map<uint32_t, int> g_unknownLogged;

constexpr uint32_t PFIFO_RAMHT = 0x2210, PFIFO_PUSH1 = 0x3204, PFIFO_DMA_INSTANCE = 0x322C, PFIFO_DMA_PUT = 0x3240, PFIFO_DMA_GET = 0x3244;

uint32_t ramin32(uint32_t addr) { return *(uint32_t*)(Pramin() + addr); }

// Look a handle up in RAMHT. Returns the RAMIN address of the object (false if not found).
bool RamhtLookup(uint32_t handle, uint32_t* instance, uint32_t* engine) {
  uint32_t reg = MmioGet(PFIFO_RAMHT);
  uint32_t size = 1u << (((reg >> 16) & 3) + 12);
  unsigned bits = 0; while ((1u << bits) < size) ++bits; bits -= 1;
  uint32_t hash = 0, h = handle;
  while (h) { hash ^= h & ((1u << bits) - 1); h >>= bits; }
  hash ^= (MmioGet(PFIFO_PUSH1) & 0x1F) << (bits - 4);
  uint32_t base = ((reg >> 4) & 0x1F) << 12;
  uint32_t eh = ramin32(base + hash * 8), ctx = ramin32(base + hash * 8 + 4);
  if (eh != handle || !(ctx & 0x80000000u)) return false;
  *instance = (ctx & 0xFFFF) << 4; *engine = (ctx >> 16) & 3;
  return true;
}

// DMA object (flags, limit, frame) in RAMIN -> host pointer into the 64 MB RAM, and its size.
uint8_t* DmaMap(uint32_t instance, uint32_t* limit) {
  uint32_t flags = ramin32(instance), lim = ramin32(instance + 4), frame = ramin32(instance + 8);
  uint32_t addr = ((frame & 0xFFFFF000u) | (flags >> 20)) & 0x07FFFFFFu;
  if (limit) *limit = lim;
  return KernelRamBase() + (addr & 0x03FFFFFFu);
}

void PgraphMethod(int subch, uint32_t method, uint32_t param) {
  ++g_methodCount[subch];
  if (g_subchClass[subch] == 0x97) {
    switch (method) {
      case 0x130: PgraphFlip(); plat::EventReset(g_vblankEvent); plat::Wait(g_vblankEvent, 40); return;   // FLIP_STALL: the swap completes at the next vblank
      case 0x12C: return;                                                                      // FLIP_INCREMENT_WRITE
      case 0x100:                                                            // NO_OPERATION; a nonzero parameter is a software method: raise a PGRAPH error/notify interrupt and stall until the ISR acknowledges it
        if (param) {
          { static int n = 0; if (n++ < 5) Log("[pgraph] software method NO_OPERATION param=0x%x: raising PGRAPH interrupt", param); }
          MmioSet(0x400704, (subch << 16) | method); MmioSet(0x400708, param); MmioSet(0x400108, 1);
          MmioSet(0x400100, MmioGet(0x400100) | (1u << 20));
          if (MmioGet(0x000140) != 0 && (MmioGet(0x400100) & MmioGet(0x400140))) KernelDeliverInterrupt(0x33);   // run the GPU ISR now instead of waiting for the next vblank tick
          int i = 0; for (; i < 2000 && (MmioGet(0x400100) & (1u << 20)); i++) plat::SleepMs(1);
          static long swCount = 0; ++swCount; if (i >= 2000) Log("[pgraph] software method #%ld (param 0x%x): ISR never acknowledged", swCount, param); else if (swCount % 200 == 0) Log("[pgraph] %ld software methods handled", swCount);
        }
        return;
      case 0x110: return;                                                    // WAIT_FOR_IDLE
      case 0x1A4: g_dmaSemaphore = param; return;                            // SET_CONTEXT_DMA_SEMAPHORE (already translated to a RAMIN address)
      case 0x1D6C: g_semaphoreOffset = param; return;                        // SET_SEMAPHORE_OFFSET
      case 0x1D70: {                                                          // BACK_END_WRITE_SEMAPHORE_RELEASE
        uint32_t lim; uint8_t* p = DmaMap(g_dmaSemaphore, &lim);
        if (g_semaphoreOffset < lim) *(uint32_t*)(p + g_semaphoreOffset) = param;
        { static int n = 0; if (n < 40 || (getenv("XBOX_SEMLOG") && n < 4000)) Log("[sem] release #%d offset=0x%x value=0x%08x (dma get=0x%x)", n, g_semaphoreOffset, param, MmioGet(PFIFO_DMA_GET)); ++n; }
        return;
      }
      default: if (Pgraph3dMethod(method, param)) return; break;
    }
  }
  if (g_subchClass[subch] == 0x44 && method == 0x310) { MmioSet(0x400B10, param); return; }   // pattern PATT_COLOR0: D3D writes a fence counter here and polls PGRAPH 0x400B10 for it
  uint32_t key = (g_subchClass[subch] << 16) | method;
  if (g_unknownLogged[key]++ == 0) Log("[pgraph] class 0x%x method 0x%04x <- 0x%08x (not implemented)", g_subchClass[subch], method, param);
}

void PullerMethod(int subch, uint32_t method, uint32_t param) {
  if (method == 0) {  // bind object to subchannel
    uint32_t inst, eng;
    if (!RamhtLookup(param, &inst, &eng)) { Log("[pfifo] SET_OBJECT: handle 0x%x not in RAMHT", param); return; }
    bool changed = g_subchInstance[subch] != inst;
    g_subchInstance[subch] = inst; g_subchClass[subch] = ramin32(inst) & 0xFF;
    if (changed) Log("[pfifo] subchannel %d bound to object 0x%x class 0x%x", subch, inst, g_subchClass[subch]);
    return;
  }
  if (method >= 0x180 && method < 0x200) {  // methods that take an object handle
    uint32_t inst, eng;
    if (RamhtLookup(param, &inst, &eng)) param = inst; else { Log("[pfifo] method 0x%x: handle 0x%x not in RAMHT", method, param); param = 0; }
  }
  PgraphMethod(subch, method, param);
}

void RunPusher() {
  uint32_t instance = (MmioGet(PFIFO_DMA_INSTANCE) & 0xFFFF) << 4, limit;
  uint8_t* dma = DmaMap(instance, &limit);
  uint32_t get = MmioGet(PFIFO_DMA_GET), put = MmioGet(PFIFO_DMA_PUT);
  { static int l = 100; if (l-- > 0) Log("[pfifo] run pusher: instance=0x%x flags=0x%08x limit=0x%x frame=0x%08x get=0x%x put=0x%x", instance, ramin32(instance), limit, ramin32(instance + 8), get, put); }
  { static bool d = false; if (!d && put > 0x1000000 && put < limit) { d = true; for (uint32_t base : {put - 0x40, put, 0x101000u}) { char b[400] = ""; for (int i = 0; i < 16; i++) { char t[16]; sprintf(t, " %08x", *(uint32_t*)(dma + base + i * 4)); strcat(b, t); } Log("[pfifo] words at 0x%x:%s", base, b); } } }
  static uint32_t method = 0, count = 0, subch = 0, retAddr = 0; static bool nonInc = false, inSub = false;
  static uint32_t hist[16][2], histPos = 0;   // last command headers, dumped when the decoder loses sync
  static uint32_t runs[32][2], runPos = 0; runs[runPos & 31][0] = get; runs[runPos & 31][1] = put; ++runPos;
  while (true) {
    get = MmioGet(PFIFO_DMA_GET); put = MmioGet(PFIFO_DMA_PUT);  // the title can rewrite GET/PUT at any time (it does when it switches pushbuffers)
    if (get == put) break;
    if (get >= limit + 1) {
      Log("[pfifo] pusher ran off the buffer (get=0x%x limit=0x%x)", get, limit);
      static int dumped2 = 0; if (dumped2++ == 0) { for (int i = 0; i < 32; i++) Log("[pfifo]   run history: get=0x%x put=0x%x", runs[(runPos + i) & 31][0], runs[(runPos + i) & 31][1]); for (int i = 0; i < 16; i++) Log("[pfifo]   header history: get=0x%x word=0x%08x", hist[(histPos + i) & 15][0], hist[(histPos + i) & 15][1]); }
      break;
    }
    uint32_t word = *(uint32_t*)(dma + get); get += 4;
    static int pbLog = getenv("XBOX_PB") ? 400 : 0;
    if (pbLog > 0 && !count) { --pbLog; Log("[pb] get=0x%x put=0x%x word=0x%08x", get - 4, put, word); }
    if (!count) { hist[histPos & 15][0] = get - 4; hist[histPos & 15][1] = word; ++histPos; }
    if (!count && (((word & 0xE0000003u) == 0x20000000u && (word & 0x1FFFFFFFu) < 0x100000) || ((word & 3) == 1 && (word & 0xFFFFFFFCu) < 0x100000))) {
      Log("[pfifo] bogus jump 0x%08x at 0x%x (put=0x%x)", word, get - 4, put); { static int pd3 = 0; if (pd3++ == 0) { MmioDumpPutHistory(); MmioDumpReadHistory(); } }
      static int pd2 = 0; if (pd2++ == 0) { for (int i = 0; i < 16; i++) Log("[pfifo]   header history: get=0x%x word=0x%08x", hist[(histPos + i) & 15][0], hist[(histPos + i) & 15][1]); for (uint32_t a = get - 4 - 96; a < get - 4 + 16; a += 4) Log("[pfifo]   0x%x: %08x", a, *(uint32_t*)(dma + a)); }
      break;
    }
    if (count && method != 0 && !g_subchClass[subch]) {
      Log("[pfifo] method 0x%x on unbound subchannel %u (data 0x%08x) at 0x%x (put=0x%x): decoder desync", method, subch, word, get - 4, put);
      static int pd4 = 0; if (pd4++ == 0) { for (int i = 0; i < 16; i++) Log("[pfifo]   header history: get=0x%x word=0x%08x", hist[(histPos + i) & 15][0], hist[(histPos + i) & 15][1]); MmioDumpPutHistory(); MmioDumpReadHistory(); }
      count = 0; break;
    }
    if (count) {
      PullerMethod((int)subch, method, word);
      if (!nonInc) method += 4;
      --count;
    } else if ((word & 0xE0000003u) == 0x20000000u) get = word & 0x1FFFFFFFu;                     // old jump
    else if ((word & 3) == 1) get = word & 0xFFFFFFFCu;                                          // jump
    else if ((word & 3) == 2) { retAddr = get; inSub = true; get = word & 0xFFFFFFFCu; }         // call
    else if (word == 0x00020000u) { if (inSub) { get = retAddr; inSub = false; } }               // return
    else if ((word & 0xE0030003u) == 0) { method = word & 0x1FFC; subch = (word >> 13) & 7; count = (word >> 18) & 0x7FF; nonInc = false; }
    else if ((word & 0xE0030003u) == 0x40000000u) { method = word & 0x1FFC; subch = (word >> 13) & 7; count = (word >> 18) & 0x7FF; nonInc = true; }
    else {
      Log("[pfifo] reserved pushbuffer command 0x%08x at 0x%x (put=0x%x)", word, get - 4, put);
      { static int pd = 0; if (pd++ == 0) { MmioDumpPutHistory(); MmioDumpReadHistory(); } }
      { static int rr = 0; if (rr++ < 3) { uint32_t w0 = *(uint32_t*)(dma + get - 4); plat::SleepMs(100); Log("[pfifo]   re-read after 100 ms: before=0x%08x after=0x%08x now put=0x%x", w0, *(uint32_t*)(dma + get - 4), MmioGet(PFIFO_DMA_PUT)); } }
      static int dumped = 0; if (dumped++ == 0) { for (int i = 0; i < 32; i++) Log("[pfifo]   run history: get=0x%x put=0x%x", runs[(runPos + i) & 31][0], runs[(runPos + i) & 31][1]); for (int i = 0; i < 16; i++) Log("[pfifo]   header history: get=0x%x word=0x%08x", hist[(histPos + i) & 15][0], hist[(histPos + i) & 15][1]); for (uint32_t a = get - 4 - 64; a < get - 4 + 16; a += 4) Log("[pfifo]   0x%x: %08x", a, *(uint32_t*)(dma + a)); }
      break;
    }
    { static int overLog = 6; if (overLog > 0 && get > put && get - 4 <= put) { --overLog; Log("[pfifo] decoder stepped past PUT: get=0x%x put=0x%x last word=0x%08x count=%u method=0x%x", get, put, word, count, method); } }
    MmioSet(PFIFO_DMA_GET, get);
  }
  MmioSet(PFIFO_DMA_GET, get);
}

uint32_t PusherThread(void*) {
  XThreadInit(0);   // the pusher runs the GPU ISR (guest code) for software methods: it needs a KPCR like any guest thread
  for (;;) { plat::Wait(g_kick, 2); if (MmioGet(PFIFO_DMA_GET) != MmioGet(PFIFO_DMA_PUT)) RunPusher(); }
}
}  // namespace

uint8_t* Nv2aDmaMap(uint32_t instance, uint32_t* limit) { return DmaMap(instance, limit); }
void Nv2aKick() { plat::EventSet(g_kick); }
void Nv2aVblank() { plat::EventPulse(g_vblankEvent); }
void Nv2aInit() { g_kick = plat::EventNew(false, false); plat::ThreadSpawn(PusherThread); }
static void DumpWords(uint32_t phys, int n) { char b[600] = ""; for (int i = 0; i < n; i++) { char t[16]; sprintf(t, " %08x", *(uint32_t*)(KernelRamBase() + phys + i * 4)); strcat(b, t); } Log("[ram] 0x%07x:%s", phys, b); }
void Nv2aDump() { Log("[pfifo] final get=0x%x put=0x%x pgraph intr=0x%x", MmioGet(PFIFO_DMA_GET), MmioGet(PFIFO_DMA_PUT), MmioGet(0x400100)); PgraphDump(); DumpWords(0, 8); DumpWords(0x101000, 12); DumpWords(0x35ed000 - 0x40, 16); DumpWords(0x35ed000, 16); for (int i = 0; i < 8; i++) if (g_methodCount[i]) Log("[pgraph] subchannel %d class 0x%x: %llu methods", i, g_subchClass[i], (unsigned long long)g_methodCount[i]); }
