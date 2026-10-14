// Headless display backend: no window, frames are dropped. Platforms with a screen (iOS, macOS, Linux desktop) replace this with their own backend.
#ifndef _WIN32
#include "display.h"
#include "plat.h"

namespace {
uint32_t TickThread(void*) { for (;;) { DisplayTick(); plat::SleepMs(16); } }
}

void DisplayInit() { plat::ThreadSpawn(TickThread); }
void DisplayBlit(const uint32_t*, int, int) {}
bool DisplayHasFocus() { return true; }
const char* DisplayVulkanLibrary() { return "libvulkan.so.1"; }
int DisplayVulkanExtensions(const char**, int) { return 0; }
bool DisplayCreateSurface(void*, void*, void*) { return false; }
#endif
