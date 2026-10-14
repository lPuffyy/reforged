// Controller HLE: the title's XAPI/XPP input functions (found with XbSymbolDatabase, work/title/dsound_hle.txt) are replaced so one virtual Xbox gamepad
// in port 0 always exists. Its state comes from the PC: keyboard (while the game window is focused), a connected XInput pad, and an optional script
// (XBOX_SCRIPT="seconds:button,..." for unattended runs).
#include "kernel.h"
#include "input.h"
#include "plat.h"
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
typedef HostPad Pad;   // XINPUT_GAMEPAD layout
enum { B_UP = PAD_UP, B_DOWN = PAD_DOWN, B_LEFT = PAD_LEFT, B_RIGHT = PAD_RIGHT, B_START = PAD_START, B_BACK = PAD_BACK, B_LTHUMB = PAD_LTHUMB, B_RTHUMB = PAD_RTHUMB };
enum { A_A = PADA_A, A_B = PADA_B, A_X = PADA_X, A_Y = PADA_Y, A_BLACK = PADA_BLACK, A_WHITE = PADA_WHITE, A_LT = PADA_LT, A_RT = PADA_RT };
std::mutex g_hostPadMx; Pad g_hostPad;   // state pushed by the app shell

struct ScriptEntry { double at; std::string button; };
std::vector<ScriptEntry> g_script; bool g_scriptLoaded = false; uint32_t g_t0 = 0;

void LoadScript() {
  if (g_scriptLoaded) return; g_scriptLoaded = true; g_t0 = (uint32_t)plat::TickMs();
  const char* env = getenv("XBOX_SCRIPT"); if (!env) return;
  char buf[512]; strncpy(buf, env, sizeof buf - 1); buf[sizeof buf - 1] = 0;
  for (char* tok = strtok(buf, ","); tok; tok = strtok(nullptr, ",")) { double t; char name[32]; if (sscanf(tok, "%lf:%31s", &t, name) == 2) g_script.push_back({t, name}); }
}

void Press(Pad& p, const std::string& b) {
  if (b == "start") p.buttons |= B_START; else if (b == "back") p.buttons |= B_BACK;
  else if (b == "up") p.buttons |= B_UP; else if (b == "down") p.buttons |= B_DOWN; else if (b == "left") p.buttons |= B_LEFT; else if (b == "right") p.buttons |= B_RIGHT;
  else if (b == "a") p.analog[A_A] = 255; else if (b == "b") p.analog[A_B] = 255; else if (b == "x") p.analog[A_X] = 255; else if (b == "y") p.analog[A_Y] = 255;
  else if (b == "black") p.analog[A_BLACK] = 255; else if (b == "white") p.analog[A_WHITE] = 255; else if (b == "lt") p.analog[A_LT] = 255; else if (b == "rt") p.analog[A_RT] = 255;
}

// Remote control (XBOX_REMOTE=1): work/pad_cmd.txt holds "button seconds" lines (e.g. "a 0.3", "down 0.2", "lx 1.0 0.5" = stick x, value, seconds); the host consumes
// (deletes) the file and holds those buttons for the given time. Lets an operator drive the game one step at a time while looking at screenshots.
struct Held { std::string b; double until; float v; }; std::vector<Held> g_held;
void PollRemote(double now) {
  static bool on = getenv("XBOX_REMOTE") != nullptr; static double lastPoll = 0; if (!on || now - lastPoll < 0.05) return; lastPoll = now;
  static const char* cmdFile = getenv("XBOX_PAD_CMD") ? getenv("XBOX_PAD_CMD") : "work/pad_cmd.txt"; FILE* f = fopen(cmdFile, "r"); if (!f) return; char line[128]; std::vector<std::string> lines; while (fgets(line, sizeof line, f)) lines.push_back(line); fclose(f); remove(cmdFile);
  for (auto& l : lines) { char name[32]; double a = 0, b = 0; int n = sscanf(l.c_str(), "%31s %lf %lf", name, &a, &b); if (n >= 2) { Held h; h.b = name; h.v = n >= 3 ? (float)a : 1.f; h.until = now + (n >= 3 ? b : a); g_held.push_back(h); Log("[remote] %s for %.2fs", name, n >= 3 ? b : a); } }
}
void GetPad(Pad* out) {
  LoadScript(); Pad p; memset(&p, 0, sizeof p);
  { std::lock_guard<std::mutex> l(g_hostPadMx); p = g_hostPad; }   // pushed by an app shell
  InputPollPlatform(p);                                           // keyboard / gamepad on desktop
  double now = ((uint32_t)plat::TickMs() - g_t0) / 1000.0;
  static long calls = 0; ++calls; if (calls % 200 == 0) Log("[xinput] %ld GetState calls (t=%.1fs)", calls, ((uint32_t)plat::TickMs() - g_t0) / 1000.0);
  for (auto& e : g_script) if (now >= e.at && now < e.at + 1.5) { Press(p, e.button); static double lastLog = -1; if (lastLog != e.at) { lastLog = e.at; Log("[xinput] script press '%s' at %.1fs (GetState calls so far: %ld)", e.button.c_str(), now, calls); } }   // each scripted press is held for 1.5 s (the software renderer polls slowly)
  PollRemote(now);
  for (size_t i = 0; i < g_held.size();) { if (now >= g_held[i].until) { g_held.erase(g_held.begin() + i); continue; } const std::string& b = g_held[i].b; float v = g_held[i].v;
    if (b == "lx") p.lx = (int16_t)(v * 32000); else if (b == "ly") p.ly = (int16_t)(v * 32000); else if (b == "rx") p.rx = (int16_t)(v * 32000); else if (b == "ry") p.ry = (int16_t)(v * 32000); else Press(p, b); ++i; }
  *out = p;
}
}  // namespace

void InputSetHostPad(const HostPad& pad) { std::lock_guard<std::mutex> l(g_hostPadMx); g_hostPad = pad; }

// XID (Xbox USB gamepad) input report for the OHCI model (usb_ohci.cpp): [1]=0x14, [2]=digital buttons, [4..11]=A B X Y Black White LT RT, [12..19]=sticks.
void InputXidReport(uint8_t* r) { Pad p; GetPad(&p); memset(r, 0, 20); r[1] = 0x14; r[2] = (uint8_t)p.buttons; memcpy(r + 4, p.analog, 8); memcpy(r + 12, &p.lx, 2); memcpy(r + 14, &p.ly, 2); memcpy(r + 16, &p.rx, 2); memcpy(r + 18, &p.ry, 2); }

#define H(name, ...) extern "C" uint32_t PLAT_STDCALL Hle_##name(__VA_ARGS__)

// The XDEVICE_TYPE pointer that means "gamepad": per-title <XBOX_TITLE_DIR>/gamepad_type.txt (hex address, the dashboard asks about several types), otherwise the first type pointer a title asks about.
static uint32_t* GamepadType(uint32_t* asked) {
  static uint32_t* t = nullptr; static bool init = false;
  if (!init) { init = true; const char* dir = getenv("XBOX_TITLE_DIR"); if (dir) { char f[300]; sprintf(f, "%s/gamepad_type.txt", dir); if (FILE* fp = fopen(f, "r")) { unsigned v = 0; if (fscanf(fp, "%x", &v) == 1) t = (uint32_t*)(uintptr_t)v; fclose(fp); } } }
  if (!t) t = asked; return t;
}
H(XGetDevices, uint32_t* type) { return type == GamepadType(type) ? 1u : 0u; }   // bitmask of connected devices of this type: gamepad in port 0
H(XInitDevices, uint32_t count, uint32_t types) { return 0; }
// XGetDeviceChanges(type, &insertions, &removals): report the gamepad in port 0 as inserted the first time it is asked
H(XGetDeviceChanges, uint32_t* type, uint32_t* ins, uint32_t* rem) {
  static int log = 6; if (log > 0) { --log; Log("[xinput] XGetDeviceChanges type=%p (first dwords %08x %08x)", type, type ? type[0] : 0, type ? type[1] : 0); }
  static long chg = 0; ++chg; if (chg % 500 == 0) Log("[xinput] %ld XGetDeviceChanges calls (t=%.1fs)", chg, ((uint32_t)plat::TickMs() - g_t0) / 1000.0);
  static bool reported = false; if (ins) *ins = 0; if (rem) *rem = 0;
  if (!type || reported) return 0;
  // the gamepad type structure is the one the title passes first; the memory unit / voice types are asked later with other pointers
  uint32_t* gamepadType = GamepadType(type);
  if (type == gamepadType) { reported = true; if (ins) *ins = 1; return 1; }
  return 0;
}
H(XInputOpen, uint32_t* type, uint32_t port, uint32_t slot, uint32_t params) { static int log = 40; if (log > 0) { --log; Log("[xinput] XInputOpen type=%p port=%u slot=%u from %p", type, port, slot, PLAT_RETADDR()); } return port == 0 ? 0xC0DE0000u : 0; }
H(XInputClose, uint32_t handle) { Log("[xinput] XInputClose(0x%x) from %p", handle, PLAT_RETADDR()); return 0; }
H(XInputGetState, uint32_t handle, uint8_t* state) {
  static uint32_t packet = 0; Pad p; GetPad(&p);
  { static long n = 0; ++n; if (n <= 3 || (p.buttons && n % 50 == 0)) Log("[xinput] GetState #%ld handle=%x buttons=%04x A=%d", n, handle, p.buttons, p.analog[A_A]); }
  *(uint32_t*)state = ++packet; memcpy(state + 4, &p, 18);   // XINPUT_STATE is 22 bytes: packet number + XINPUT_GAMEPAD (18)  // XINPUT_STATE: packet number + XINPUT_GAMEPAD
  return 0;
}
H(XInputGetCapabilities, uint32_t handle, uint8_t* caps) { caps[0] = 1; caps[1] = 0; return 0; }   // SubType = gamepad
H(XInputGetDeviceDescription, uint32_t handle) { return 0; }   // dashboard: description lookup (value unused by our fake device)
H(XInputDashSetFeedback, uint32_t handle, uint32_t a, uint32_t b) { return 0; }   // unnamed xapi helper at 0x15e57e in the dashboard (called with handle, desc, 5)
H(XInputDashPoll, uint32_t handle, uint32_t* out) { if (out) { out[0] = 0; out[1] = 0; } return 0; }   // unnamed xapi helper at 0x15e694 (handle, &out[2])
H(XInputSetState, uint32_t handle, uint32_t feedback) { return 0; }

namespace {
struct Registrar { Registrar() {
#define E(n) HleRegister(#n, (void*)Hle_##n)
  E(XInitDevices); E(XGetDevices); E(XGetDeviceChanges); E(XInputOpen); E(XInputClose); E(XInputGetState); E(XInputGetCapabilities); E(XInputSetState); E(XInputGetDeviceDescription); E(XInputDashSetFeedback); E(XInputDashPoll);
#undef E
} } g_registrar;
}  // namespace
