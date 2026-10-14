// Entry point of a translated-code module: compiled together with the generated sources (lift_*.cpp, lift_table.cpp) into a shared library that the
// runtime loads at startup (XBOX_LIFT_MODULE=path). The module reaches the runtime only through the HostApi table given to reforged_module_init().
#ifndef LIFT_MODULE
#error "build this file with -DLIFT_MODULE"
#endif
#ifdef GUEST_IDENTITY
#error "modules use the relocated guest layout"
#endif
#include "lift_rt.h"

#ifdef _WIN32
#define LIFT_EXPORT extern "C" __declspec(dllexport)
#else
#define LIFT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

const HostApi* g_host = nullptr;
extern "C" {
uintptr_t g_guestBase = 0;                // G(a) = g_guestBase + a, set at init from the host
volatile long long g_liftIcount = 0;      // instruction counter when the module was translated with --count
}

struct LiftEntry { uint32_t addr; LiftFn fn; uint32_t hi; uint32_t flags; };
extern "C" const LiftEntry g_liftTable[];
extern "C" const unsigned g_liftCount;

LIFT_EXPORT int reforged_module_init(const HostApi* host, const LiftEntry** table, unsigned* count) {
  if (!host || host->abi != LIFT_ABI_VERSION) return -1;
  g_host = host; g_guestBase = host->guestBase;
  *table = g_liftTable; *count = g_liftCount;
  return 0;
}
