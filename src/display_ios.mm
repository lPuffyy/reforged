// iOS display backend: the app's view owns a CAMetalLayer and hands it to the runtime (ReforgedSetMetalLayer); the Vulkan renderer draws into it through
// MoltenVK, which the app bundles as a framework. Software frames are not shown (the Vulkan path is the display path on iOS).
#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>
#include <atomic>
#include "display.h"
#include "plat.h"

namespace { std::atomic<void*> g_layer{nullptr}; }

extern "C" void ReforgedSetMetalLayer(void* layer) { g_layer = layer; }

void DisplayInit() {}
void DisplayBlit(const uint32_t*, int, int) {}
bool DisplayHasFocus() { return true; }

const char* DisplayVulkanLibrary() {
  static std::string path;
  if (path.empty()) path = std::string([[[NSBundle mainBundle] privateFrameworksPath] UTF8String]) + "/MoltenVK.framework/MoltenVK";
  return path.c_str();
}

int DisplayVulkanExtensions(const char** out, int maxCount) {
  const char* e[] = {"VK_KHR_surface", "VK_EXT_metal_surface"};
  int n = 0; for (const char* x : e) if (n < maxCount) out[n++] = x;
  return n;
}

bool DisplayCreateSurface(void* inst, void* getProc, void* surfaceOut) {
  void* layer = nullptr; for (int i = 0; i < 200 && !(layer = g_layer.load()); i++) plat::SleepMs(50);
  if (!layer) return false;
  auto gipa = (PFN_vkGetInstanceProcAddr)getProc;
  auto create = (PFN_vkCreateMetalSurfaceEXT)gipa((VkInstance)inst, "vkCreateMetalSurfaceEXT"); if (!create) return false;
  VkMetalSurfaceCreateInfoEXT ci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
  ci.pLayer = (const CAMetalLayer*)layer;
  return create((VkInstance)inst, &ci, nullptr, (VkSurfaceKHR*)surfaceOut) == VK_SUCCESS;
}
#endif
