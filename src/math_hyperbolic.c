#include <math.h>
double sinh(double value) {
  double magnitude = fabs(value);
  double result = magnitude > 22 ? (exp(magnitude / 2) / 2) * exp(magnitude / 2)
                  : magnitude < 0.5 ? (expm1(magnitude) - expm1(-magnitude)) / 2
                                    : (exp(magnitude) - exp(-magnitude)) / 2;
  return signbit(value) ? -result : result;
}
double cosh(double value) {
  double magnitude = fabs(value);
  if (magnitude > 22) {
    double half = exp(magnitude / 2);
    return (half / 2) * half;
  }
  return (exp(value) + exp(-value)) / 2;
}
double tanh(double value) {
  double magnitude = fabs(value);
  double result =
      magnitude > 20 ? 1 : -expm1(-2 * magnitude) / (2 + expm1(-2 * magnitude));
  return signbit(value) ? -result : result;
}
double asinh(double value) {
  double magnitude = fabs(value);
  double result =
      magnitude > 1e150
          ? log(magnitude) + 0.6931471805599453
          : log1p(magnitude + magnitude * magnitude /
                                  (1 + sqrt(1 + magnitude * magnitude)));
  return signbit(value) ? -result : result;
}
double acosh(double value) {
  if (value < 1)
    return NAN;
  if (value > 1e150)
    return log(value) + 0.6931471805599453;
  return log1p(value - 1 + sqrt(value - 1) * sqrt(value + 1));
}
double atanh(double value) { return (log1p(value) - log1p(-value)) / 2; }
