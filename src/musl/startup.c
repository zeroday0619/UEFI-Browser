#define _GNU_SOURCE
#include "pthread_impl.h"
#include "uefi.h"
#include <stdio.h>
#include <string.h>

static SystemTable *firmware;
static void *thread_keys[PTHREAD_KEYS_MAX];
static size_t auxiliary[] = {6, 4096, 0, 0};
_Static_assert(sizeof(struct pthread) <= 4096,
               "The EFI loader reserves one page for the thread structure.");
SystemTable *platform_firmware(void) { return firmware; }
void graphics_log(const char *text) {
  while (*text) {
    unsigned char state;
    do {
      __asm__ volatile("inb %1,%0" : "=a"(state) : "Nd"((unsigned short)0x3fd));
    } while (!(state & 0x20));
    __asm__ volatile("outb %0,%1" ::"a"((unsigned char)*text++),
                     "Nd"((unsigned short)0x3f8));
  }
}
void musl_initialize(SystemTable *table, void *stack, size_t stack_size) {
  firmware = table;
  pthread_t thread = __pthread_self();
  thread->self = thread;
  thread->prev = thread->next = thread;
  thread->tid = 1;
  thread->locale = &libc.global_locale;
  thread->stack = (char *)stack + stack_size;
  thread->stack_size = stack_size;
  thread->tsd = thread_keys;
  thread->robust_list.head = &thread->robust_list.head;
  libc.page_size = 4096;
  libc.auxv = auxiliary;
  libc.can_do_threads = 0;
}
/* A blocked wait cannot make progress without another schedulable thread. */
_Noreturn void abort(void) {
  graphics_log("FAIL: musl runtime aborted\n");
  for (;;)
    __asm__ volatile("hlt");
}
int pthread_create(pthread_t *thread, const pthread_attr_t *attribute,
                   void *(*entry)(void *), void *argument) {
  (void)thread;
  (void)attribute;
  (void)entry;
  (void)argument;
  return ENOTSUP;
}
long __syscall_cp_c(long number, long first, long second, long third,
                    long fourth, long fifth, long sixth) {
  return firmware_syscall(number, first, second, third, fourth, fifth, sixth);
}

void *__dso_handle;

static struct {
  void (*callback)(void *);
  void *argument;
} thread_destructors[1024];
static size_t thread_destructor_count;
int __cxa_thread_atexit_impl(void (*callback)(void *), void *argument,
                             void *handle) {
  (void)handle;
  if (thread_destructor_count == 1024)
    return -1;
  thread_destructors[thread_destructor_count].callback = callback;
  thread_destructors[thread_destructor_count++].argument = argument;
  return 0;
}
void runtime_finalize_threads(void) {
  /* The only execution thread exits before global C++ objects are destroyed. */
  while (thread_destructor_count) {
    size_t index = --thread_destructor_count;
    thread_destructors[index].callback(thread_destructors[index].argument);
  }
}

int sigaction(int signal_number, const struct sigaction *action,
              struct sigaction *previous) {
  (void)signal_number;
  (void)action;
  (void)previous;
  errno = ENOTSUP;
  return -1;
}

_Noreturn void __restore_rt(void) {
  /* Signal delivery is unsupported, so a Linux signal frame cannot exist. */
  abort();
}
