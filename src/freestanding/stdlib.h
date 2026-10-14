// Minimal freestanding <stdlib.h>: declarations only (see string.h).
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* malloc(size_t n);
void free(void* p);
int posix_memalign(void** out, size_t alignment, size_t n);
void abort(void) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif
