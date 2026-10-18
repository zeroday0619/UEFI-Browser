#ifndef PLATFORM_SYS_TIME_H
#define PLATFORM_SYS_TIME_H
struct timeval { long long tv_sec, tv_usec; };
int gettimeofday(struct timeval *, void *);
#endif
