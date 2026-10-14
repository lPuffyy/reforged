// Audio: a software mixer behind the DirectSound HLE (see dsound_hle.cpp for how the title's DSOUND entry points are redirected to the Hle_* functions here).
// Voices come from IDirectSoundBuffer (static buffers, optionally looping) and IDirectSoundStream (queues of XMEDIAPACKETs). Formats: PCM 8/16 bit, Xbox ADPCM
// (IMA with 64 samples per 36-byte channel block). Everything is resampled to 48 kHz stereo 16-bit and played with waveOut. The MCPX DSP (effects, 3D, mix bins)
// is not emulated: volume and pitch are honoured, positioning and mix bins are ignored (mono sources are centred).
#include "kernel.h"
#include "guestmem.h"
#include "plat.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

void LiftAddOverride(uint32_t addr, void* fn, int argBytes, uint32_t ptrMask = 0); uint32_t LiftSynthAddr(void* fn, int argBytes, uint32_t ptrMask = 0);
namespace {
constexpr int kOutRate = 48000, kChunk = 960, kNumHdr = 5;   // 20 ms chunks, 100 ms of queued audio

struct Packet { const uint8_t* buf; uint32_t size, used; uint32_t pCompleted, pStatus, evOrCb, ctx; };
struct Voice {
  bool isStream = false, alive = true, playing = false, paused = false, looping = false;
  uint32_t tag = 1, channels = 1, rate = 48000, bits = 16, blockAlign = 2, spb = 1;   // spb: samples per block (ADPCM 64)
  double pitchRate = 48000;                    // source frames consumed per second
  float gain = 1.f;
  // static buffer
  uint8_t* mem = nullptr; bool ownsMem = false; uint32_t size = 0; double pos = 0;   // pos in source frames
  // stream
  std::deque<Packet> q; uint32_t maxPackets = 8, cbFn = 0, cbCtx = 0;
  // ADPCM decode cache: one decoded block per voice
  int32_t cacheBlock = -1; const uint8_t* cacheData = nullptr; int16_t cache[64 * 2];
  const uint8_t* curData() const { return mem; }
};

std::recursive_mutex g_mx; std::vector<Voice*> g_voices; bool g_started = false; int g_logN = 400; bool g_log = getenv("XBOX_DSLOG") != nullptr;
void L(const char* f, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0, uint32_t e = 0) { if (g_log && g_logN > 0) { --g_logN; char t[256]; snprintf(t, sizeof t, f, a, b, c, d, e); Log("[audio] %s", t); } }

const int kStep[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
const int kIdx[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
int Clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

// Decode one 64-sample Xbox ADPCM block (all channels) into v->cache (interleaved)
void DecodeBlock(Voice* v, const uint8_t* base, int block) {
  const uint8_t* b = base + (size_t)block * v->blockAlign; int ch = v->channels; int pred[2], idx[2];
  for (int c = 0; c < ch && c < 2; c++) { pred[c] = (int16_t)(b[c * 4] | b[c * 4 + 1] << 8); idx[c] = b[c * 4 + 2]; if (idx[c] > 88) idx[c] = 88; }
  const uint8_t* d = b + 4 * ch;
  for (int g = 0; g < 8; g++) for (int c = 0; c < ch && c < 2; c++) {
    for (int k = 0; k < 4; k++) { uint8_t by = d[(g * ch + c) * 4 + k];
      for (int h = 0; h < 2; h++) { int nib = h ? by >> 4 : by & 15, step = kStep[idx[c]], diff = step >> 3;
        if (nib & 1) diff += step >> 2; if (nib & 2) diff += step >> 1; if (nib & 4) diff += step; if (nib & 8) diff = -diff;
        pred[c] = Clamp16(pred[c] + diff); idx[c] += kIdx[nib & 7]; if (idx[c] < 0) idx[c] = 0; if (idx[c] > 88) idx[c] = 88;
        v->cache[(g * 8 + k * 2 + h) * ch + c] = (int16_t)pred[c]; } } }
  v->cacheBlock = block; v->cacheData = base;
}
// Read source frame f of a buffer (or packet) into l/r in [-1,1]; false when past the end
bool Frame(Voice* v, const uint8_t* data, uint32_t bytes, int64_t f, float* l, float* r) {
  if (f < 0) return false; int ch = v->channels; float s[2] = {0, 0};
  if (v->tag == 0x69) {   // Xbox ADPCM: block structure addressed relative to `data`
    int64_t blk = f / 64; if ((blk + 1) * (int64_t)v->blockAlign > bytes) return false;
    if (v->cacheBlock != (int32_t)blk || v->cacheData != data) DecodeBlock(v, data, (int)blk);
    int o = (int)(f % 64) * ch; s[0] = v->cache[o] / 32768.f; s[1] = ch > 1 ? v->cache[o + 1] / 32768.f : s[0];
  } else if (v->bits == 16) {
    int64_t o = f * ch * 2; if (o + ch * 2 > bytes) return false; const int16_t* p = (const int16_t*)(data + o); s[0] = p[0] / 32768.f; s[1] = ch > 1 ? p[1] / 32768.f : s[0];
  } else {
    int64_t o = f * ch; if (o + ch > bytes) return false; s[0] = ((int)data[o] - 128) / 128.f; s[1] = ch > 1 ? ((int)data[o + 1] - 128) / 128.f : s[0];
  }
  *l = s[0]; *r = s[1]; return true;
}
uint32_t TotalFrames(Voice* v, uint32_t bytes) { return v->tag == 0x69 ? (bytes / v->blockAlign) * 64 : bytes / (v->channels * (v->bits / 8)); }

extern "C" uint32_t PLAT_STDCALL K_KeSetEvent(uint32_t ev, uint32_t inc, uint32_t wait);
// XMEDIAPACKET word 4 is a union: with a stream completion callback (DSSTREAMDESC.lpfnCallback) it is the per-packet context passed to that callback,
// otherwise it is the completion event (a kernel handle, or a pointer to a KEVENT).
void Complete(Packet& p, uint32_t status, Voice* v) {
  if (p.pCompleted) *(uint32_t*)G(p.pCompleted) = p.size;
  if (p.pStatus) *(uint32_t*)G(p.pStatus) = status;
  if (v->cbFn) GuestCall(v->cbFn, 3, v->cbCtx, p.evOrCb, status);
  else if (p.evOrCb) { if (p.evOrCb >= 0x80000000u) K_KeSetEvent(p.evOrCb, 0, 0); else plat::EventSet((plat::Obj*)HandleGet(p.evOrCb)); }
}

// Mix one voice into acc (interleaved stereo float) for n output frames. Called with g_mx held; may call guest completion callbacks.
void MixVoice(Voice* v, float* acc, int n) {
  if (!v->alive || !v->playing || v->paused) return;
  double step = v->pitchRate / kOutRate;
  for (int i = 0; i < n; i++) {
    float l = 0, r = 0, l2 = 0, r2 = 0; bool ok;
    if (!v->isStream) {
      int64_t f = (int64_t)v->pos; uint32_t tot = TotalFrames(v, v->size);
      if (f >= (int64_t)tot) { if (v->looping && tot) { v->pos = fmod(v->pos, (double)tot); f = (int64_t)v->pos; } else { v->playing = false; break; } }
      ok = Frame(v, v->mem, v->size, f, &l, &r); if (!ok) { v->playing = false; break; }
      Frame(v, v->mem, v->size, f + 1 >= (int64_t)tot ? (v->looping ? 0 : f) : f + 1, &l2, &r2);
      float fr = (float)(v->pos - (double)f); l += (l2 - l) * fr; r += (r2 - r) * fr; v->pos += step;
    } else {
      while (!v->q.empty() && v->q.front().used >= TotalFrames(v, v->q.front().size)) { Packet p = v->q.front(); v->q.pop_front(); Complete(p, 0, v); }
      if (v->q.empty()) break;
      Packet& p = v->q.front(); int64_t f = (int64_t)v->pos; uint32_t tot = TotalFrames(v, p.size);
      if (f >= (int64_t)tot) { v->pos = 0; p.used = tot; continue; }
      ok = Frame(v, p.buf, p.size, f, &l, &r); if (!ok) { p.used = tot; v->pos = 0; continue; }
      Frame(v, p.buf, p.size, f + 1 >= (int64_t)tot ? f : f + 1, &l2, &r2);
      float fr = (float)(v->pos - (double)f); l += (l2 - l) * fr; r += (r2 - r) * fr; v->pos += step; p.used = (uint32_t)v->pos;
      if ((uint32_t)v->pos >= tot) { v->pos -= tot; if (v->pos < 0) v->pos = 0; p.used = tot; }
    }
    acc[i * 2] += l * v->gain; acc[i * 2 + 1] += r * v->gain;
  }
}

// Fills one chunk (kChunk stereo frames) of 16-bit output; called by the platform audio backend on its own thread.
void MixChunk(int16_t* out, void*) {
  thread_local bool guestInit = false; if (!guestInit) { guestInit = true; XThreadInit(0); }   // completion callbacks run guest code
  float acc[kChunk * 2] = {};
  { static bool first = true; if (first) { first = false; Log("[audio] mixer: first chunk"); } std::lock_guard<std::recursive_mutex> l(g_mx); for (Voice* v : g_voices) MixVoice(v, acc, kChunk); }
  { static float peak = 0; static int chunks = 0; static int act = 0; for (int k = 0; k < kChunk * 2; k++) { float a = fabsf(acc[k]); if (a > peak) peak = a; } if (++chunks >= 50) { if (g_log) { { std::lock_guard<std::recursive_mutex> l(g_mx); act = 0; for (Voice* v : g_voices) if (v->alive && v->playing && !v->paused) ++act; } Log("[audio] 1 s: peak %.3f, %d active voices of %d", peak, act, (int)g_voices.size()); } chunks = 0; peak = 0; } }
  for (int k = 0; k < kChunk * 2; k++) { float v = acc[k]; v = v < -1.f ? -1.f : v > 1.f ? 1.f : v; out[k] = (int16_t)(v * 32767.f); }
  { static FILE* wav = nullptr; static bool tried = false; static uint32_t bytes = 0; const size_t chunkBytes = sizeof(int16_t) * kChunk * 2;   // XBOX_AUDIO_WAV=path: also write the mix to a 48 kHz stereo 16-bit WAV (header patched at 1 s intervals)
    if (!tried) { tried = true; if (const char* w = getenv("XBOX_AUDIO_WAV")) { wav = fopen(w, "wb"); if (wav) { uint8_t h[44] = {}; fwrite(h, 1, 44, wav); } } }
    if (wav) { fwrite(out, 1, chunkBytes, wav); bytes += (uint32_t)chunkBytes; if ((bytes / chunkBytes) % 50 == 0) { uint8_t h[44]; memcpy(h, "RIFF", 4); *(uint32_t*)(h + 4) = 36 + bytes; memcpy(h + 8, "WAVEfmt ", 8); *(uint32_t*)(h + 16) = 16; *(uint16_t*)(h + 20) = 1; *(uint16_t*)(h + 22) = 2; *(uint32_t*)(h + 24) = kOutRate; *(uint32_t*)(h + 28) = kOutRate * 4; *(uint16_t*)(h + 32) = 4; *(uint16_t*)(h + 34) = 16; memcpy(h + 36, "data", 4); *(uint32_t*)(h + 40) = bytes; long pos = ftell(wav); fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav); fseek(wav, pos, SEEK_SET); fflush(wav); } } }
}
void Start() { if (!g_started) { g_started = true; if (!plat::AudioOutStart(kOutRate, kChunk, kNumHdr, MixChunk, nullptr)) Log("[audio] no output device: audio disabled"); } }

float Gain(int32_t millibels) { if (millibels <= -10000) return 0.f; if (millibels > 0) millibels = 0; return powf(10.f, millibels / 2000.f); }
double RateFromPitch(int32_t pitch) { return kOutRate * pow(2.0, pitch / 4096.0); }

struct Wfx { uint16_t tag, ch; uint32_t rate, avg; uint16_t align, bits, cb; };
void ReadFormat(Voice* v, uint32_t wfxPtr) {
  if (!wfxPtr) return; const Wfx* w = (const Wfx*)G(wfxPtr); v->tag = w->tag; v->channels = w->ch ? w->ch : 1; v->rate = w->rate ? w->rate : 48000; v->bits = w->bits ? w->bits : 16; v->blockAlign = w->align ? w->align : v->channels * v->bits / 8;
  if (v->tag == 0x69) { v->bits = 4; v->blockAlign = 36 * v->channels; }
  if (v->tag != 1 && v->tag != 0x69) { L("unsupported wave format tag 0x%x (%u ch, %u Hz)", v->tag, v->channels, v->rate); }
  v->pitchRate = v->rate;
}
// The guest object stores a 1-based index into g_voices (never a host pointer: it must fit in 32 bits on any host).
Voice* Get(uint32_t obj) { if (!obj) return nullptr; uint32_t i = *(uint32_t*)G(obj + 0x10); std::lock_guard<std::recursive_mutex> l(g_mx); return i && i <= g_voices.size() ? g_voices[i - 1] : nullptr; }
// Safe vtable for objects whose real vtable we do not have (buffers, the IDirectSound object): COM-style slots that return 0 and pop their stdcall arguments
// (AddRef/Release 4, GetInfo/GetStatus 8, Process 12, Discontinuity/Flush 4, the rest 8), so a title that calls them through the object cannot jump to null.
uint32_t GenericVtable() {
  static uint32_t table = 0; if (table) return table;
  static const uint8_t pops[16] = {4, 4, 8, 8, 12, 4, 4, 8, 8, 8, 8, 8, 8, 8, 8, 8};
  table = GuestAlloc(64); uint32_t* t = (uint32_t*)G(table);
#ifdef GUEST_IDENTITY
  uint8_t* code = (uint8_t*)plat::VmAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);   // native-fallback machine code: xor eax,eax / ret N
  for (int i = 0; i < 16; i++) { uint8_t* c = code + i * 8; c[0] = 0x33; c[1] = 0xC0; c[2] = 0xC2; c[3] = pops[i]; c[4] = 0; t[i] = (uint32_t)(uintptr_t)c; LiftAddOverride(t[i], nullptr, pops[i]); }
#else
  for (int i = 0; i < 16; i++) t[i] = LiftSynthAddr(nullptr, pops[i]);   // synthetic guest address: returns 0 and pops its arguments
#endif
  return table;
}
uint32_t NewObj(uint32_t kind, Voice* v) {
  uint32_t oA = GuestAlloc(0x100); uint8_t* o = G(oA); *(uint32_t*)(o + 0xC) = kind;
  *(uint32_t*)o = kind == 2 ? 0x417170 : GenericVtable();   // streams: CDirectSoundStream vtable in the XBE (its methods are patched to the hooks below)
  if (getenv("XBOX_DSOBJ")) Log("[audio] object 0x%x kind %u", oA, kind);
  std::lock_guard<std::recursive_mutex> l(g_mx); g_voices.push_back(v); *(uint32_t*)(o + 0x10) = (uint32_t)g_voices.size(); Start(); return oA;
}
}  // namespace

#define H(name, ...) extern "C" uint32_t PLAT_STDCALL Hle_##name(__VA_ARGS__)

H(DirectSoundCreate, uint32_t guid, uint32_t* pp, uint32_t outer) { Voice* none = new Voice; none->alive = false; *pp = NewObj(0, none); Start(); L("DirectSoundCreate"); return 0; }
static uint32_t CreateBuffer(uint32_t desc, uint32_t* pp) {
  Voice* v = new Voice; uint32_t flags = 0, bytes = 0, wfx = 0;
  if (desc) { flags = ((uint32_t*)G(desc))[1]; bytes = ((uint32_t*)G(desc))[2]; wfx = ((uint32_t*)G(desc))[3]; }
  ReadFormat(v, wfx);
  if (bytes) { v->mem = (uint8_t*)GuestVirtualAlloc((bytes + 0xFFF) & ~0xFFFu, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); v->ownsMem = true; v->size = bytes; }
  { static int nb = 0; if (nb++ < 2) L("CreateSoundBuffer flags=0x%x bytes=%u tag=0x%x ch=%u rate=%u", flags, bytes, v->tag, v->channels, v->rate); }
  *pp = NewObj(1, v); return 0;
}
H(IDirectSound_CreateSoundBuffer, uint32_t ds, uint32_t desc, uint32_t* pp, uint32_t outer) { return CreateBuffer(desc, pp); }
H(DirectSoundCreateBuffer, uint32_t desc, uint32_t* pp) { return CreateBuffer(desc, pp); }
static uint32_t CreateStream(uint32_t desc, uint32_t* pp) {
  Voice* v = new Voice; v->isStream = true; v->playing = true;
  if (desc) { uint32_t* d = (uint32_t*)G(desc); v->maxPackets = d[1] ? d[1] : 8; ReadFormat(v, d[2]); v->cbFn = d[3]; v->cbCtx = d[4]; static int nc = 0; if (nc++ < 2) L("CreateSoundStream flags=0x%x maxPackets=%u tag=0x%x ch=%u", d[0], v->maxPackets, v->tag, v->channels); if (nc <= 2) L("   rate=%u cb=0x%x", v->rate, v->cbFn); }
  *pp = NewObj(2, v); return 0;
}
H(IDirectSound_CreateSoundStream, uint32_t ds, uint32_t desc, uint32_t* pp, uint32_t outer) { return CreateStream(desc, pp); }
H(DirectSoundCreateStream, uint32_t desc, uint32_t* pp) { return CreateStream(desc, pp); }
H(XAudioDownloadEffectsImage, uint32_t name, uint32_t loc, uint32_t flags, uint32_t* ppDesc) { if (ppDesc) *ppDesc = GuestAlloc(0x400); return 0; }
H(XAudioCalculatePitch, uint32_t freq) { return (uint32_t)(int32_t)lround(4096.0 * log2((double)(freq ? freq : 48000) / 48000.0)); }

H(IDirectSoundBuffer_SetBufferData, uint32_t buf, uint32_t data, uint32_t size) { Voice* v = Get(buf); if (!v) return 0; std::lock_guard<std::recursive_mutex> l(g_mx); v->mem = data ? G(data) : nullptr; v->size = size; v->cacheBlock = -1; v->cacheData = nullptr; v->pos = 0; return 0; }
H(IDirectSoundBuffer_Lock, uint32_t buf, uint32_t off, uint32_t bytes, uint32_t* pp1, uint32_t* pcb1, uint32_t* pp2, uint32_t* pcb2, uint32_t flags) {
  Voice* v = Get(buf); uint32_t data = v && v->mem ? GA(v->mem) : 0, size = v ? v->size : 0;
  if (v) { std::lock_guard<std::recursive_mutex> l(g_mx); v->cacheBlock = -1; }
  // circular buffer: a request that runs past the end is split into the tail (pp1) and a wrapped part at the start (pp2)
  if (size && off > size) off %= size; if (!bytes) bytes = size > off ? size - off : 0;
  uint32_t first = (off + bytes <= size || !size) ? bytes : size - off, second = bytes - first; if (second > size) second = size;
  if (pp1) *pp1 = data + off; if (pcb1) *pcb1 = first;
  if (pp2) *pp2 = second ? data : 0; if (pcb2) *pcb2 = second; return 0;
}
H(IDirectSoundBuffer_Play, uint32_t buf, uint32_t r1, uint32_t r2, uint32_t flags) { Voice* v = Get(buf); if (!v) return 0; std::lock_guard<std::recursive_mutex> l(g_mx); v->playing = true; v->paused = false; v->looping = flags & 1; if (!v->looping && v->pos >= TotalFrames(v, v->size)) v->pos = 0; L("Play flags=0x%x", flags); return 0; }
H(IDirectSoundBuffer_Stop, uint32_t buf) { Voice* v = Get(buf); if (v) { std::lock_guard<std::recursive_mutex> l(g_mx); v->playing = false; } return 0; }
H(IDirectSoundBuffer_StopEx, uint32_t buf, uint32_t tsLo, uint32_t tsHi, uint32_t flags)   /* the timestamp is a 64-bit REFERENCE_TIME: 16 bytes of arguments in total */ { Voice* v = Get(buf); if (v) { std::lock_guard<std::recursive_mutex> l(g_mx); v->playing = false; if (flags & 2) v->looping = false; } return 0; }
H(IDirectSoundBuffer_Pause, uint32_t buf, uint32_t pause) { Voice* v = Get(buf); if (v) { std::lock_guard<std::recursive_mutex> l(g_mx); v->paused = (pause & 1) != 0; } return 0; }
H(IDirectSoundBuffer_SetCurrentPosition, uint32_t buf, uint32_t bytePos) { Voice* v = Get(buf); if (v) { std::lock_guard<std::recursive_mutex> l(g_mx); v->pos = v->tag == 0x69 ? (double)(bytePos / v->blockAlign) * 64 : (double)bytePos / (v->channels * (v->bits / 8)); } return 0; }
H(IDirectSoundBuffer_GetCurrentPosition, uint32_t buf, uint32_t* play, uint32_t* write) {
  Voice* v = Get(buf); uint32_t p = 0; if (v) { std::lock_guard<std::recursive_mutex> l(g_mx); p = v->tag == 0x69 ? (uint32_t)((int64_t)v->pos / 64) * v->blockAlign : (uint32_t)v->pos * v->channels * (v->bits / 8); }
  if (play) *play = p; if (write) *write = p; return 0;
}
H(IDirectSoundBuffer_GetStatus, uint32_t buf, uint32_t* status) { Voice* v = Get(buf); uint32_t s = 0; if (v && v->playing) s |= 1 | (v->looping ? 4 : 0); if (status) *status = s;
  { static const bool lg = getenv("XBOX_DSLOG2") != nullptr; static std::map<uint32_t, uint32_t> last; if (lg) { auto it = last.find(buf); if (it == last.end() || it->second != s) { last[buf] = s; Log("[audio] GetStatus buf=0x%x v=%p -> 0x%x (from %p)", buf, v, s, _ReturnAddress()); } } }
  return 0; }
H(IDirectSoundBuffer_SetVolume, uint32_t buf, int32_t vol) { Voice* v = Get(buf); if (v) v->gain = Gain(vol); return 0; }
H(IDirectSoundBuffer_SetFrequency, uint32_t buf, uint32_t hz) { Voice* v = Get(buf); if (v) v->pitchRate = hz ? hz : v->rate; return 0; }
H(IDirectSoundBuffer_Release, uint32_t buf) { Voice* v = Get(buf); if (v) { std::lock_guard<std::recursive_mutex> l(g_mx); v->alive = false; v->playing = false; for (Packet& p : v->q) {} } return 0; }
H(IDirectSoundStream_SetVolume, uint32_t s, int32_t vol) { Voice* v = Get(s); if (v) v->gain = Gain(vol); return 0; }
H(IDirectSoundStream_SetPitch, uint32_t s, int32_t pitch) { Voice* v = Get(s); if (v) v->pitchRate = RateFromPitch(pitch); return 0; }
H(IDirectSoundStream_Pause, uint32_t s, uint32_t mode) { Voice* v = Get(s); if (v) { std::lock_guard<std::recursive_mutex> l(g_mx); v->paused = mode != 0 && mode != 0x8000; } return 0; }
static uint32_t FlushStream(uint32_t s) { Voice* v = Get(s); if (!v) return 0; std::lock_guard<std::recursive_mutex> l(g_mx); while (!v->q.empty()) { Packet p = v->q.front(); v->q.pop_front(); Complete(p, 0x80004005u, v); } v->pos = 0; return 0; }
H(IDirectSoundStream_FlushEx, uint32_t s, uint32_t tsLo, uint32_t tsHi, uint32_t flags) { return FlushStream(s); }
H(CDirectSoundStream_Flush, uint32_t s) { return FlushStream(s); }
H(CDirectSoundStream_Discontinuity, uint32_t s) { return 0; }
H(CDirectSoundStream_Release, uint32_t s) { Voice* v = Get(s); if (v) { std::lock_guard<std::recursive_mutex> l(g_mx); v->alive = false; v->playing = false; } return 0; }
H(CDirectSoundStream_GetStatus__r2, uint32_t s, uint32_t* status) { Voice* v = Get(s); uint32_t st = 1; if (v) { std::lock_guard<std::recursive_mutex> l(g_mx); if (v->q.size() >= v->maxPackets) st = 0; } if (status) *status = st; return 0; }
H(CDirectSoundStream_GetInfo, uint32_t s, uint32_t* info) { Voice* v = Get(s); if (info) { info[0] = 0; info[1] = 1; info[2] = 0; info[3] = 0; } return 0; }   // XMEDIAINFO: flags, dwInputSize (granularity; 1 = byte-granular, as before), dwOutputSize, dwMaxLookahead
H(CDirectSoundStream_Process, uint32_t s, uint32_t* in, uint32_t* out) {
  Voice* v = Get(s); if (!v || !in) return 0;
  if (in[0] && in[1]) { Packet p{(const uint8_t*)G(in[0]), in[1], 0, in[2], in[3], in[4], in[5]}; if (p.pStatus) *(uint32_t*)G(p.pStatus) = 0x8000000Au; std::lock_guard<std::recursive_mutex> l(g_mx); v->q.push_back(p); static int n = 0; if (n++ < 6) L("stream packet %u bytes (queue %u)", p.size, (uint32_t)v->q.size()); }
  else { Packet p{nullptr, 0, 0, in[2], in[3], in[4], in[5]}; Complete(p, 0, v); }
  return 0;
}

namespace {
struct Registrar { Registrar() {
#define E(n) HleRegister(#n, (void*)Hle_##n)
  E(DirectSoundCreate); E(XAudioDownloadEffectsImage); E(XAudioCalculatePitch); E(IDirectSound_CreateSoundBuffer); E(IDirectSound_CreateSoundStream); E(DirectSoundCreateBuffer); E(DirectSoundCreateStream);
  E(IDirectSoundBuffer_SetBufferData); E(IDirectSoundBuffer_Lock); E(IDirectSoundBuffer_Play); E(IDirectSoundBuffer_Stop); E(IDirectSoundBuffer_StopEx); E(IDirectSoundBuffer_Pause);
  E(IDirectSoundBuffer_SetCurrentPosition); E(IDirectSoundBuffer_GetCurrentPosition); E(IDirectSoundBuffer_GetStatus); E(IDirectSoundBuffer_SetVolume); E(IDirectSoundBuffer_SetFrequency); E(IDirectSoundBuffer_Release);
  E(IDirectSoundStream_SetVolume); E(IDirectSoundStream_SetPitch); E(IDirectSoundStream_Pause); E(IDirectSoundStream_FlushEx);
  E(CDirectSoundStream_Flush); E(CDirectSoundStream_Discontinuity); E(CDirectSoundStream_Release); E(CDirectSoundStream_GetStatus__r2); E(CDirectSoundStream_GetInfo); E(CDirectSoundStream_Process);
#undef E
} } g_registrar2;
}  // namespace
