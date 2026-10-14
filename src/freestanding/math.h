// Minimal freestanding <math.h>: the functions the translated x87/SSE code uses, declared for the system math library, plus classification macros
// built on compiler intrinsics.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
double sin(double); double cos(double); double tan(double); double atan(double); double atan2(double, double);
double sqrt(double); double pow(double, double); double log(double); double log2(double); double log10(double); double exp(double); double exp2(double);
double ldexp(double, int); double frexp(double, int*); double fabs(double); double floor(double); double ceil(double); double trunc(double);
double fmod(double, double); double rint(double); double nearbyint(double); double round(double); double copysign(double, double);
float sinf(float); float cosf(float); float tanf(float); float sqrtf(float); float fabsf(float); float floorf(float); float ceilf(float); float powf(float, float);
long lrint(double); long long llrint(double);
#ifdef __cplusplus
}
#endif
#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
