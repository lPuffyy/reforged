// Minimal freestanding <fenv.h> for arm64 Apple platforms (rounding-mode access used by the SSE-to-NEON shim). Values match the system header.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#define FE_TONEAREST 0x00000000
#define FE_UPWARD 0x00400000
#define FE_DOWNWARD 0x00800000
#define FE_TOWARDZERO 0x00C00000
typedef struct { unsigned long long __fpcr; unsigned int __fpsr; } fenv_t;
typedef unsigned int fexcept_t;
int fegetround(void);
int fesetround(int mode);
#ifdef __cplusplus
}
#endif
