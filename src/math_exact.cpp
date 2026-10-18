#include "shared/libc_common.h"
#undef LIBC_MATH_USE_SYSTEM_FENV
#include "shared/math/acos.h"
#include "shared/math/acosf.h"
#include "shared/math/asin.h"
#include "shared/math/asinf.h"
#include "shared/math/atan.h"
#include "shared/math/atan2.h"
#include "shared/math/atan2f.h"
#include "shared/math/atanf.h"
#include "shared/math/cbrt.h"
#include "shared/math/cbrtf.h"
#include "shared/math/ceil.h"
#include "shared/math/ceilf.h"
#include "shared/math/cos.h"
#include "shared/math/cosf.h"
#include "shared/math/exp.h"
#include "shared/math/exp2.h"
#include "shared/math/exp2f.h"
#include "shared/math/expf.h"
#include "shared/math/expm1.h"
#include "shared/math/expm1f.h"
#include "shared/math/fabs.h"
#include "shared/math/fabsf.h"
#include "shared/math/floor.h"
#include "shared/math/floorf.h"
#include "shared/math/fma.h"
#include "shared/math/fmaf.h"
#include "shared/math/fmax.h"
#include "shared/math/fmaxf.h"
#include "shared/math/fmin.h"
#include "shared/math/fminf.h"
#include "shared/math/fmod.h"
#include "shared/math/fmodf.h"
#include "shared/math/hypot.h"
#include "shared/math/hypotf.h"
#include "shared/math/log.h"
#include "shared/math/log10.h"
#include "shared/math/log10f.h"
#include "shared/math/log1p.h"
#include "shared/math/log1pf.h"
#include "shared/math/log2.h"
#include "shared/math/log2f.h"
#include "shared/math/logf.h"
#include "shared/math/nextafterf.h"
#include "shared/math/pow.h"
#include "shared/math/powf.h"
#include "shared/math/remainder.h"
#include "shared/math/remainderf.h"
#include "shared/math/round.h"
#include "shared/math/roundf.h"
#include "shared/math/sin.h"
#include "shared/math/sinf.h"
#include "shared/math/sqrt.h"
#include "shared/math/sqrtf.h"
#include "shared/math/tan.h"
#include "shared/math/tanf.h"
#include "shared/math/trunc.h"
#include "shared/math/truncf.h"
extern "C" double fabs(double value) {
  return LIBC_NAMESPACE::shared::fabs(value);
}
extern "C" double trunc(double value) {
  return LIBC_NAMESPACE::shared::trunc(value);
}
extern "C" double floor(double value) {
  return LIBC_NAMESPACE::shared::floor(value);
}
extern "C" double ceil(double value) {
  return LIBC_NAMESPACE::shared::ceil(value);
}
extern "C" double sqrt(double value) {
  return LIBC_NAMESPACE::shared::sqrt(value);
}
extern "C" double fmod(double left, double right) {
  return LIBC_NAMESPACE::shared::fmod(left, right);
}
extern "C" double log2(double value) {
  return LIBC_NAMESPACE::shared::log2(value);
}
extern "C" double log(double value) {
  return LIBC_NAMESPACE::shared::log(value);
}
extern "C" double log10(double value) {
  return LIBC_NAMESPACE::shared::log10(value);
}
extern "C" double exp(double value) {
  return LIBC_NAMESPACE::shared::exp(value);
}
extern "C" double pow(double left, double right) {
  return LIBC_NAMESPACE::shared::pow(left, right);
}
extern "C" double sin(double value) {
  return LIBC_NAMESPACE::shared::sin(value);
}
extern "C" double cos(double value) {
  return LIBC_NAMESPACE::shared::cos(value);
}
extern "C" double tan(double value) {
  return LIBC_NAMESPACE::shared::tan(value);
}
extern "C" double atan2(double left, double right) {
  return LIBC_NAMESPACE::shared::atan2(left, right);
}
extern "C" double atan(double value) {
  return LIBC_NAMESPACE::shared::atan(value);
}
extern "C" double asin(double value) {
  return LIBC_NAMESPACE::shared::asin(value);
}
extern "C" double acos(double value) {
  return LIBC_NAMESPACE::shared::acos(value);
}
extern "C" double cbrt(double value) {
  return LIBC_NAMESPACE::shared::cbrt(value);
}
extern "C" double round(double value) {
  return LIBC_NAMESPACE::shared::round(value);
}
extern "C" double fmin(double left, double right) {
  return LIBC_NAMESPACE::shared::fmin(left, right);
}
extern "C" double fmax(double left, double right) {
  return LIBC_NAMESPACE::shared::fmax(left, right);
}
extern "C" double hypot(double left, double right) {
  return LIBC_NAMESPACE::shared::hypot(left, right);
}
extern "C" double expm1(double value) {
  return LIBC_NAMESPACE::shared::expm1(value);
}
extern "C" double log1p(double value) {
  return LIBC_NAMESPACE::shared::log1p(value);
}
extern "C" double exp2(double value) {
  return LIBC_NAMESPACE::shared::exp2(value);
}
extern "C" double remainder(double left, double right) {
  return LIBC_NAMESPACE::shared::remainder(left, right);
}
extern "C" double fma(double left, double right, double addend) {
  return LIBC_NAMESPACE::shared::fma(left, right, addend);
}
extern "C" float fabsf(float value) {
  return LIBC_NAMESPACE::shared::fabsf(value);
}
extern "C" float truncf(float value) {
  return LIBC_NAMESPACE::shared::truncf(value);
}
extern "C" float floorf(float value) {
  return LIBC_NAMESPACE::shared::floorf(value);
}
extern "C" float ceilf(float value) {
  return LIBC_NAMESPACE::shared::ceilf(value);
}
extern "C" float sqrtf(float value) {
  return LIBC_NAMESPACE::shared::sqrtf(value);
}
extern "C" float fmodf(float left, float right) {
  return LIBC_NAMESPACE::shared::fmodf(left, right);
}
extern "C" float log2f(float value) {
  return LIBC_NAMESPACE::shared::log2f(value);
}
extern "C" float logf(float value) {
  return LIBC_NAMESPACE::shared::logf(value);
}
extern "C" float log10f(float value) {
  return LIBC_NAMESPACE::shared::log10f(value);
}
extern "C" float expf(float value) {
  return LIBC_NAMESPACE::shared::expf(value);
}
extern "C" float powf(float left, float right) {
  return LIBC_NAMESPACE::shared::powf(left, right);
}
extern "C" float sinf(float value) {
  return LIBC_NAMESPACE::shared::sinf(value);
}
extern "C" float cosf(float value) {
  return LIBC_NAMESPACE::shared::cosf(value);
}
extern "C" float tanf(float value) {
  return LIBC_NAMESPACE::shared::tanf(value);
}
extern "C" float atan2f(float left, float right) {
  return LIBC_NAMESPACE::shared::atan2f(left, right);
}
extern "C" float atanf(float value) {
  return LIBC_NAMESPACE::shared::atanf(value);
}
extern "C" float asinf(float value) {
  return LIBC_NAMESPACE::shared::asinf(value);
}
extern "C" float acosf(float value) {
  return LIBC_NAMESPACE::shared::acosf(value);
}
extern "C" float cbrtf(float value) {
  return LIBC_NAMESPACE::shared::cbrtf(value);
}
extern "C" float roundf(float value) {
  return LIBC_NAMESPACE::shared::roundf(value);
}
extern "C" float fminf(float left, float right) {
  return LIBC_NAMESPACE::shared::fminf(left, right);
}
extern "C" float fmaxf(float left, float right) {
  return LIBC_NAMESPACE::shared::fmaxf(left, right);
}
extern "C" float hypotf(float left, float right) {
  return LIBC_NAMESPACE::shared::hypotf(left, right);
}
extern "C" float expm1f(float value) {
  return LIBC_NAMESPACE::shared::expm1f(value);
}
extern "C" float log1pf(float value) {
  return LIBC_NAMESPACE::shared::log1pf(value);
}
extern "C" float exp2f(float value) {
  return LIBC_NAMESPACE::shared::exp2f(value);
}
extern "C" float remainderf(float left, float right) {
  return LIBC_NAMESPACE::shared::remainderf(left, right);
}
extern "C" float fmaf(float left, float right, float addend) {
  return LIBC_NAMESPACE::shared::fmaf(left, right, addend);
}
extern "C" float nextafterf(float left, float right) {
  return LIBC_NAMESPACE::shared::nextafterf(left, right);
}
