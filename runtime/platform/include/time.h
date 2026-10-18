#ifndef PLATFORM_TIME_H
#define PLATFORM_TIME_H
typedef long long time_t;
struct tm {
  int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday,
      tm_isdst;
};
#endif
