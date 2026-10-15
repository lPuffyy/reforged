// iOS input backend: a connected game controller (MFi, PlayStation, Xbox, ...) through the GameController framework. The on-screen touch controller
// (ios/TouchControls.mm) pushes its own state with InputSetHostPad(); both are combined by the input layer.
#ifdef __APPLE__
#import <GameController/GameController.h>
#include "input.h"

static int16_t Axis(float v) { return (int16_t)(v * 32767.f); }

void InputPollPlatform(HostPad& p) {
  GCController* pad = nil;
  for (GCController* c in [GCController controllers]) if (c.extendedGamepad) { pad = c; break; }
  if (!pad) return;
  GCExtendedGamepad* g = pad.extendedGamepad;
  if (g.dpad.up.pressed) p.buttons |= PAD_UP;
  if (g.dpad.down.pressed) p.buttons |= PAD_DOWN;
  if (g.dpad.left.pressed) p.buttons |= PAD_LEFT;
  if (g.dpad.right.pressed) p.buttons |= PAD_RIGHT;
  if (g.buttonMenu.pressed) p.buttons |= PAD_START;
  if (g.buttonOptions && g.buttonOptions.pressed) p.buttons |= PAD_BACK;
  if (g.leftThumbstickButton && g.leftThumbstickButton.pressed) p.buttons |= PAD_LTHUMB;
  if (g.rightThumbstickButton && g.rightThumbstickButton.pressed) p.buttons |= PAD_RTHUMB;
  if (g.buttonA.pressed) p.analog[PADA_A] = 255;
  if (g.buttonB.pressed) p.analog[PADA_B] = 255;
  if (g.buttonX.pressed) p.analog[PADA_X] = 255;
  if (g.buttonY.pressed) p.analog[PADA_Y] = 255;
  if (g.leftShoulder.pressed) p.analog[PADA_WHITE] = 255;    // shoulder buttons are the original pad's white / black buttons
  if (g.rightShoulder.pressed) p.analog[PADA_BLACK] = 255;
  uint8_t lt = (uint8_t)(g.leftTrigger.value * 255.f), rt = (uint8_t)(g.rightTrigger.value * 255.f);
  if (lt > p.analog[PADA_LT]) p.analog[PADA_LT] = lt;
  if (rt > p.analog[PADA_RT]) p.analog[PADA_RT] = rt;
  if (g.leftThumbstick.xAxis.value != 0 || g.leftThumbstick.yAxis.value != 0) { p.lx = Axis(g.leftThumbstick.xAxis.value); p.ly = Axis(g.leftThumbstick.yAxis.value); }
  if (g.rightThumbstick.xAxis.value != 0 || g.rightThumbstick.yAxis.value != 0) { p.rx = Axis(g.rightThumbstick.xAxis.value); p.ry = Axis(g.rightThumbstick.yAxis.value); }
}
#endif
