// Minimal stand-in for the Apple cache-control header (declarations only; the functions come from the system library when the module is loaded).
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void sys_icache_invalidate(void* start, size_t len);
void sys_dcache_flush(void* start, size_t len);
#ifdef __cplusplus
}
#endif
