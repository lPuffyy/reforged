// Windows input backend: XInput pad (loaded dynamically, so the binary runs without it) and the keyboard while the game window has focus.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "input.h"
#include <cstdlib>

bool DisplayHasFocus();   // display.cpp

namespace {
typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, void*);
struct PcPad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct PcState { DWORD packet; PcPad pad; };
}

void InputPollPlatform(HostPad& p) {
  static XInputGetStateFn fn = nullptr; static bool tried = false;
  if (!tried) { tried = true; HMODULE m = LoadLibraryA("xinput1_4.dll"); if (!m) m = LoadLibraryA("xinput9_1_0.dll"); if (m) fn = (XInputGetStateFn)GetProcAddress(m, "XInputGetState"); }
  if (fn) {
    PcState s{}; if (fn(0, &s) == 0) {
      WORD b = s.pad.buttons;
      if (b & 0x0001) p.buttons |= PAD_UP; if (b & 0x0002) p.buttons |= PAD_DOWN; if (b & 0x0004) p.buttons |= PAD_LEFT; if (b & 0x0008) p.buttons |= PAD_RIGHT;
      if (b & 0x0010) p.buttons |= PAD_START; if (b & 0x0020) p.buttons |= PAD_BACK; if (b & 0x0040) p.buttons |= PAD_LTHUMB; if (b & 0x0080) p.buttons |= PAD_RTHUMB;
      if (b & 0x1000) p.analog[PADA_A] = 255; if (b & 0x2000) p.analog[PADA_B] = 255; if (b & 0x4000) p.analog[PADA_X] = 255; if (b & 0x8000) p.analog[PADA_Y] = 255;
      if (b & 0x0100) p.analog[PADA_WHITE] = 255; if (b & 0x0200) p.analog[PADA_BLACK] = 255;  // shoulder buttons -> white / black
      p.analog[PADA_LT] = s.pad.lt; p.analog[PADA_RT] = s.pad.rt; p.lx = s.pad.lx; p.ly = s.pad.ly; p.rx = s.pad.rx; p.ry = s.pad.ry;
    }
  }
  if (DisplayHasFocus() || getenv("XBOX_INPUT_ANY")) {
    auto k = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (k(VK_RETURN)) p.buttons |= PAD_START; if (k(VK_BACK) || k(VK_ESCAPE)) p.buttons |= PAD_BACK;
    if (k(VK_UP)) p.buttons |= PAD_UP; if (k(VK_DOWN)) p.buttons |= PAD_DOWN; if (k(VK_LEFT)) p.buttons |= PAD_LEFT; if (k(VK_RIGHT)) p.buttons |= PAD_RIGHT;
    if (k(VK_SPACE)) p.analog[PADA_A] = 255; if (k('B')) p.analog[PADA_B] = 255; if (k('X')) p.analog[PADA_X] = 255; if (k('Y')) p.analog[PADA_Y] = 255;
    if (k('Q')) p.analog[PADA_BLACK] = 255; if (k('E')) p.analog[PADA_WHITE] = 255; if (k('F')) p.analog[PADA_LT] = 255; if (k('R')) p.analog[PADA_RT] = 255;
    if (k('A')) p.lx = -32000; if (k('D')) p.lx = 32000; if (k('W')) p.ly = 32000; if (k('S')) p.ly = -32000;
    if (k('J')) p.rx = -32000; if (k('L')) p.rx = 32000; if (k('I')) p.ry = 32000; if (k('K')) p.ry = -32000;
  }
}
#endif
