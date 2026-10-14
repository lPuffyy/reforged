// Windows display backend: a plain window. Software frames are blitted with GDI; the Vulkan renderer creates its surface from the same window.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include "display.h"
#include "plat.h"

namespace {
HWND g_wnd;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_CLOSE) { TerminateProcess(GetCurrentProcess(), 0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

uint32_t DisplayThread(void*) {
  WNDCLASSA wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "ReforgedDisplay"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassA(&wc);
  RECT r{0, 0, 640, 480}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  g_wnd = CreateWindowA("ReforgedDisplay", "Reforged", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
  MSG msg; uint32_t last = 0;
  for (;;) {
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    uint32_t now = (uint32_t)plat::TickMs();
    if (now - last >= 16) { last = now; DisplayTick(); }
    plat::SleepMs(2);
  }
}
}  // namespace

void DisplayInit() { plat::ThreadSpawn(DisplayThread); }

void DisplayBlit(const uint32_t* bgra, int w, int h) {
  if (!g_wnd) return;
  BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
  HDC dc = GetDC(g_wnd); RECT rc; GetClientRect(g_wnd, &rc);
  StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, w, h, bgra, &bi, DIB_RGB_COLORS, SRCCOPY);
  ReleaseDC(g_wnd, dc);
}

bool DisplayHasFocus() { return g_wnd && GetForegroundWindow() == g_wnd; }

const char* DisplayVulkanLibrary() { return "vulkan-1.dll"; }
int DisplayVulkanExtensions(const char** out, int maxCount) { const char* e[] = {"VK_KHR_surface", "VK_KHR_win32_surface"}; int n = 0; for (const char* x : e) if (n < maxCount) out[n++] = x; return n; }

bool DisplayCreateSurface(void* inst, void* getProc, void* surfaceOut) {
  HWND hwnd = g_wnd; for (int i = 0; i < 200 && !hwnd; i++) { plat::SleepMs(50); hwnd = g_wnd; }
  if (!hwnd) return false;
  auto gipa = (PFN_vkGetInstanceProcAddr)getProc;
  auto create = (PFN_vkCreateWin32SurfaceKHR)gipa((VkInstance)inst, "vkCreateWin32SurfaceKHR"); if (!create) return false;
  VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR}; sci.hinstance = GetModuleHandleA(nullptr); sci.hwnd = hwnd;
  return create((VkInstance)inst, &sci, nullptr, (VkSurfaceKHR*)surfaceOut) == VK_SUCCESS;
}
#endif
