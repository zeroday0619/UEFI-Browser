#define _GNU_SOURCE
#include "firmware_threads.h"
#include "pthread_impl.h"
#include "uefi.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Cooperative contexts never switch while an allocator or firmware call runs.
 */
enum { THREAD_LIMIT = 10, THREAD_STACK = 4 * 1024 * 1024 };
typedef struct {
  uint64_t stack, registers[6], reserved;
  unsigned char floating_point[512] __attribute__((aligned(16)));
  uint64_t tls;
} ThreadContext;
_Static_assert(offsetof(ThreadContext, floating_point) == 64 &&
                   offsetof(ThreadContext, tls) == 576,
               "The context matches the assembly ABI.");
typedef struct {
  ThreadContext context;
  pthread_t thread;
  void *allocation, *stack;
  void *(*entry)(void *);
  void *argument, *result;
  void *keys[PTHREAD_KEYS_MAX];
  char name[16];
  int *wait_address;
  uint64_t deadline;
  unsigned wait_mask;
  int alive, detached, claimed, timed_out;
} FirmwareThread;
static FirmwareThread threads[THREAD_LIMIT];
static unsigned current;
static int next_identifier = 10;
extern unsigned char __thread_template_start[], __thread_template_data_end[];
extern unsigned char __thread_template_end[];
extern SystemTable *platform_firmware(void);
extern void runtime_finalize_threads(void);
extern void firmware_thread_switch(ThreadContext *, ThreadContext *);

static uint64_t now(void) {
  struct timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}
static void initialize(void) {
  if (threads[0].thread)
    return;
  threads[0].thread = __pthread_self();
  threads[0].alive = 1;
}
static void reap(unsigned index) {
  FirmwareThread *thread = &threads[index];
  free(thread->allocation);
  free(thread->stack);
  memset(thread, 0, sizeof(*thread));
}
unsigned firmware_threads_available(void) {
  initialize();
  unsigned available = 0;
  for (unsigned index = 1; index < THREAD_LIMIT; ++index) {
    if (index != current && threads[index].thread && !threads[index].alive &&
        threads[index].detached)
      reap(index);
    if (!threads[index].thread)
      ++available;
  }
  return available;
}
void firmware_threads_yield(void) {
  initialize();
  unsigned previous = current;
  for (;;) {
    uint64_t timestamp = now();
    for (unsigned offset = 1; offset <= THREAD_LIMIT; ++offset) {
      unsigned index = (previous + offset) % THREAD_LIMIT;
      FirmwareThread *candidate = &threads[index];
      if (index && index != previous && candidate->thread &&
          !candidate->alive && candidate->detached) {
        reap(index);
        continue;
      }
      if (!candidate->alive)
        continue;
      if (candidate->deadline && timestamp >= candidate->deadline) {
        candidate->wait_address = NULL;
        candidate->deadline = 0;
        candidate->timed_out = 1;
      }
      if (candidate->wait_address || candidate->deadline)
        continue;
      if (index == previous)
        return;
      current = index;
      firmware_thread_switch(&threads[previous].context, &candidate->context);
      return;
    }
    /* A bounded wait permits timeout progress when every context is asleep. */
    ((Status(UEFI_API *)(size_t))platform_firmware()->boot->function[28])(100);
  }
}
int firmware_thread_identifier(void) { return __pthread_self()->tid; }
long firmware_thread_name(int operation, char *name) {
  if (!name)
    return -EFAULT;
  initialize();
  if (operation == 15) {
    size_t length = strnlen(name, 15);
    memset(threads[current].name, 0, 16);
    memcpy(threads[current].name, name, length);
  } else if (operation == 16)
    memcpy(name, threads[current].name, 16);
  else
    return -EINVAL;
  return 0;
}
static _Noreturn void start(void) {
  FirmwareThread *thread = &threads[current];
  pthread_exit(thread->entry(thread->argument));
}
int pthread_create(pthread_t *output, const pthread_attr_t *attribute,
                   void *(*entry)(void *), void *argument) {
  if (!output || !entry)
    return EINVAL;
  initialize();
  size_t stack_size = THREAD_STACK;
  if (attribute) {
    if (attribute->_a_stackaddr || attribute->_a_sched)
      return ENOTSUP;
    if (attribute->_a_stacksize)
      stack_size = attribute->_a_stacksize;
  }
  if (stack_size < PTHREAD_STACK_MIN || stack_size > 16 * 1024 * 1024)
    return EINVAL;
  unsigned index;
  for (index = 1; index < THREAD_LIMIT && threads[index].thread; ++index) {
  }
  if (index == THREAD_LIMIT || next_identifier == INT_MAX)
    return EAGAIN;
  size_t tls_size = __thread_template_end - __thread_template_start;
  FirmwareThread *thread = &threads[index];
  thread->allocation = aligned_alloc(4096, tls_size + 4096);
  thread->stack = aligned_alloc(16, stack_size);
  if (!thread->allocation || !thread->stack) {
    reap(index);
    return EAGAIN;
  }
  memset(thread->allocation, 0, tls_size + 4096);
  memcpy(thread->allocation, __thread_template_start,
         __thread_template_data_end - __thread_template_start);
  pthread_t native = (void *)((char *)thread->allocation + tls_size);
  thread->thread = native;
  native->self = native;
  native->tid = next_identifier++;
  native->locale = &libc.global_locale;
  native->stack = (char *)thread->stack + stack_size;
  native->stack_size = stack_size;
  native->tsd = thread->keys;
  native->robust_list.head = &native->robust_list.head;
  native->detach_state =
      attribute && attribute->_a_detach ? DT_DETACHED : DT_JOINABLE;
  native->next = __pthread_self()->next;
  native->prev = __pthread_self();
  native->next->prev = native;
  native->prev->next = native;
  thread->detached = native->detach_state == DT_DETACHED;
  thread->entry = entry;
  thread->argument = argument;
  thread->alive = 1;
  uintptr_t top = (uintptr_t)native->stack & ~(uintptr_t)15;
  thread->context.stack = top - 16;
  *(uintptr_t *)(top - 16) = (uintptr_t)start;
  thread->context.tls = (uintptr_t)native;
  __asm__ volatile("fxsave64 %0" : "=m"(thread->context.floating_point));
  libc.can_do_threads = libc.need_locks = 1;
  libc.threads_minus_1++;
  *output = native;
  return 0;
}
_Noreturn void pthread_exit(void *result) {
  if (!current)
    abort();
  pthread_t native = __pthread_self();
  while (native->cancelbuf) {
    struct __ptcb *cleanup = native->cancelbuf;
    native->cancelbuf = cleanup->__next;
    cleanup->__f(cleanup->__x);
  }
  runtime_finalize_threads();
  __pthread_tsd_run_dtors();
  native->prev->next = native->next;
  native->next->prev = native->prev;
  native->next = native->prev = native;
  libc.threads_minus_1--;
  threads[current].result = result;
  threads[current].alive = 0;
  firmware_threads_yield();
  abort();
}
int pthread_join(pthread_t native, void **result) {
  initialize();
  if (native == __pthread_self())
    return EDEADLK;
  unsigned index;
  for (index = 1; index < THREAD_LIMIT && threads[index].thread != native;
       ++index) {
  }
  if (index == THREAD_LIMIT)
    return ESRCH;
  FirmwareThread *thread = &threads[index];
  if (thread->detached || thread->claimed)
    return EINVAL;
  thread->claimed = 1;
  while (thread->alive)
    firmware_threads_yield();
  if (result)
    *result = thread->result;
  reap(index);
  return 0;
}
int pthread_detach(pthread_t native) {
  initialize();
  for (unsigned index = 1; index < THREAD_LIMIT; ++index) {
    FirmwareThread *thread = &threads[index];
    if (thread->thread != native)
      continue;
    if (thread->detached || thread->claimed)
      return EINVAL;
    thread->detached = 1;
    native->detach_state = DT_DETACHED;
    if (!thread->alive)
      reap(index);
    return 0;
  }
  return ESRCH;
}
long firmware_threads_futex(int *address, int operation, int expected,
                            const struct timespec *timeout, int *destination,
                            unsigned mask) {
  if (!address || ((uintptr_t)address & 3))
    return -EINVAL;
  initialize();
  unsigned command = operation & 127;
  if (operation & ~(127 | 128 | 256))
    return -EINVAL;
  if (command == 3 || command == 4) {
    uintptr_t requeue_count = (uintptr_t)timeout;
    if (!destination || ((uintptr_t)destination & 3) || expected < 0 ||
        requeue_count > INT_MAX || (operation & 256))
      return -EINVAL;
    if (command == 4 && __atomic_load_n(address, __ATOMIC_ACQUIRE) != (int)mask)
      return -EAGAIN;
    int woken = 0;
    uintptr_t moved = 0;
    for (unsigned index = 0; index < THREAD_LIMIT; ++index) {
      FirmwareThread *thread = &threads[index];
      if (thread->wait_address != address)
        continue;
      if (woken < expected) {
        thread->wait_address = NULL;
        thread->deadline = 0;
        ++woken;
      } else if (moved < requeue_count) {
        thread->wait_address = destination;
        ++moved;
      }
    }
    return woken + moved;
  }
  if (command == 1 || command == 10) {
    if (expected < 0 || (command == 10 && !mask))
      return -EINVAL;
    int count = 0;
    for (unsigned index = 0; index < THREAD_LIMIT && count < expected;
         ++index) {
      FirmwareThread *thread = &threads[index];
      if (thread->wait_address != address ||
          (command == 10 && !(thread->wait_mask & mask)))
        continue;
      thread->wait_address = NULL;
      thread->deadline = 0;
      ++count;
    }
    return count;
  }
  if (command != 0 && command != 9)
    return -ENOSYS;
  if ((command == 9 && !mask) || (command == 0 && (operation & 256)))
    return -EINVAL;
  if (__atomic_load_n(address, __ATOMIC_ACQUIRE) != expected)
    return -EAGAIN;
  uint64_t deadline = 0;
  if (timeout) {
    if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
        timeout->tv_nsec >= 1000000000 ||
        (uint64_t)timeout->tv_sec > UINT64_MAX / 1000000000 - 1)
      return -EINVAL;
    uint64_t duration =
        (uint64_t)timeout->tv_sec * 1000000000 + timeout->tv_nsec;
    uint64_t timestamp = now();
    if (command == 9) {
      struct timespec clock;
      clock_gettime(operation & 256 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &clock);
      uint64_t current_time =
          (uint64_t)clock.tv_sec * 1000000000 + clock.tv_nsec;
      if (duration <= current_time)
        return -ETIMEDOUT;
      duration -= current_time;
    }
    if (!duration)
      return -ETIMEDOUT;
    deadline =
        duration > UINT64_MAX - timestamp ? UINT64_MAX : timestamp + duration;
  }
  FirmwareThread *thread = &threads[current];
  thread->wait_address = address;
  thread->wait_mask = command == 9 ? mask : ~0u;
  thread->deadline = deadline;
  thread->timed_out = 0;
  firmware_threads_yield();
  return thread->timed_out ? -ETIMEDOUT : 0;
}
