#ifndef PLATFORM_MATH_H
#define PLATFORM_MATH_H
#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define isnan(value) __builtin_isnan(value)
#define isinf(value) __builtin_isinf(value)
#define isfinite(value) __builtin_isfinite(value)
#define signbit(value) __builtin_signbit(value)
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define fpclassify(value)                                                      \
  __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO,  \
                       value)
double floor(double);
double ceil(double);
double fabs(double);
double fmod(double, double);
double pow(double, double);
double sqrt(double);
double sin(double);
double cos(double);
double tan(double);
double asin(double);
double acos(double);
double atan(double);
double atan2(double, double);
double log(double);
double exp(double);
double log2(double);
double log10(double);
double trunc(double);
double round(double);
double cbrt(double);
double copysign(double, double);
double nearbyint(double);
double ldexp(double, int);
double sinh(double);
double cosh(double);
double tanh(double);
double asinh(double);
double acosh(double);
double atanh(double);
double expm1(double);
double log1p(double);
long lrint(double);
double fmin(double,double);
double fmax(double,double);
double hypot(double,double);
#endif
