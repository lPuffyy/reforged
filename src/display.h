// Display interface. display.cpp (portable) turns the guest's frame buffer into pixels; a platform backend shows them:
//   display_win.cpp  - a Win32 window (software blit + Vulkan surface)
//   display_null.cpp - headless (no window); an app shell on another platform supplies its own backend
#pragma once
#include <cstdint>

void DisplayInit();                                           // backend: start the window / presentation thread (which must call DisplayTick() ~60 times a second)
void DisplayTick();                                           // portable: present the current frame (calls DisplayBlit unless the Vulkan swapchain owns the window)
void DisplayBlit(const uint32_t* bgra, int width, int height);   // backend: show a 32-bit frame
bool DisplayHasFocus();                                       // backend: the game window has input focus

// Vulkan presentation (platform specific): the loader library, the instance extensions the surface needs, and the surface itself.
const char* DisplayVulkanLibrary();                           // e.g. "vulkan-1.dll", "libvulkan.so.1", "libMoltenVK.dylib"
int DisplayVulkanExtensions(const char** out, int maxCount);  // instance extensions (VK_KHR_surface + the platform's); returns the count
bool DisplayCreateSurface(void* vkInstance, void* vkGetInstanceProcAddr, void* vkSurfaceOut);   // waits for the window; *vkSurfaceOut receives the VkSurfaceKHR
