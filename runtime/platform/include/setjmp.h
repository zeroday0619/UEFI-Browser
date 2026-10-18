#ifndef PLATFORM_SETJMP_H
#define PLATFORM_SETJMP_H
/* The buffer preserves all nonvolatile Windows x64 registers and FP controls. */
typedef unsigned long long jmp_buf[32];
int browser_setjmp(jmp_buf) __attribute__((returns_twice));
void browser_longjmp(jmp_buf,int) __attribute__((noreturn));
#define setjmp(buffer) browser_setjmp(buffer)
#define longjmp(buffer,value) browser_longjmp(buffer,value)
#endif
