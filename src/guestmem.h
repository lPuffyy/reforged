// Guest address translation. Every guest address (a 32-bit Xbox virtual address) goes through G() to become a host pointer, and GA() converts back.
// GUEST_IDENTITY (the Windows host today): the XBE and RAM are mapped at their own addresses, so G/GA are casts. Otherwise guest memory lives in one
// reserved region at g_guestBase and G(a) = base + a (the iOS/Linux layout). Lifted code and host code must both use these, never raw casts.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern uintptr_t g_guestBase;
#ifdef __cplusplus
}
#endif
#ifdef GUEST_IDENTITY
#define G(a)  ((uint8_t*)(uintptr_t)(uint32_t)(a))
#define GA(p) ((uint32_t)(uintptr_t)(p))
#else
#define G(a)  ((uint8_t*)(g_guestBase + (uint32_t)(a)))
#define GA(p) ((uint32_t)((uintptr_t)(p) - g_guestBase))
#endif

// Guest heap (host/guestmem.cpp): memory for objects the guest holds pointers to. Zeroed, 16-byte aligned.
uint32_t GuestAlloc(uint32_t size); uint32_t GuestAllocSize(uint32_t addr); void GuestFree(uint32_t addr);
// Handle table: host object (HANDLE, fd, ...) <-> the 32-bit value the guest sees.
uint32_t HandleNew(void* h); void* HandleGet(uint32_t v); void* HandleRelease(uint32_t v);
// Host memory the guest will see (stacks, heaps, VirtualAlloc results): placed below the hardware windows. Windows identity mode only; other hosts use the guest arena.
void* GuestVirtualAlloc(uint32_t size, uint32_t type, uint32_t prot);
// Per-thread KPCR pointer (guest address). On Win32 x86 it is also mirrored in fs:[0x14] for the native-fallback stubs.
uint32_t XPcrGet(); void XPcrSet(uint32_t pcr);
void GuestMemInit();   // pick the guest base (non-identity builds); call before any guest memory is touched
