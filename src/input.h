// Host input: the HLE (input_hle.cpp) presents one virtual gamepad to the guest. Where its state comes from is platform specific:
//  - Windows: input_win.cpp polls XInput and the keyboard.
//  - Everywhere else: the app shell pushes state with InputSetHostPad() (touch overlay, MFi/Bluetooth pad, ...).
#pragma once
#include <cstdint>

struct HostPad {                                // the guest-facing layout (XINPUT_GAMEPAD-like)
  uint16_t buttons;                             // HostPadButton bits
  uint8_t analog[8];                            // HostPadAnalog: 0..255
  int16_t lx, ly, rx, ry;                       // sticks, -32768..32767
};
enum HostPadButton : uint16_t { PAD_UP = 1, PAD_DOWN = 2, PAD_LEFT = 4, PAD_RIGHT = 8, PAD_START = 0x10, PAD_BACK = 0x20, PAD_LTHUMB = 0x40, PAD_RTHUMB = 0x80 };
enum HostPadAnalog { PADA_A, PADA_B, PADA_X, PADA_Y, PADA_BLACK, PADA_WHITE, PADA_LT, PADA_RT };

void InputSetHostPad(const HostPad& pad);       // thread safe; the state stays until replaced
void InputPollPlatform(HostPad& out);           // platform backend: OR the platform's own sources (keyboard, gamepad) into `out`
