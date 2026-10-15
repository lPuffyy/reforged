// Vulkan back end for the NV2A 3D pipeline. Draws arrive from pgraph.cpp as fully fetched vertices (the same SoftVertex arrays the software renderer uses);
// the vertex program and the register combiners are translated to SPIR-V (vk_shaders.cpp) and cached; textures are decoded on the CPU to BGRA8 and cached
// by content hash; render targets live in GPU images and are presented at the title's buffer swap.
#define VK_NO_PROTOTYPES
#include "kernel.h"
#include "display.h"
#include "plat.h"
#include "gpu_soft.h"
#include "gpu_vk.h"
#include "vk_shaders.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

// ---- state accessors implemented elsewhere ----
uint32_t Pgraph3dState(uint32_t method);
const uint32_t* Pgraph3dProgram();
const float* Pgraph3dConstants();
bool SoftDecodeTexture(int stage, std::vector<uint32_t>* bgra, int* w, int* h, uint64_t* hash, bool* linear);

#define VK_FUNCS(X) \
  X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
  X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
  X(vkDestroySurfaceKHR) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceFeatures)
#define VK_DEV_FUNCS(X) \
  X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
  X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) \
  X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
  X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) \
  X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
  X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkDestroyPipelineCache) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
  X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkQueueSubmit) X(vkDeviceWaitIdle) \
  X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdClearAttachments) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) \
  X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdBeginRendering) X(vkCmdEndRendering) \
  X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage)

#define DECL(n) static PFN_##n n;
VK_FUNCS(DECL)
VK_DEV_FUNCS(DECL)
#undef DECL
static PFN_vkGetInstanceProcAddr pGetInstanceProcAddr;
static PFN_vkCreateInstance pCreateInstance;

static bool g_trace = getenv("XBOX_VKTRACE") != nullptr;
#define TR(...) do { if (g_trace) Log(__VA_ARGS__); } while (0)
namespace {
uint32_t R(uint32_t m) { return Pgraph3dState(m); }
float Fl(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

#define CHK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { Log("[vk] %s failed: %d (line %d)", #x, (int)r_, __LINE__); } } while (0)

struct Buf { VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; uint8_t* map = nullptr; size_t size = 0; };
struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; int w = 0, h = 0; VkFormat fmt = VK_FORMAT_UNDEFINED; VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; };
struct RenderTarget { uint32_t key = 0; int w = 0, h = 0; Img color, depth; bool depthCleared = false; uint32_t offset = 0; uint64_t lastWrite = 0; uint32_t zeta = 0; uint64_t lastDepthWrite = 0; };
struct Frame {
  VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cb = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE; VkSemaphore imageAvail = VK_NULL_HANDLE, renderDone = VK_NULL_HANDLE;
  Buf ring; size_t ringOff = 0; VkDescriptorPool dpool = VK_NULL_HANDLE; bool recording = false, submitted = false; std::vector<std::function<void()>> deferred;
};

VkInstance g_inst; VkPhysicalDevice g_phys; VkDevice g_dev; VkQueue g_queue; uint32_t g_qfam; VkSurfaceKHR g_surface; VkSwapchainKHR g_swap; VkExtent2D g_swapExtent; VkFormat g_swapFormat;
std::vector<VkImage> g_swapImages; std::vector<VkImageLayout> g_swapLayouts;
VkPhysicalDeviceMemoryProperties g_memProps; VkPhysicalDeviceProperties g_props;
VkDescriptorSetLayout g_dsl; VkPipelineLayout g_playout; VkSampler g_samplers[2][2][2];
int g_drawLogFlip = -100;   // the last flip whose draws were logged (XBOX_FRAMEDUMP=sync dumps the images of those same flips)
uint32_t g_startMs = (uint32_t)plat::TickMs();   // process start, for the elapsed time in log lines
VkPipelineCache g_pcache = VK_NULL_HANDLE; size_t g_pcacheSaved = 0;   // persisted in pipeline_cache.bin (working directory): shaders compile once, not on every launch
Frame g_frames[2]; int g_fi = 0; uint64_t g_frameNo = 0; bool g_ok = false, g_tried = false;
std::vector<RenderTarget*> g_rts; RenderTarget* g_rt = nullptr; bool g_rendering = false;
uint32_t g_ubAlign = 256;

uint32_t FindMem(uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < g_memProps.memoryTypeCount; i++) if ((bits & (1u << i)) && (g_memProps.memoryTypes[i].propertyFlags & want) == want) return i;
  return 0;
}
Buf MakeBuf(size_t size, VkBufferUsageFlags usage, bool hostVisible) {
  Buf b; b.size = size; VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = size; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  CHK(vkCreateBuffer(g_dev, &ci, nullptr, &b.b)); VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, b.b, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = FindMem(mr.memoryTypeBits, hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  CHK(vkAllocateMemory(g_dev, &ai, nullptr, &b.m)); CHK(vkBindBufferMemory(g_dev, b.b, b.m, 0));
  if (hostVisible) CHK(vkMapMemory(g_dev, b.m, 0, VK_WHOLE_SIZE, 0, (void**)&b.map));
  return b;
}
Img MakeImg(int w, int h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, bool cube = false) {
  Img im; im.w = w; im.h = h; im.fmt = fmt; VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {(uint32_t)w, (uint32_t)h, 1};
  if (cube) ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  ci.mipLevels = 1; ci.arrayLayers = cube ? 6 : 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  CHK(vkCreateImage(g_dev, &ci, nullptr, &im.img)); VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, im.img, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = FindMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  CHK(vkAllocateMemory(g_dev, &ai, nullptr, &im.mem)); CHK(vkBindImageMemory(g_dev, im.img, im.mem, 0));
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = im.img; vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt; vi.subresourceRange = {aspect, 0, 1, 0, cube ? 6u : 1u};
  CHK(vkCreateImageView(g_dev, &vi, nullptr, &im.view)); return im;
}
void Defer(std::function<void()> fn) { g_frames[g_fi].deferred.push_back(std::move(fn)); }
void FreeImg(Img im) { Defer([im]() { vkDestroyImageView(g_dev, im.view, nullptr); vkDestroyImage(g_dev, im.img, nullptr); vkFreeMemory(g_dev, im.mem, nullptr); }); }

void Barrier(VkCommandBuffer cb, Img& im, VkImageLayout to, VkImageAspectFlags aspect, VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout = im.layout; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im.img; b.subresourceRange = {aspect, 0, 1, 0, 1}; b.srcAccessMask = srcA; b.dstAccessMask = dstA;
  vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b); im.layout = to;
}

// ---------------- init ----------------
bool InitVulkan() {
  void* lib = plat::LoadLib(DisplayVulkanLibrary()); if (!lib) { Log("[vk] %s not found", DisplayVulkanLibrary()); return false; }
  pGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)plat::LibSym(lib, "vkGetInstanceProcAddr");
  pCreateInstance = (PFN_vkCreateInstance)pGetInstanceProcAddr(nullptr, "vkCreateInstance");
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "xbox-recomp"; app.apiVersion = VK_API_VERSION_1_3;
  const char* iext[8]; int iextCount = DisplayVulkanExtensions(iext, 8);
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app; ici.enabledExtensionCount = (uint32_t)iextCount; ici.ppEnabledExtensionNames = iext;
  if (pCreateInstance(&ici, nullptr, &g_inst) != VK_SUCCESS) { Log("[vk] vkCreateInstance failed"); return false; }
#define LOADI(n) n = (PFN_##n)pGetInstanceProcAddr(g_inst, #n);
  VK_FUNCS(LOADI)
#undef LOADI
  uint32_t n = 0; vkEnumeratePhysicalDevices(g_inst, &n, nullptr); if (!n) { Log("[vk] no physical device"); return false; }
  std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(g_inst, &n, pds.data());
  g_phys = pds[0]; for (auto d : pds) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { g_phys = d; break; } }
  vkGetPhysicalDeviceProperties(g_phys, &g_props); vkGetPhysicalDeviceMemoryProperties(g_phys, &g_memProps); g_ubAlign = (uint32_t)std::max<VkDeviceSize>(16, g_props.limits.minUniformBufferOffsetAlignment);
  Log("[vk] using %s (API %u.%u)", g_props.deviceName, VK_VERSION_MAJOR(g_props.apiVersion), VK_VERSION_MINOR(g_props.apiVersion));
  if (!DisplayCreateSurface((void*)g_inst, (void*)pGetInstanceProcAddr, &g_surface)) { Log("[vk] no window surface"); return false; }
  uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qn, nullptr); std::vector<VkQueueFamilyProperties> qf(qn); vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qn, qf.data());
  g_qfam = ~0u; for (uint32_t i = 0; i < qn; i++) { VkBool32 sup = 0; vkGetPhysicalDeviceSurfaceSupportKHR(g_phys, i, g_surface, &sup); if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && sup) { g_qfam = i; break; } }
  if (g_qfam == ~0u) { Log("[vk] no graphics+present queue"); return false; }
  float prio = 1.f; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = g_qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; f13.dynamicRendering = VK_TRUE;
  VkPhysicalDeviceFeatures feats{}; feats.independentBlend = VK_TRUE;
  std::vector<const char*> dext = {"VK_KHR_swapchain"};
  { uint32_t en = 0; vkEnumerateDeviceExtensionProperties(g_phys, nullptr, &en, nullptr); std::vector<VkExtensionProperties> ex(en); vkEnumerateDeviceExtensionProperties(g_phys, nullptr, &en, ex.data());
    for (auto& e : ex) if (!strcmp(e.extensionName, "VK_KHR_portability_subset")) dext.push_back("VK_KHR_portability_subset"); }   // layered implementations (MoltenVK on Metal) require it to be enabled when offered
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext = &f13; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.enabledExtensionCount = (uint32_t)dext.size(); dci.ppEnabledExtensionNames = dext.data(); dci.pEnabledFeatures = &feats;
  if (vkCreateDevice(g_phys, &dci, nullptr, &g_dev) != VK_SUCCESS) { Log("[vk] vkCreateDevice failed"); return false; }
#define LOADD(n) n = (PFN_##n)vkGetDeviceProcAddr(g_dev, #n);
  VK_DEV_FUNCS(LOADD)
#undef LOADD
  vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
  // descriptor set layout: 0 = VS uniforms (dynamic), 5 = FS uniforms (dynamic), 1..4 = combined image samplers
  std::vector<VkDescriptorSetLayoutBinding> bs;
  { VkDescriptorSetLayoutBinding b{}; b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; bs.push_back(b); }
  { VkDescriptorSetLayoutBinding b{}; b.binding = 5; b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; bs.push_back(b); }
  for (int i = 1; i <= 4; i++) { VkDescriptorSetLayoutBinding b{}; b.binding = i; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; bs.push_back(b); }
  VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dli.bindingCount = (uint32_t)bs.size(); dli.pBindings = bs.data(); CHK(vkCreateDescriptorSetLayout(g_dev, &dli, nullptr, &g_dsl));
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &g_dsl; CHK(vkCreatePipelineLayout(g_dev, &pli, nullptr, &g_playout));
  for (int mag = 0; mag < 2; mag++) for (int au = 0; au < 2; au++) for (int av = 0; av < 2; av++) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter = si.minFilter = mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = au ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT; si.addressModeV = av ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT; si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    CHK(vkCreateSampler(g_dev, &si, nullptr, &g_samplers[mag][au][av]));
  }
  for (auto& f : g_frames) {
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.queueFamilyIndex = g_qfam; CHK(vkCreateCommandPool(g_dev, &cpi, nullptr, &f.pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = f.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1; CHK(vkAllocateCommandBuffers(g_dev, &cai, &f.cb));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; CHK(vkCreateFence(g_dev, &fci, nullptr, &f.fence));
    VkSemaphoreCreateInfo smi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; CHK(vkCreateSemaphore(g_dev, &smi, nullptr, &f.imageAvail)); CHK(vkCreateSemaphore(g_dev, &smi, nullptr, &f.renderDone));
    f.ring = MakeBuf(96u << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16384}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 8192; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps; CHK(vkCreateDescriptorPool(g_dev, &dpi, nullptr, &f.dpool));
  }
  {   // pipeline cache: start from the file left by the last run (the driver ignores data that does not belong to this device or driver version)
    std::vector<uint8_t> init; if (FILE* cf = fopen("pipeline_cache.bin", "rb")) { fseek(cf, 0, SEEK_END); long n = ftell(cf); fseek(cf, 0, SEEK_SET); if (n > 0 && n < (512l << 20)) { init.resize((size_t)n); if (fread(init.data(), 1, init.size(), cf) != init.size()) init.clear(); } fclose(cf); }
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO}; pci.initialDataSize = init.size(); pci.pInitialData = init.empty() ? nullptr : init.data();
    if (vkCreatePipelineCache(g_dev, &pci, nullptr, &g_pcache) != VK_SUCCESS) g_pcache = VK_NULL_HANDLE; else { g_pcacheSaved = init.size(); Log("[vk] pipeline cache: %zu bytes loaded", init.size()); }
  }
  return true;
}

bool EnsureSwapchain() {
  if (g_swap) return true;
  VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surface, &caps);
  uint32_t fn = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &fn, nullptr); std::vector<VkSurfaceFormatKHR> fmts(fn); vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &fn, fmts.data());
  VkSurfaceFormatKHR sf = fmts[0]; for (auto& f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { sf = f; break; }
  g_swapFormat = sf.format; g_swapExtent = caps.currentExtent; if (g_swapExtent.width == 0xFFFFFFFFu) g_swapExtent = {640, 480};
  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; ci.surface = g_surface; ci.minImageCount = std::max(2u, caps.minImageCount); ci.imageFormat = sf.format; ci.imageColorSpace = sf.colorSpace;
  ci.imageExtent = g_swapExtent; ci.imageArrayLayers = 1; ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform; ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; ci.clipped = VK_TRUE;
  if (vkCreateSwapchainKHR(g_dev, &ci, nullptr, &g_swap) != VK_SUCCESS) { Log("[vk] swapchain creation failed"); g_swap = VK_NULL_HANDLE; return false; }
  uint32_t n = 0; vkGetSwapchainImagesKHR(g_dev, g_swap, &n, nullptr); g_swapImages.resize(n); vkGetSwapchainImagesKHR(g_dev, g_swap, &n, g_swapImages.data()); g_swapLayouts.assign(n, VK_IMAGE_LAYOUT_UNDEFINED);
  Log("[vk] swapchain %ux%u, %u images", g_swapExtent.width, g_swapExtent.height, n);
  return true;
}

// ---------------- frame recording ----------------
Frame& F() { return g_frames[g_fi]; }
void BeginFrame() {
  Frame& f = F(); if (f.recording) return;
  vkWaitForFences(g_dev, 1, &f.fence, VK_TRUE, ~0ull);
  for (auto& d : f.deferred) d(); f.deferred.clear();
  vkResetCommandPool(g_dev, f.pool, 0); vkResetDescriptorPool(g_dev, f.dpool, 0); f.ringOff = 0;
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; vkBeginCommandBuffer(f.cb, &bi);
  f.recording = true; g_rendering = false;
}
size_t RingAlloc(size_t bytes, size_t align, uint8_t** out) {
  Frame& f = F(); size_t off = (f.ringOff + align - 1) & ~(align - 1);
  if (off + bytes > f.ring.size) { Log("[vk] upload ring exhausted (%zu + %zu > %zu)", off, bytes, f.ring.size); off = 0; }
  f.ringOff = off + bytes; *out = f.ring.map + off; return off;
}
void EndRendering() { if (g_rendering) { vkCmdEndRendering(F().cb); g_rendering = false; } }

// ---------------- textures ----------------
struct TexEntry { Img img; uint64_t hash = 0; uint64_t lastUsed = 0; bool linear = false; };
std::map<std::pair<uint64_t, uint32_t>, TexEntry> g_texCache;   // (content hash, size) -> image

// Cube map: six BGRA8 faces (level 0), one 6-layer image with a cube view.
bool UploadCubeTexture(int stage, TexEntry** out) {
  int size = 0; uint64_t hash = 0;
  if (!SoftDecodeCube(stage, nullptr, &size, &hash)) return false;
  auto key = std::make_pair(hash, (uint32_t)(size << 16 | 0xCB));
  auto it = g_texCache.find(key);
  if (it != g_texCache.end()) { it->second.lastUsed = g_frameNo; *out = &it->second; return true; }
  std::vector<uint32_t> faces; if (!SoftDecodeCube(stage, &faces, &size, &hash)) return false;
  EndRendering();
  TexEntry e; e.img = MakeImg(size, size, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, true); e.hash = hash; e.lastUsed = g_frameNo; e.linear = false;
  size_t faceBytes = (size_t)size * size * 4; uint8_t* dst; size_t off = RingAlloc(faceBytes * 6, 16, &dst); memcpy(dst, faces.data(), faceBytes * 6);
  VkCommandBuffer cb = F().cb;
  VkImageMemoryBarrier b0{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b0.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b0.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b0.srcQueueFamilyIndex = b0.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b0.image = e.img.img; b0.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6}; b0.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b0);
  VkBufferImageCopy bc[6]; for (int f = 0; f < 6; f++) { bc[f] = {}; bc[f].bufferOffset = off + faceBytes * f; bc[f].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)f, 1}; bc[f].imageExtent = {(uint32_t)size, (uint32_t)size, 1}; }
  vkCmdCopyBufferToImage(cb, F().ring.b, e.img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, bc);
  VkImageMemoryBarrier b1 = b0; b1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b1.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b1); e.img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  auto res = g_texCache.emplace(key, e); *out = &res.first->second;
  return true;
}
bool UploadTexture(int stage, TexEntry** out) {
  std::vector<uint32_t> bgra; int w, h; uint64_t hash; bool linear;
  static std::vector<uint32_t> scratch;
  // Content hashing every draw was ~90% of draw time: re-hash a given texture binding at most once per frame.
  struct HashMemo { uint64_t frame; uint64_t hash; int w, h; bool linear; };
  static std::unordered_map<uint64_t, HashMemo> memo;
  uint64_t dkey; if (!SoftTextureDesc(stage, &dkey, &w, &h, &linear)) return false;
  { auto m = memo.find(dkey); if (m != memo.end() && m->second.frame == g_frameNo) hash = m->second.hash; else { if (!SoftDecodeTexture(stage, nullptr, &w, &h, &hash, &linear)) return false; memo[dkey] = {g_frameNo, hash, w, h, linear}; if (memo.size() > 4096) memo.clear(); } }
  auto key = std::make_pair(hash, (uint32_t)(w << 16 | h));
  auto it = g_texCache.find(key);
  if (it != g_texCache.end()) { it->second.lastUsed = g_frameNo; *out = &it->second; return true; }
  SoftDecodeTexture(stage, &bgra, &w, &h, &hash, &linear);
  { static int nd = 0; static std::set<uint64_t> seenTex; uint64_t sk = ((uint64_t)R(0x1B00 + 64 * stage) << 32) ^ ((uint64_t)R(0x1B04 + 64 * stage) << 8) ^ ((uint64_t)w << 16) ^ (uint64_t)h; if (w == 1024 && h <= 16 && getenv("XBOX_TEXDUMP_STRIPS")) sk ^= hash * 0x9E3779B97F4A7C15ull; if (getenv("XBOX_TEXDUMP") && nd < 150 && seenTex.insert(sk).second) { char path[100]; sprintf(path, "work/tex/t%03d.bmp", nd); plat::MakeDir("work/tex"); DumpBmp(path, bgra.data(), w, h);
      Log("[tex] #%d stage %d offset=0x%x fmt=0x%08x size=%dx%d linear=%d -> %s", nd, stage, R(0x1B00 + 64 * stage), R(0x1B04 + 64 * stage), w, h, (int)linear, path); ++nd; } }
  EndRendering();
  TexEntry e; e.img = MakeImg(w, h, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT); e.hash = hash; e.lastUsed = g_frameNo; e.linear = linear;
  uint8_t* dst; size_t off = RingAlloc((size_t)w * h * 4, 16, &dst); memcpy(dst, bgra.data(), (size_t)w * h * 4);
  VkCommandBuffer cb = F().cb;
  Barrier(cb, e.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy bc{}; bc.bufferOffset = off; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
  vkCmdCopyBufferToImage(cb, F().ring.b, e.img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
  Barrier(cb, e.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  auto res = g_texCache.emplace(key, e); *out = &res.first->second;
  // evict images that have not been used for a while
  if (g_texCache.size() > 600) for (auto i = g_texCache.begin(); i != g_texCache.end();) { if (g_frameNo - i->second.lastUsed > 120) { FreeImg(i->second.img); i = g_texCache.erase(i); } else ++i; }
  return true;
}

// ---------------- render targets ----------------
RenderTarget* GetRT(int w, int h) {
  uint32_t key = (uint32_t)(w << 16 | h) ^ (R(0x210) * 2654435761u);
  for (auto* r : g_rts) if (r->key == key) return r;
  RenderTarget* rt = new RenderTarget; rt->key = key; rt->w = w; rt->h = h; rt->offset = R(0x210);
  rt->color = MakeImg(w, h, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
  rt->depth = MakeImg(w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
  g_rts.push_back(rt); Log("[vk] render target %dx%d (key %08x)", w, h, key); return rt;
}
bool SurfaceDims(int* x, int* y, int* w, int* h) {
  uint32_t ch = R(0x200), cv = R(0x204); *x = ch & 0xFFFF; *y = cv & 0xFFFF; *w = ch >> 16; *h = cv >> 16;
  return *w > 0 && *h > 0 && *w <= 2048 && *h <= 2048;
}
// Render-to-texture: the most recently drawn render target whose colour offset equals a texture stage's offset (the title renders a scene, then samples it for bloom / post passes)
static uint64_t g_drawSerial = 0;
RenderTarget* FindRTByOffset(uint32_t off, RenderTarget* exclude) {
  RenderTarget* best = nullptr; for (auto* r : g_rts) if (r->offset == off && r != exclude && r->lastWrite && (!best || r->lastWrite > best->lastWrite)) best = r; return best;
}
// Depth-as-texture: the title samples its Z24S8 buffer (as a linear A8R8G8B8 texture) in a post pass. Find the render target that most recently wrote depth to that offset.
RenderTarget* FindRTByZeta(uint32_t off) {
  if (!off) return nullptr; RenderTarget* best = nullptr; for (auto* r : g_rts) if (r->zeta == off && r->lastDepthWrite && (!best || r->lastDepthWrite > best->lastDepthWrite)) best = r; return best;
}
// A sampled copy of a render target's depth image (the live depth image is still attached while the post pass renders, so it cannot be sampled directly)
std::map<uint32_t, Img> g_depthSnap;   // key: w<<16|h
Img* SnapshotDepth(RenderTarget* src) {
  VkCommandBuffer cb = F().cb; uint32_t key = (uint32_t)(src->w << 16 | src->h); auto it = g_depthSnap.find(key);
  if (it == g_depthSnap.end()) it = g_depthSnap.emplace(key, MakeImg(src->w, src->h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)).first;
  Img& dst = it->second; EndRendering();
  Barrier(cb, src->depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  Barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkImageCopy ic{}; ic.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}; ic.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}; ic.extent = {(uint32_t)src->w, (uint32_t)src->h, 1};
  vkCmdCopyImage(cb, src->depth.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
  Barrier(cb, dst, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  return &dst;
}
// In-place post pass: a draw that samples the colour surface it renders into. Vulkan forbids that feedback, so sample a copy of the current contents.
std::map<uint32_t, Img> g_colorSnap;   // key: w<<16|h
Img* SnapshotColor(RenderTarget* src) {
  VkCommandBuffer cb = F().cb; uint32_t key = (uint32_t)(src->w << 16 | src->h); auto it = g_colorSnap.find(key);
  if (it == g_colorSnap.end()) it = g_colorSnap.emplace(key, MakeImg(src->w, src->h, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT)).first;
  Img& dst = it->second; EndRendering();
  Barrier(cb, src->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  Barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkImageCopy ic{}; ic.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; ic.extent = {(uint32_t)src->w, (uint32_t)src->h, 1};
  vkCmdCopyImage(cb, src->color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
  Barrier(cb, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  return &dst;
}
// Make the current surface's image ready for rendering (begin dynamic rendering if needed)
void BeginRendering(RenderTarget* rt) {
  if (g_rendering && g_rt == rt) return;
  EndRendering(); VkCommandBuffer cb = F().cb; g_rt = rt;
  bool colorFresh = rt->color.layout == VK_IMAGE_LAYOUT_UNDEFINED, depthFresh = rt->depth.layout == VK_IMAGE_LAYOUT_UNDEFINED;
  if (rt->color.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) Barrier(cb, rt->color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  if (rt->depth.layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) Barrier(cb, rt->depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT, 0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca.imageView = rt->color.view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = colorFresh ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE; ca.clearValue.color = {{0, 0, 0, 1}};
  VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; da.imageView = rt->depth.view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; da.loadOp = depthFresh ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; da.clearValue.depthStencil = {1.f, 0};
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = {{0, 0}, {(uint32_t)rt->w, (uint32_t)rt->h}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca; ri.pDepthAttachment = &da;
  vkCmdBeginRendering(cb, &ri); g_rendering = true;
}

// ---------------- pipelines ----------------
struct PipeKey { uint64_t vs, fs; uint32_t usedMask, blendEnable, sfac, dfac, eq, depthTest, depthFunc, depthMask, cull, front, colorMask; bool operator<(const PipeKey& o) const { return memcmp(this, &o, sizeof *this) < 0; } };
struct Shader { VkShaderModule mod = VK_NULL_HANDLE; uint32_t used = 0; };
std::unordered_map<uint64_t, Shader> g_vsCache, g_fsCache;
std::map<PipeKey, VkPipeline> g_pipes;

uint64_t Fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) { const uint8_t* b = (const uint8_t*)p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; } return h; }
VkShaderModule MakeModule(const std::vector<uint32_t>& spv) { VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize = spv.size() * 4; ci.pCode = spv.data(); VkShaderModule m = VK_NULL_HANDLE; CHK(vkCreateShaderModule(g_dev, &ci, nullptr, &m)); return m; }

VkBlendFactor BF(uint32_t f) {
  switch (f) { case 0: return VK_BLEND_FACTOR_ZERO; case 1: return VK_BLEND_FACTOR_ONE; case 0x300: return VK_BLEND_FACTOR_SRC_COLOR; case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA; case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; case 0x304: return VK_BLEND_FACTOR_DST_ALPHA; case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return VK_BLEND_FACTOR_DST_COLOR; case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA; }
  return VK_BLEND_FACTOR_ONE;
}
VkBlendOp BO(uint32_t e) { switch (e) { case 0x800A: return VK_BLEND_OP_SUBTRACT; case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT; case 0x8007: return VK_BLEND_OP_MIN; case 0x8008: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; } }

VkPipeline GetPipeline(const PipeKey& k, const Shader& vs, const Shader& fs) {
  auto it = g_pipes.find(k); if (it != g_pipes.end()) return it->second;
  VkPipelineShaderStageCreateInfo st[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs.mod; st[0].pName = "main"; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs.mod; st[1].pName = "main";
  std::vector<VkVertexInputAttributeDescription> attrs; uint32_t off = 0;
  for (int i = 0; i < 16; i++) if (k.usedMask & (1u << i)) { VkVertexInputAttributeDescription a{}; a.location = i; a.binding = 0; a.format = VK_FORMAT_R32G32B32A32_SFLOAT; a.offset = off; off += 16; attrs.push_back(a); }
  VkVertexInputBindingDescription bd{0, off ? off : 16, VK_VERTEX_INPUT_RATE_VERTEX};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bd; vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size(); vi.pVertexAttributeDescriptions = attrs.data();
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.f;
  rs.cullMode = !k.cull ? VK_CULL_MODE_NONE : (k.cull == 0x404 ? VK_CULL_MODE_FRONT_BIT : k.cull == 0x405 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_AND_BACK);
  rs.frontFace = k.front == 0x901 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable = k.depthTest; ds.depthWriteEnable = k.depthMask && k.depthTest; ds.depthCompareOp = (VkCompareOp)(k.depthFunc >= 0x200 ? k.depthFunc - 0x200 : 7);
  VkPipelineColorBlendAttachmentState ba{}; ba.blendEnable = k.blendEnable; ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = BF(k.sfac); ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = BF(k.dfac); ba.colorBlendOp = ba.alphaBlendOp = BO(k.eq);
  ba.colorWriteMask = ((k.colorMask & 0x00010000) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((k.colorMask & 0x00000100) ? VK_COLOR_COMPONENT_G_BIT : 0) | ((k.colorMask & 1) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((k.colorMask & 0x01000000) ? VK_COLOR_COMPONENT_A_BIT : 0);
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &ba;
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount = 3; dy.pDynamicStates = dyn;
  VkFormat colorFmt = VK_FORMAT_B8G8R8A8_UNORM;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &colorFmt; ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
  VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pi.pNext = &ri; pi.stageCount = 2; pi.pStages = st; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp; pi.pRasterizationState = &rs;
  pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds; pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = g_playout;
  VkPipeline p = VK_NULL_HANDLE; CHK(vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &pi, nullptr, &p)); g_pipes[k] = p; return p;
}
}  // namespace

// =============== public API ===============
int g_dbgFlips = 0;       // flips so far (debug logging windows)
static int g_lastFrameDraws = 0, g_tailK = -1, g_ovSwitches = 0, g_ovSince = 0, g_ovCap = -1; static void* g_ovLastRt = nullptr;   // overlay-phase accounting (debug): render-target switches this frame, draws since the last switch, XBOX_OVERLAYSWEEP cap
static int g_frameDraws = 0;   // draws issued since the last flip (debug: XBOX_MAXDRAW / XBOX_MINDRAW / XBOX_BIGLOG)
static uint64_t g_tTexUs = 0, g_tDescUs = 0, g_tGeomUs = 0, g_tShaderUs = 0;
static uint64_t g_statDraws = 0, g_statDrawUs = 0, g_statFlipUs = 0, g_statFlips = 0, g_statT0 = 0;
static uint64_t NowUs() { return plat::PerfCounter() * 1000000ull / plat::PerfFreq(); }

bool GpuVkReady() {
  if (g_ok) return true; if (g_tried) return false; g_tried = true;
  if (getenv("XBOX_SOFT")) return false;
  g_ok = InitVulkan(); if (!g_ok) Log("[vk] Vulkan unavailable, using the software renderer");
  return g_ok;
}

bool GpuVkActive() { return g_ok; }

void GpuClear(uint32_t mask, uint32_t colorValue, uint32_t zValue, int x0, int y0, int x1, int y1) {
  if (!GpuVkReady()) return;
  { static std::set<uint32_t> seen; if (seen.insert(mask).second) Log("[vk] clear mask 0x%x first seen (colour 0x%08x z 0x%x rect (%d,%d)-(%d,%d))", mask, colorValue, zValue, x0, y0, x1, y1); }
  int sx, sy, sw, sh; if (!SurfaceDims(&sx, &sy, &sw, &sh)) return;
  BeginFrame(); RenderTarget* rt = GetRT(sx + sw, sy + sh); BeginRendering(rt); rt->lastWrite = ++g_drawSerial;
  { static const char* fl = getenv("XBOX_FRAMELOG2"); if (fl && g_dbgFlips >= atoi(fl) && g_dbgFlips < atoi(fl) + 2) Log("[frame2] flip %d draw#%d CLEAR mask=0x%x colour=0x%08x z=0x%x rect=(%d,%d)-(%d,%d) -> RT off=0x%x %dx%d (surface clip %d,%d %dx%d)", g_dbgFlips, g_frameDraws, mask, colorValue, zValue, x0, y0, x1, y1, rt->offset, rt->w, rt->h, sx, sy, sw, sh); }
  VkClearAttachment ca[2]; int n = 0;
  if (mask & 0xF0) { ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca[n].colorAttachment = 0; ca[n].clearValue.color = {{((colorValue >> 16) & 255) / 255.f, ((colorValue >> 8) & 255) / 255.f, (colorValue & 255) / 255.f, (colorValue >> 24) / 255.f}}; n++; }
  if (mask & 0x3) { uint32_t zfmt = (R(0x208) >> 4) & 0xF; float d = zfmt == 1 ? (zValue & 0xFFFF) / 65535.f : (zValue >> 8) / 16777215.f; ca[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; ca[n].clearValue.depthStencil = {d, 0}; n++; }
  if (!n) return;
  VkClearRect cr{{{x0, y0}, {(uint32_t)std::max(1, x1 - x0 + 1), (uint32_t)std::max(1, y1 - y0 + 1)}}, 0, 1};
  vkCmdClearAttachments(F().cb, n, ca, 1, &cr);
}

// Per-draw fixed-function parameters for the ffp[] uniform array (layout documented in vk_ffp.cpp).
static void FfpFill(float* o, const FfState& st) {
  auto F3 = [&](float* d, uint32_t reg, float w = 0.f) { d[0] = Fl(R(reg)); d[1] = Fl(R(reg + 4)); d[2] = Fl(R(reg + 8)); d[3] = w; };
  F3(o + 0, 0xA10, Fl(R(0x3B4)));                                   // scene ambient, material alpha
  float p0 = Fl(R(0x9E0)), p1 = Fl(R(0x9E4)), p2 = Fl(R(0x9E8)); float sp = (p1 > 0.f && p2 < 1.f) ? p2 - p0 * 2.f : 1.f; if (sp <= 0.f) sp = 0.0001f;
  F3(o + 4, 0x3A8, sp);                                              // material emission, specular power
  o[8] = Fl(R(0xA50)); o[9] = Fl(R(0xA54)); o[10] = Fl(R(0xA58)); o[11] = 1.f;   // eye position
  o[12] = Fl(R(0x9D0)); o[13] = Fl(R(0x9D4)); o[14] = Fl(R(0x9D8)); o[15] = Fl(R(0x9DC));   // fog plane
  for (int i = 0; i < 8; i++) { if (!st.light[i]) continue; uint32_t b = 0x1000 + 0x80 * i; float* l = o + 16 + 20 * i;
    F3(l, b); F3(l + 4, b + 0x0C); F3(l + 8, b + 0x18);
    if (st.light[i] == 1) F3(l + 12, b + 0x34); else F3(l + 12, b + 0x5C, Fl(R(b + 0x24)));
    F3(l + 16, b + 0x68); }
}

static void GpuDrawImpl(uint32_t prim, const SoftVertex* verts, size_t n);
void GpuDraw(uint32_t prim, const SoftVertex* verts, size_t n) { uint64_t t = NowUs(); GpuDrawImpl(prim, verts, n); g_statDrawUs += NowUs() - t; ++g_statDraws; }
static int g_capSweep = -1;   // XBOX_CAPSWEEP=startSeconds: one launch, per-frame draw cap rises by 25 every 1.5 s after startSeconds (log "[sweep] cap N"), for screenshot bisection
   // draws issued since the last flip (debug: XBOX_MAXDRAW / XBOX_MINDRAW bisect which draw produces an artefact)
static void GpuDrawImpl(uint32_t prim, const SoftVertex* verts, size_t n) {
  { static const char* bl = getenv("XBOX_BANDLOG"); static int nlog = 0;   // XBOX_BANDLOG=y0,y1[,afterSeconds]: log draws whose screen bbox overlaps rows y0..y1 (frame after the title appears)
    if (bl && nlog < 60) { int a = 0, b = 0, after = 0; sscanf(bl, "%d,%d,%d", &a, &b, &after); static uint32_t t0 = (uint32_t)plat::TickMs(); if ((int)((uint32_t)plat::TickMs() - t0) > after * 1000) { float bx0, by0, bx1, by1;
      if (SoftScreenBBox(verts, n, &bx0, &by0, &bx1, &by1) && by1 >= a && by0 <= b && bx1 - bx0 > 150 && by1 - by0 < 40) { ++nlog; Log("[band] draw #%d prim=%u n=%zu bbox=(%.0f,%.0f)-(%.0f,%.0f) tex0 off=0x%x fmt=0x%08x tex1 off=0x%x tex2 off=0x%x mode=0x%x blend=%u src=%x dst=%x ps=%08x/%08x/%u", g_frameDraws, prim, n, bx0, by0, bx1, by1, R(0x1B00), R(0x1B04), R(0x1B40), R(0x1B80), R(0x1E70), R(0x304), R(0x344), R(0x348), R(0x288), R(0x28C), R(0x1E60) & 0xFF); } } } }
  { static const char* bg = getenv("XBOX_BIGLOG"); static int nb = 0;   // XBOX_BIGLOG=minDrawIdx,afterSeconds: log large (>= 60% of screen) draws after that draw index within a frame
    if (bg && nb < 90) { int mi = 0, after = 0, mx2 = 1 << 30; sscanf(bg, "%d,%d,%d", &mi, &after, &mx2); static uint32_t t0 = (uint32_t)plat::TickMs(); if (g_frameDraws >= mi && g_frameDraws <= mx2 && (int)((uint32_t)plat::TickMs() - t0) > after * 1000) { float bx0, by0, bx1, by1;
      float bfo[16] = {}, bp[4], bd1[4], bt0[4]; SoftFirstVertexOut(verts, n, bp, bfo, bd1, bt0); memcpy(bfo + 8, bt0, 16);
      if (SoftScreenBBox(verts, n, &bx0, &by0, &bx1, &by1) && (mx2 < (1 << 30) || ((std::min(bx1, 640.f) - std::max(bx0, 0.f)) > 60 && (std::min(by1, 480.f) - std::max(by0, 0.f)) > 40))) { ++nb;
        Log("[big] draw #%d prim=%u n=%zu bbox=(%.0f,%.0f)-(%.0f,%.0f) blend=%u src=%x dst=%x depth=%u alphatest=%u mode=0x%x tex0 off=0x%x fmt=0x%08x ps=%08x/%08x/%u mask=%08x [vtx0 in.a3=(%.2f %.2f %.2f %.2f) out.d0=(%.2f %.2f %.2f %.2f) out.t0=(%.2f %.2f)] icwA=%08x,%08x ocwA=%08x,%08x icwC=%08x,%08x ocwC=%08x,%08x cc=%08x", g_frameDraws, prim, n, bx0, by0, bx1, by1, R(0x304), R(0x344), R(0x348), R(0x30C), R(0x300), R(0x1E70), R(0x1B00), R(0x1B04), R(0x288), R(0x28C), R(0x1E60) & 0xFF, R(0x358), verts[0].a[3][0], verts[0].a[3][1], verts[0].a[3][2], verts[0].a[3][3], bfo[0], bfo[1], bfo[2], bfo[3], bfo[8], bfo[9], R(0x260), R(0x264), R(0xAA0), R(0xAA4), R(0xAC0), R(0xAC4), R(0x1E40), R(0x1E44), R(0x1E60)); } } } }
  { static const char* nt = getenv("XBOX_NOTEXLOG"); static int nn = 0;   // XBOX_NOTEXLOG=afterSeconds: log large untextured draws (fade / overlay quads) with their combiner constants
    if (nt && nn < 60 && R(0x1E70) == 0) { static uint32_t t0 = (uint32_t)plat::TickMs(); if ((int)((uint32_t)plat::TickMs() - t0) > atoi(nt) * 1000) { float bx0, by0, bx1, by1, bp[4], bd0[4], bd1[4], bt0[4]; SoftScreenBBox(verts, n, &bx0, &by0, &bx1, &by1); SoftFirstVertexOut(verts, n, bp, bd0, bd1, bt0);
      if (bx1 - bx0 > 200 && by1 - by0 > 150) { ++nn; Log("[notex] draw #%d prim=%u n=%zu bbox=(%.0f,%.0f)-(%.0f,%.0f) blend=%u src=%x dst=%x depth=%u mask=%08x d0=(%.2f %.2f %.2f %.2f) c0=%08x c1=%08x final=%08x/%08x cc=%08x icwA0=%08x ocwA0=%08x icwC0=%08x ocwC0=%08x", g_frameDraws, prim, n, bx0, by0, bx1, by1, R(0x304), R(0x344), R(0x348), R(0x30C), R(0x358), bd0[0], bd0[1], bd0[2], bd0[3], R(0xA60), R(0xA80), R(0x288), R(0x28C), R(0x1E60), R(0x260), R(0xAA0), R(0xAC0), R(0x1E40)); } } } }
  { static const char* sk = getenv("XBOX_SKIPTEX0"); if (sk) { static std::vector<uint32_t> skipOffs; static bool init = false; if (!init) { init = true; for (const char* q = sk; *q;) { char* e; skipOffs.push_back((uint32_t)strtoul(q, &e, 0)); q = (*e == ',') ? e + 1 : e; if (e == q && *q) break; } } for (uint32_t o : skipOffs) if (R(0x1B00) == o) return; } }   // debug: skip every draw whose texture 0 sits at this offset
  { static int mx = getenv("XBOX_MAXDRAW") ? atoi(getenv("XBOX_MAXDRAW")) : 1 << 30, mn = getenv("XBOX_MINDRAW") ? atoi(getenv("XBOX_MINDRAW")) : 0; int idx = g_frameDraws++; if (idx >= mx || idx < mn) return; if (g_capSweep >= 0 && idx >= g_capSweep) return; }
  if (!GpuVkReady() || n < 3) return;
  int sx, sy, sw, sh; if (!SurfaceDims(&sx, &sy, &sw, &sh)) return;
  const bool ffVs = (R(0x1E94) & 3) != 2;
  FfState ffs;   // fixed-function state straight from the NV097 method registers
  if (ffVs) {
    ffs.lighting = R(0x314) != 0; ffs.localEye = (R(0x294) >> 16) & 1; ffs.normalize = R(0x3A4) != 0; ffs.skin = R(0x328) & 7;
    ffs.emission = R(0x298) & 3; ffs.ambient = (R(0x298) >> 2) & 3; ffs.diffuse = (R(0x298) >> 4) & 3; ffs.specular = (R(0x298) >> 6) & 3;
    ffs.fogOn = R(0x2A4) != 0; ffs.fogGen = R(0x2A0);
    for (int i = 0; i < 4; i++) { ffs.texMat[i] = R(0x420 + 4 * i) != 0; for (int j = 0; j < 4; j++) ffs.texgen[i][j] = R(0x3C0 + 0x10 * i + 4 * j); }
    if (ffs.lighting) for (int i = 0; i < 8; i++) ffs.light[i] = (R(0x3BC) >> (2 * i)) & 3;
  }
  if (ffVs) { static int fl = 0; if (getenv("XBOX_FFLOG") && fl++ < 12) { const float* c = Pgraph3dConstants(); Log("[ff] draw prim=%u n=%zu rt=%dx%d  v0=(%g %g %g %g) d=(%g %g %g %g) t0=(%g %g)  C0=(%g %g %g %g) C1=(%g %g %g %g) C2=(%g %g %g %g) C3=(%g %g %g %g) tex0mode=%u", prim, n, sw, sh, verts[0].a[0][0], verts[0].a[0][1], verts[0].a[0][2], verts[0].a[0][3], verts[0].a[3][0], verts[0].a[3][1], verts[0].a[3][2], verts[0].a[3][3], verts[0].a[9][0], verts[0].a[9][1], c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15], R(0x1E70) & 0x1F); } }
  BeginFrame(); Frame& fr = F(); RenderTarget* rt = GetRT(sx + sw, sy + sh);
  // ---- vertex program ----
  const uint32_t* prog = Pgraph3dProgram(); uint32_t start = R(0x1EA0);
  uint64_t vhash = ffVs ? (0xFF00FF00FF00FF00ull ^ Fnv(&ffs, sizeof ffs)) : Fnv(prog + start * 4, (136 - start) * 16, start); Shader& vs = g_vsCache[vhash];
  if (!vs.mod) { uint32_t used; auto spv = ffVs ? GenFixedVertexShader(ffs, &used) : getenv("XBOX_VS_SIMPLE") ? GenSimpleVertexShader(&used) : GenVertexShader(prog, start, &used); vs.mod = MakeModule(spv); vs.used = used; Log("[vk] vertex shader %016llx: %zu words, inputs mask 0x%x", (unsigned long long)vhash, spv.size(), used); if (ffVs) Log("[vk]   fixed-function: lighting=%u skin=%u normalize=%u fog=%u/%u texgen0=%x,%x,%x,%x texmat=%u%u%u%u lights=%u,%u,%u,%u", ffs.lighting, ffs.skin, ffs.normalize, ffs.fogOn, ffs.fogGen, ffs.texgen[0][0], ffs.texgen[0][1], ffs.texgen[0][2], ffs.texgen[0][3], ffs.texMat[0], ffs.texMat[1], ffs.texMat[2], ffs.texMat[3], ffs.light[0], ffs.light[1], ffs.light[2], ffs.light[3]); }
  // ---- fragment (combiner) state ----
  FsInfo fi; uint32_t cc = R(0x1E60); fi.numStages = cc & 0xFF; fi.uniqueC0 = (cc >> 8) & 0x10; fi.uniqueC1 = (cc >> 8) & 0x100; fi.muxMsb = (cc >> 8) & 1;
  for (int i = 0; i < 8; i++) { fi.icwC[i] = R(0xAC0 + 4 * i); fi.icwA[i] = R(0x260 + 4 * i); fi.ocwC[i] = R(0x1E40 + 4 * i); fi.ocwA[i] = R(0xAA0 + 4 * i); }
  fi.final0 = R(0x288); fi.final1 = R(0x28C); fi.alphaTest = R(0x300) != 0; fi.alphaFunc = R(0x33C);
  { static std::set<uint32_t> rtOffsets; static uint64_t hits = 0; rtOffsets.insert(R(0x210));
    for (int t = 0; t < 4; t++) { uint32_t mode = (R(0x1E70) >> (5 * t)) & 0x1F; if (mode && rtOffsets.count(R(0x1B00 + 64 * t))) { if (hits++ < 12) Log("[vk] RENDER-TO-TEXTURE: stage %d samples offset 0x%x which was a render target (RT now 0x%x, %dx%d)", t, R(0x1B00 + 64 * t), R(0x210), sw, sh); } } }
  uint64_t tA = NowUs();
  TexEntry* tex[4] = {}; RenderTarget* rtTex[4] = {}, *rtDepth[4] = {}; Img* depthSnap[4] = {}; bool selfSnap[4] = {}; Img* colorSnap[4] = {};
  for (int t = 0; t < 4; t++) {
    uint32_t mode = (R(0x1E70) >> (5 * t)) & 0x1F; fi.texMode[t] = mode;
    if (mode != 0 && SoftTextureIsCube(t)) { fi.texPresent[t] = true; fi.texCube[t] = true; if (!UploadCubeTexture(t, &tex[t])) tex[t] = nullptr; continue; }   // cube map: its own image type and sampler
    if (mode != 0) {
      RenderTarget* src = FindRTByOffset(R(0x1B00 + 64 * t), rt);
      uint64_t dk; int dw, dh; bool dl;
      if (!src && rt->lastWrite && rt->offset == R(0x1B00 + 64 * t)) { uint64_t dk0; int dw0, dh0; bool dl0; if (SoftTextureDesc(t, &dk0, &dw0, &dh0, &dl0)) { selfSnap[t] = true; rtTex[t] = rt; fi.texPresent[t] = true; fi.texLinear[t] = dl0; continue; } }
      RenderTarget* dsrc = src ? nullptr : FindRTByZeta(R(0x1B00 + 64 * t));
      if (src && SoftTextureDesc(t, &dk, &dw, &dh, &dl)) { rtTex[t] = src; fi.texPresent[t] = true; fi.texLinear[t] = dl; }
      else if (dsrc && SoftTextureDesc(t, &dk, &dw, &dh, &dl)) { { static int dl2 = 0; if (dl2++ < 4) Log("[vk] DEPTH-AS-TEXTURE: stage %d samples zeta 0x%x of RT 0x%x", t, R(0x1B00 + 64 * t), dsrc->offset); } rtDepth[t] = dsrc; fi.texPresent[t] = true; fi.texLinear[t] = dl; fi.texDepth[t] = true; }
      else if (UploadTexture(t, &tex[t])) { fi.texPresent[t] = true; fi.texLinear[t] = tex[t]->linear; }
    }
  }
  g_tTexUs += NowUs() - tA;
  uint64_t fhash = Fnv(&fi, sizeof fi); Shader& fs = g_fsCache[fhash];
  if (!fs.mod) { auto spv = getenv("XBOX_FS_SIMPLE") ? GenSimpleFragmentShader() : GenFragmentShader(fi); fs.mod = MakeModule(spv); Log("[vk] fragment shader %016llx: %zu words (%u stages) texture modes %u,%u,%u,%u cube %u%u%u%u", (unsigned long long)fhash, spv.size(), fi.numStages,
      fi.texMode[0], fi.texMode[1], fi.texMode[2], fi.texMode[3], (R(0x1B04) >> 2) & 1, (R(0x1B44) >> 2) & 1, (R(0x1B84) >> 2) & 1, (R(0x1BC4) >> 2) & 1); }
  // ---- geometry: triangle list + packed used attributes ----
  std::vector<uint32_t> idx;
  auto tri = [&](uint32_t a, uint32_t b, uint32_t c) { idx.push_back(a); idx.push_back(b); idx.push_back(c); };
  switch (prim) {
    case 5: for (size_t i = 0; i + 2 < n; i += 3) tri(i, i + 1, i + 2); break;
    case 6: for (size_t i = 0; i + 2 < n; i++) { if (i & 1) tri(i + 1, i, i + 2); else tri(i, i + 1, i + 2); } break;
    case 7: case 10: for (size_t i = 1; i + 1 < n; i++) tri(0, i, i + 1); break;
    case 8: for (size_t i = 0; i + 3 < n; i += 4) { tri(i, i + 1, i + 2); tri(i, i + 2, i + 3); } break;
    case 9: for (size_t i = 0; i + 3 < n; i += 2) { tri(i, i + 1, i + 3); tri(i, i + 3, i + 2); } break;
    default: return;
  }
  if (idx.empty()) return;
  uint32_t used = vs.used; int nUsed = plat::PopCount32(used); uint32_t stride = nUsed ? nUsed * 16 : 16;
  uint8_t* vptr; size_t voff = RingAlloc((size_t)n * stride, 16, &vptr);
  for (size_t v = 0; v < n; v++) { float* d = (float*)(vptr + v * stride); if (!nUsed) { d[0] = d[1] = d[2] = d[3] = 0; continue; } int k = 0; for (int a = 0; a < 16; a++) if (used & (1u << a)) { memcpy(d + 4 * k, verts[v].a[a], 16); k++; } }
  uint8_t* iptr; size_t ioff = RingAlloc(idx.size() * 4, 4, &iptr); memcpy(iptr, idx.data(), idx.size() * 4);
  // ---- uniforms ----
  uint8_t* vu; size_t vuOff = RingAlloc(4112, g_ubAlign, &vu); memcpy(vu, Pgraph3dConstants(), 192 * 16); memset(vu + 3088, 0, 1024);
  if (ffVs) FfpFill((float*)(vu + 3088), ffs);
  float misc[4] = {(float)(sx + sw), (float)(sy + sh), Fl(R(0x398)) > 0 ? Fl(R(0x398)) : 16777215.f, 0}; memcpy(vu + 3072, misc, 16);
  uint8_t* fu; size_t fuOff = RingAlloc(384, g_ubAlign, &fu); float* fb = (float*)fu; memset(fu, 0, 384);
  auto argb = [&](uint32_t v, float* o) { o[0] = ((v >> 16) & 255) / 255.f; o[1] = ((v >> 8) & 255) / 255.f; o[2] = (v & 255) / 255.f; o[3] = (v >> 24) / 255.f; };
  for (int i = 0; i < 8; i++) { argb(R(0xA60 + 4 * (fi.uniqueC0 ? i : 0)), fb + 4 * i); argb(R(0xA80 + 4 * (fi.uniqueC1 ? i : 0)), fb + 32 + 4 * i); }
  argb(R(0x1E20), fb + 64); argb(R(0x1E24), fb + 68); argb(R(0x294), fb + 72);   // final-combiner constants, fog colour
  for (int t = 0; t < 4; t++) { float* d = fb + 76 + 4 * t; if (tex[t]) { d[0] = (float)tex[t]->img.w; d[1] = (float)tex[t]->img.h; } else if (rtTex[t]) { d[0] = (float)rtTex[t]->w; d[1] = (float)rtTex[t]->h; } else if (rtDepth[t]) { d[0] = (float)rtDepth[t]->w; d[1] = (float)rtDepth[t]->h; } }
  fb[92] = (float)(R(0x340) & 0xFF);
  for (int t = 0; t < 4; t++) if (selfSnap[t]) colorSnap[t] = SnapshotColor(rt);
  for (int t = 0; t < 4; t++) if (rtTex[t] && !selfSnap[t] && rtTex[t]->color.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {   // sampled render target: leave rendering, make its colour image shader-readable
    EndRendering(); Barrier(fr.cb, rtTex[t]->color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT); }
  for (int t = 0; t < 4; t++) if (rtDepth[t]) depthSnap[t] = SnapshotDepth(rtDepth[t]);
  uint64_t tB = NowUs();
  // ---- descriptor set ----
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = fr.dpool; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_dsl; VkDescriptorSet ds = VK_NULL_HANDLE; CHK(vkAllocateDescriptorSets(g_dev, &ai, &ds));
  VkDescriptorBufferInfo bi0{fr.ring.b, 0, 4112}, bi1{fr.ring.b, 0, 384};
  std::vector<VkWriteDescriptorSet> ws; VkDescriptorImageInfo ii[4];
  { VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = ds; w.dstBinding = 0; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w.pBufferInfo = &bi0; ws.push_back(w); }
  { VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = ds; w.dstBinding = 5; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w.pBufferInfo = &bi1; ws.push_back(w); }
  static Img dummy; if (!dummy.img) { EndRendering(); dummy = MakeImg(1, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    Barrier(fr.cb, dummy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue cv{{0, 0, 0, 0}}; VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; vkCmdClearColorImage(fr.cb, dummy.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rg);
    Barrier(fr.cb, dummy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT); }
  static Img dummyCube; if (!dummyCube.img) { EndRendering(); dummyCube = MakeImg(1, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, true);
    VkImageMemoryBarrier cb0{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; cb0.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; cb0.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; cb0.srcQueueFamilyIndex = cb0.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; cb0.image = dummyCube.img; cb0.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6}; cb0.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(fr.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &cb0);
    VkClearColorValue cv{{0, 0, 0, 0}}; VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6}; vkCmdClearColorImage(fr.cb, dummyCube.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rg);
    VkImageMemoryBarrier cb1 = cb0; cb1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; cb1.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; cb1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; cb1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(fr.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &cb1); }
  for (int t = 0; t < 4; t++) {
    uint32_t base = 0x1B00 + 64 * t, filt = R(base + 0x14), addr = R(base + 8); int mag = ((filt >> 24) & 0xF) == 2, au = (addr & 7) >= 3, av = ((addr >> 8) & 7) >= 3;
    ii[t].sampler = g_samplers[mag][au][av]; ii[t].imageView = fi.texCube[t] ? (tex[t] ? tex[t]->img.view : dummyCube.view) : tex[t] ? tex[t]->img.view : colorSnap[t] ? colorSnap[t]->view : rtTex[t] ? rtTex[t]->color.view : depthSnap[t] ? depthSnap[t]->view : dummy.view; ii[t].imageLayout = depthSnap[t] ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = ds; w.dstBinding = 1 + t; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii[t]; ws.push_back(w);
  }
  vkUpdateDescriptorSets(g_dev, (uint32_t)ws.size(), ws.data(), 0, nullptr);
  // ---- pipeline + draw ----
  {   // XBOX_DRAWLOG=<first flip>,<count>: one line per draw in those flips (shader hashes, texture modes, render target, blend, depth, textures); compare two machines draw by draw
    static const char* dl = getenv("XBOX_DRAWLOG"); static int dl0 = -1, dln = 0, every = 0, armedAt = -1; static bool parsed = false;
    if (dl && !parsed) { parsed = true; if (!strncmp(dl, "every=", 6)) every = atoi(dl + 6); else if (!strcmp(dl, "file")) every = -1; else sscanf(dl, "%d,%d", &dl0, &dln); }
    if (dl && every == -1 && armedAt < 0) { if (FILE* ff = fopen("drawlog.flag", "rb")) { fclose(ff); remove("drawlog.flag"); armedAt = g_dbgFlips; } }   // file mode: create drawlog.flag to log the next two flips
    bool logNow = dl && ((every > 0 && g_dbgFlips > 0 && (g_dbgFlips % every) < 2) || (every == -1 && armedAt >= 0 && g_dbgFlips >= armedAt && g_dbgFlips < armedAt + 2) || (every == 0 && g_dbgFlips >= dl0 && g_dbgFlips < dl0 + dln));
    if (every == -1 && armedAt >= 0 && g_dbgFlips >= armedAt + 2) armedAt = -1;
    if (logNow) { g_drawLogFlip = g_dbgFlips;
      char tx[256]; tx[0] = 0; size_t tp = 0;
      for (int t = 0; t < 4; t++) if (fi.texPresent[t] && tp < 200) tp += snprintf(tx + tp, sizeof tx - tp, " t%d:f%02x%s%dx%d", t, (R(0x1B04 + 64 * t) >> 8) & 0xFF, fi.texCube[t] ? "C" : (rtTex[t] || selfSnap[t] ? "R" : ""), tex[t] ? tex[t]->img.w : 0, tex[t] ? tex[t]->img.h : 0);
      Log("[drawlog] flip %d draw %d rt=%dx%d@%x prim=%u n=%zu vs=%08x fs=%08x modes=%u,%u,%u,%u stages=%u blend=%u(%x,%x) depth=%u/%u%s", g_dbgFlips, g_frameDraws, rt->w, rt->h, rt->offset, prim, n, (unsigned)vhash, (unsigned)fhash,
          fi.texMode[0], fi.texMode[1], fi.texMode[2], fi.texMode[3], fi.numStages, R(0x304) != 0, R(0x344), R(0x348), R(0x30C) != 0, R(0x35C) != 0, tx);
    }
  }
  PipeKey pk{}; pk.vs = vhash; pk.fs = fhash; pk.usedMask = used; pk.blendEnable = R(0x304) != 0; pk.sfac = R(0x344); pk.dfac = R(0x348); pk.eq = R(0x350); pk.depthTest = R(0x30C) != 0; pk.depthFunc = R(0x354); pk.depthMask = R(0x35C) != 0;
  { static int zl = 0; if (zl++ < 6) Log("[vk] depth state: test=%u mask=%u func=0x%x zeta=0x%x (RT 0x%x)", pk.depthTest, pk.depthMask, pk.depthFunc, R(0x214), rt->offset); }
  if (pk.depthTest && pk.depthMask) { rt->zeta = R(0x214); rt->lastDepthWrite = g_drawSerial + 1; }
  pk.cull = R(0x308) ? R(0x39C) : 0; pk.front = R(0x3A0); pk.colorMask = R(0x358);
  g_tDescUs += NowUs() - tB;
  TR("[vk] draw: creating pipeline");
  VkPipeline pipe = GetPipeline(pk, vs, fs);
  TR("[vk] draw: pipeline ok %p; begin rendering", (void*)pipe);
  { if ((void*)rt != g_ovLastRt) { g_ovLastRt = rt; ++g_ovSwitches; g_ovSince = 0; }
    static int cl = 0; if (getenv("XBOX_COMPLOG") && g_ovSwitches >= 3 && g_ovSince == 0 && cl < 60 && g_dbgFlips >= 570 && g_dbgFlips < 572) { ++cl;   // XBOX_COMPLOG=1: log the first draw after every render-target switch (post-process chain) in detail
      Log("[comp] zeta offset=0x%x colour offset=0x%x flip %d switch#%d draw#%d -> RT off=0x%x %dx%d blend=%u src=%x dst=%x eq=%x depth=%u mask=%08x ps=%08x/%08x stages=%u mode=0x%x", R(0x214), R(0x210), g_dbgFlips, g_ovSwitches, g_frameDraws, rt->offset, rt->w, rt->h, R(0x304), R(0x344), R(0x348), R(0x350), R(0x30C), R(0x358), R(0x288), R(0x28C), R(0x1E60) & 0xFF, R(0x1E70));
      for (int t = 0; t < 4; t++) { uint32_t mode = (R(0x1E70) >> (5 * t)) & 0x1F; if (!mode) continue; uint32_t off = R(0x1B00 + 64 * t); RenderTarget* m = FindRTByOffset(off, rt); uint64_t dk; int dw = 0, dh = 0; bool dl = false; SoftTextureDesc(t, &dk, &dw, &dh, &dl);
        Log("[comp]    stage %d mode=%u tex off=0x%x fmt=0x%08x desc %dx%d linear=%d filter=0x%08x -> %s (RT off=0x%x %dx%d lastWrite=%llu)", t, mode, off, R(0x1B04 + 64 * t), dw, dh, (int)dl, R(0x1B14 + 64 * t), m ? "RENDER TARGET" : "memory texture", m ? m->offset : 0, m ? m->w : 0, m ? m->h : 0, m ? (unsigned long long)m->lastWrite : 0ull); } }
    { static int skipSw = getenv("XBOX_SKIPSWITCH") ? atoi(getenv("XBOX_SKIPSWITCH")) : -1; if (skipSw >= 0 && g_ovSwitches == skipSw) { ++g_ovSince; return; } }   // debug: skip the whole draw segment after the Nth render-target switch of a frame
    if (getenv("XBOX_OVLOG") && g_ovSwitches >= 10 && g_ovSince < 12 && g_dbgFlips >= 570 && g_dbgFlips < 572) { float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0, bp[4], bd0[4], bd1[4], bt0[4]; SoftScreenBBox(verts, n, &bx0, &by0, &bx1, &by1); SoftFirstVertexOut(verts, n, bp, bd0, bd1, bt0);
      Log("[ov] flip %d u=%d draw#%d prim=%u n=%zu bbox=(%.0f,%.0f)-(%.0f,%.0f) blend=%u src=%x dst=%x depth=%u/%u alphatest=%u/%x mode=0x%x stages=%u final=%08x/%08x d0=(%.2f %.2f %.2f %.2f) t0=(%.2f %.2f) tex0 off=0x%x fmt=0x%08x tex1 off=0x%x tex2 off=0x%x tex3 off=0x%x", g_dbgFlips, g_ovSince, g_frameDraws, prim, n, bx0, by0, bx1, by1, R(0x304), R(0x344), R(0x348), R(0x30C), R(0x35C), R(0x300), R(0x33C), R(0x1E70), R(0x1E60) & 0xFF, R(0x288), R(0x28C), bd0[0], bd0[1], bd0[2], bd0[3], bt0[0], bt0[1], R(0x1B00), R(0x1B04), R(0x1B40), R(0x1B80), R(0x1BC0)); }
    int u = g_ovSince++; if (g_ovCap >= 0 && g_ovSwitches >= 10 && u >= g_ovCap) return; }
  { static const char* fl = getenv("XBOX_FRAMELOG2"); static RenderTarget* last = nullptr; static int perRt = 0;   // XBOX_FRAMELOG2=flipNo: log render-target switches for two frames starting at that flip
    if (fl && g_dbgFlips >= atoi(fl) && g_dbgFlips < atoi(fl) + 2) { if (rt != last) { Log("[frame2] flip %d draw#%d -> RT off=0x%x %dx%d zeta=0x%x zfmt=%u pitch=0x%x (prev RT got %d draws)", g_dbgFlips, g_frameDraws, rt->offset, rt->w, rt->h, R(0x214), (R(0x208) >> 4) & 15, R(0x20C), perRt); last = rt; perRt = 0; } ++perRt; } }
  BeginRendering(rt); TR("[vk] draw: rendering begun"); VkCommandBuffer cb = fr.cb;
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  uint32_t dyn[2] = {(uint32_t)vuOff, (uint32_t)fuOff}; vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_playout, 0, 1, &ds, 2, dyn);
  VkViewport vpt{(float)sx, (float)sy, (float)sw, (float)sh, 0.f, 1.f}; vkCmdSetViewport(cb, 0, 1, &vpt); VkRect2D sc{{sx, sy}, {(uint32_t)sw, (uint32_t)sh}}; vkCmdSetScissor(cb, 0, 1, &sc);
  float bc[4]; argb(R(0x34C), bc); vkCmdSetBlendConstants(cb, bc);
  VkBuffer vb = fr.ring.b; VkDeviceSize vo = voff; vkCmdBindVertexBuffers(cb, 0, 1, &vb, &vo); vkCmdBindIndexBuffer(cb, fr.ring.b, ioff, VK_INDEX_TYPE_UINT32);
  TR("[vk] draw: recording indexed draw of %zu indices", idx.size());
  vkCmdDrawIndexed(cb, (uint32_t)idx.size(), 1, 0, 0, 0); rt->lastWrite = ++g_drawSerial;
  TR("[vk] draw: done");
}

// Buffer swap: present the render target the title just finished.
static void GpuFlipImpl();
// Frame dump (debug): XBOX_FRAMEDUMP="300,600" dumps at those flip numbers, "every=120" every 120 flips. Writes frame_<flip>_<n>.bmp (n=0: the presented image, then the most
// recently written other render targets) into the working directory and logs each image's mean colour (the numbers alone are enough to compare two machines).
static bool FrameDumpDue(int flip) {
  static const char* e = getenv("XBOX_FRAMEDUMP"); if (!e || !*e) return false;
  if (!strcmp(e, "sync")) return flip >= g_drawLogFlip && flip <= g_drawLogFlip + 1 && g_drawLogFlip >= 0;
  if (!strncmp(e, "every=", 6)) { int n = atoi(e + 6); return n > 0 && flip > 0 && flip % n == 0; }
  for (const char* q = e; *q;) { if (atoi(q) == flip) return true; while (*q && *q != ',') ++q; if (*q == ',') ++q; }
  return false;
}
struct DumpReq { uint8_t* p; int w, h, n; bool file; };
static void WriteDump(int flip, const DumpReq& d) {
  size_t px = (size_t)d.w * d.h; double sum[3] = {0, 0, 0};
  for (size_t i = 0; i < px; i += 3) { sum[0] += d.p[i * 4 + 2]; sum[1] += d.p[i * 4 + 1]; sum[2] += d.p[i * 4]; }   // image is BGRA
  double cnt = (double)(px / 3 + 1);
  static const uint32_t t0 = (uint32_t)plat::TickMs();   // (the first dump happens a while after start: add the time of the first dump to compare runs)
  Log("[dump] flip %d t=%.1fs image %d %dx%d mean RGB = (%.1f %.1f %.1f)", flip, ((uint32_t)plat::TickMs() - g_startMs) / 1000.0, d.n, d.w, d.h, sum[0] / cnt, sum[1] / cnt, sum[2] / cnt); (void)t0;
  if (!d.file) return;
  char name[96]; snprintf(name, sizeof name, "frame_%d_%d.bmp", flip, d.n);
  FILE* f = fopen(name, "wb"); if (!f) return;
  uint32_t fsz = 54 + (uint32_t)px * 4, z = 0, off = 54, hs = 40, pl = 1 | (32 << 16); int32_t w = d.w, h = d.h;
  fwrite("BM", 1, 2, f); fwrite(&fsz, 4, 1, f); fwrite(&z, 4, 1, f); fwrite(&off, 4, 1, f); fwrite(&hs, 4, 1, f); fwrite(&w, 4, 1, f); fwrite(&h, 4, 1, f);
  fwrite(&pl, 4, 1, f); for (int i = 0; i < 6; i++) fwrite(&z, 4, 1, f);
  for (int y = d.h - 1; y >= 0; y--) fwrite(d.p + (size_t)y * d.w * 4, 4, d.w, f);   // bottom-up rows
  fclose(f);
}

void GpuFlip() {
  uint64_t t = NowUs(); if (!g_statT0) g_statT0 = t;
  GpuFlipImpl();
  uint64_t t2 = NowUs(); g_statFlipUs += t2 - t; ++g_statFlips;
  if (t2 - g_statT0 >= 1000000) { Log("[perf] %llu flips, %llu draws, draw %llu ms (tex %llu, desc %llu), flip %llu ms per %llu ms", (unsigned long long)g_statFlips, (unsigned long long)g_statDraws, (unsigned long long)(g_statDrawUs / 1000), (unsigned long long)(g_tTexUs / 1000), (unsigned long long)(g_tDescUs / 1000), (unsigned long long)(g_statFlipUs / 1000), (unsigned long long)((t2 - g_statT0) / 1000)); g_statFlips = g_statDraws = g_statDrawUs = g_statFlipUs = g_tTexUs = g_tDescUs = 0; g_statT0 = t2; }
}
static void GpuFlipImpl() {
  g_lastFrameDraws = g_frameDraws; g_frameDraws = 0; ++g_dbgFlips; g_ovSwitches = 0; g_ovSince = 0; g_ovLastRt = nullptr;
  { static const char* sw = getenv("XBOX_CAPLIST"); static uint32_t t0 = (uint32_t)plat::TickMs();   // XBOX_CAPLIST="16:100,20:300,...": from t seconds on, cap draws per frame at N (logged as [sweep])
    if (sw) { uint32_t el = ((uint32_t)plat::TickMs() - t0) / 1000; int cap = -1; const char* q = sw; while (*q) { int t = 0, c = 0, used = 0; if (sscanf(q, "%d:%d%n", &t, &c, &used) < 2) break; if ((int)el >= t) cap = c; q += used; if (*q == ',') ++q; }
      if (cap != g_capSweep) { g_capSweep = cap; Log("[sweep] cap %d at %lus", cap, (unsigned long)el); } } }
  { static const char* os = getenv("XBOX_OVERLAYSWEEP"); static uint32_t t0 = (uint32_t)plat::TickMs(); static int base = -1;   // XBOX_OVERLAYSWEEP="Ustart,Uend,afterSeconds": in the final overlay phase (after >= 10 render-target switches) skip draws once Ucur draws have run since the last switch; Ucur rises one per flip
    if (os) { int a = 0, b = 0, after = 0; sscanf(os, "%d,%d,%d", &a, &b, &after); if ((int)((uint32_t)plat::TickMs() - t0) > after * 1000) { if (base < 0) base = g_dbgFlips; int u = a + (g_dbgFlips - base); g_ovCap = u <= b ? u : -1; } } }
  { static const char* ts = getenv("XBOX_TAILSWEEP"); static uint32_t t0 = (uint32_t)plat::TickMs(); static int base = -1;   // XBOX_TAILSWEEP="Kstart,Kend,afterSeconds": skip the last K draws of each frame (K from Kstart down to Kend, one per flip), relative to the previous frame's draw count
    if (ts) { int a = 0, b = 0, after = 0; sscanf(ts, "%d,%d,%d", &a, &b, &after); if ((int)((uint32_t)plat::TickMs() - t0) > after * 1000) { if (base < 0) base = g_dbgFlips; int k = a - (g_dbgFlips - base); if (k < b) { g_capSweep = -1; g_tailK = -1; } else { g_tailK = k; g_capSweep = g_lastFrameDraws - k; if (g_capSweep < 0) g_capSweep = 0; } } } }
  { static const char* cs = getenv("XBOX_CAPSTEP"); static uint32_t t0 = (uint32_t)plat::TickMs(); static int base = -1;   // XBOX_CAPSTEP="start,end,afterSeconds": per-frame draw cap goes start, start+1, ... one step per flip (use with XBOX_LUMLOG)
    if (cs) { int a = 0, b = 0, after = 0; sscanf(cs, "%d,%d,%d", &a, &b, &after); if ((int)((uint32_t)plat::TickMs() - t0) > after * 1000) { if (base < 0) base = g_dbgFlips; int c = a + (g_dbgFlips - base); g_capSweep = c <= b ? c : -1; } } }
  if (!g_ok) return;
  Frame& fr = F(); if (!fr.recording) { BeginFrame(); }
  if (!g_rt || !EnsureSwapchain()) { EndRendering(); vkEndCommandBuffer(fr.cb); fr.recording = false; VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &fr.cb; vkResetFences(g_dev, 1, &fr.fence); vkQueueSubmit(g_queue, 1, &si, fr.fence); g_fi ^= 1; ++g_frameNo; return; }
  EndRendering(); VkCommandBuffer cb = fr.cb;
  uint32_t ix = 0; VkResult ar = vkAcquireNextImageKHR(g_dev, g_swap, ~0ull, fr.imageAvail, VK_NULL_HANDLE, &ix);
  if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { Log("[vk] acquire failed %d", (int)ar); }
  Img& rtc = g_rt->color; Barrier(cb, rtc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkImageMemoryBarrier sb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; sb.oldLayout = g_swapLayouts[ix]; sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; sb.srcQueueFamilyIndex = sb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; sb.image = g_swapImages[ix];
  sb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; sb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &sb);
  VkImageBlit bl{}; bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bl.srcOffsets[1] = {g_rt->w, g_rt->h, 1}; bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bl.dstOffsets[1] = {(int)g_swapExtent.width, (int)g_swapExtent.height, 1};
  vkCmdBlitImage(cb, rtc.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_swapImages[ix], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
  const uint8_t* lumPtr = nullptr; int lumW = g_rt->w, lumH = g_rt->h; static bool lumOn = getenv("XBOX_LUMLOG") != nullptr;   // XBOX_LUMLOG=1: read the presented image back and log its mean brightness per flip (with the draw cap), for automated bisection
  if (lumOn) { uint8_t* dst; size_t off = RingAlloc((size_t)lumW * lumH * 4, 16, &dst); VkBufferImageCopy bc{}; bc.bufferOffset = off; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {(uint32_t)lumW, (uint32_t)lumH, 1}; vkCmdCopyImageToBuffer(cb, rtc.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fr.ring.b, 1, &bc); lumPtr = dst; }
  if (g_pcache && g_dbgFlips > 0 && g_dbgFlips % 600 == 0) {   // write the pipeline cache back when it has grown
    size_t sz = 0; if (vkGetPipelineCacheData(g_dev, g_pcache, &sz, nullptr) == VK_SUCCESS && sz > g_pcacheSaved + 4096) {
      std::vector<uint8_t> data(sz); if (vkGetPipelineCacheData(g_dev, g_pcache, &sz, data.data()) == VK_SUCCESS) {
        if (FILE* cf = fopen("pipeline_cache.tmp", "wb")) { fwrite(data.data(), 1, sz, cf); fclose(cf); remove("pipeline_cache.bin"); rename("pipeline_cache.tmp", "pipeline_cache.bin"); g_pcacheSaved = sz; Log("[vk] pipeline cache saved: %zu bytes", sz); } } }
  }
  std::vector<DumpReq> dumpReqs;
  if (!FrameDumpDue(g_dbgFlips) && g_dbgFlips > 0 && g_dbgFlips % 300 == 0 && g_dbgFlips <= 12000) {   // always on: the presented image's mean colour every 300 flips (a black screen shows up in the log)
    uint8_t* dst; size_t off = RingAlloc((size_t)g_rt->w * g_rt->h * 4, 16, &dst);
    VkBufferImageCopy bc{}; bc.bufferOffset = off; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {(uint32_t)g_rt->w, (uint32_t)g_rt->h, 1};
    vkCmdCopyImageToBuffer(cb, rtc.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fr.ring.b, 1, &bc); dumpReqs.push_back({dst, g_rt->w, g_rt->h, 0, false});
  }
  if (FrameDumpDue(g_dbgFlips)) {
    auto grab = [&](Img& im, int w, int h, int n, bool inTransferSrc) {
      if (im.layout == VK_IMAGE_LAYOUT_UNDEFINED) return;
      VkImageLayout orig = im.layout;
      if (!inTransferSrc) Barrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      uint8_t* dst; size_t off = RingAlloc((size_t)w * h * 4, 16, &dst);
      VkBufferImageCopy bc{}; bc.bufferOffset = off; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
      vkCmdCopyImageToBuffer(cb, im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fr.ring.b, 1, &bc);
      if (!inTransferSrc) Barrier(cb, im, orig, VK_IMAGE_ASPECT_COLOR_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      dumpReqs.push_back({dst, w, h, n, true});
    };
    grab(rtc, g_rt->w, g_rt->h, 0, true);
    std::vector<RenderTarget*> others; for (auto* r : g_rts) if (r != g_rt && r->lastWrite && r->color.img && r->w * r->h <= 1280 * 960) others.push_back(r);
    std::sort(others.begin(), others.end(), [](RenderTarget* x, RenderTarget* y) { return x->lastWrite > y->lastWrite; });
    for (size_t i = 0; i < others.size() && i < 5; i++) grab(others[i]->color, others[i]->w, others[i]->h, (int)i + 1, false);
  }
  sb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; sb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; sb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; sb.dstAccessMask = 0;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &sb); g_swapLayouts[ix] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  vkEndCommandBuffer(cb); fr.recording = false;
  VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.waitSemaphoreCount = 1; si.pWaitSemaphores = &fr.imageAvail; si.pWaitDstStageMask = &wait; si.commandBufferCount = 1; si.pCommandBuffers = &cb; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &fr.renderDone;
  vkResetFences(g_dev, 1, &fr.fence); CHK(vkQueueSubmit(g_queue, 1, &si, fr.fence));
  if (!dumpReqs.empty()) { vkWaitForFences(g_dev, 1, &fr.fence, VK_TRUE, ~0ull); for (auto& q : dumpReqs) WriteDump(g_dbgFlips, q); }
  if (lumPtr) { vkWaitForFences(g_dev, 1, &fr.fence, VK_TRUE, ~0ull); double sum = 0; size_t n = (size_t)lumW * lumH; for (size_t i = 0; i < n; i += 7) sum += lumPtr[i * 4] + lumPtr[i * 4 + 1] + lumPtr[i * 4 + 2]; Log("[lum] flip %d cap %d tailK %d ovCap %d lastframe-draws %d mean %.2f", g_dbgFlips, g_capSweep, g_tailK, g_ovCap, g_lastFrameDraws, sum / (n / 7 + 1) / 3); }
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &fr.renderDone; pi.swapchainCount = 1; pi.pSwapchains = &g_swap; pi.pImageIndices = &ix; vkQueuePresentKHR(g_queue, &pi);
  g_fi ^= 1; ++g_frameNo; g_rendering = false;
}
