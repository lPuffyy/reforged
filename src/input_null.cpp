// Input backend for platforms without a built-in controller source: the app shell pushes state through InputSetHostPad().
#ifndef _WIN32
#include "input.h"

void InputPollPlatform(HostPad&) {}
#endif
