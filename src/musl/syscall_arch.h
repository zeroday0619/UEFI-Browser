/* All C syscall wrappers enter the firmware dispatcher instead of a kernel. */
#define __SYSCALL_LL_E(value) (value)
#define __SYSCALL_LL_O(value) (value)
#define IPC_64 0
long firmware_syscall(long, long, long, long, long, long, long);
static inline long __syscall0(long number) {
  return firmware_syscall(number, 0, 0, 0, 0, 0, 0);
}
static inline long __syscall1(long number, long argument0) {
  return firmware_syscall(number, argument0, 0, 0, 0, 0, 0);
}
static inline long __syscall2(long number, long argument0, long argument1) {
  return firmware_syscall(number, argument0, argument1, 0, 0, 0, 0);
}
static inline long __syscall3(long number, long argument0, long argument1,
                              long argument2) {
  return firmware_syscall(number, argument0, argument1, argument2, 0, 0, 0);
}
static inline long __syscall4(long number, long argument0, long argument1,
                              long argument2, long argument3) {
  return firmware_syscall(number, argument0, argument1, argument2, argument3, 0,
                          0);
}
static inline long __syscall5(long number, long argument0, long argument1,
                              long argument2, long argument3, long argument4) {
  return firmware_syscall(number, argument0, argument1, argument2, argument3,
                          argument4, 0);
}
static inline long __syscall6(long number, long argument0, long argument1,
                              long argument2, long argument3, long argument4,
                              long argument5) {
  return firmware_syscall(number, argument0, argument1, argument2, argument3,
                          argument4, argument5);
}
