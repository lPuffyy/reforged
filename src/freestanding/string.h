// Minimal freestanding <string.h>: declarations only. The translated-code module is built without any platform headers; these functions are resolved
// from the system library when the module is loaded.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
#ifdef __cplusplus
}
#endif
