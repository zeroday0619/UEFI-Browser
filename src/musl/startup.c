#define _GNU_SOURCE
#include "pthread_impl.h"
#include "uefi.h"
#include "renderer_runtime.h"
extern void renderer_runtime_log(const char *);
#include <stdio.h>
#include <string.h>

#ifndef ENABLE_BLINK
const int firmware_task_diagnostics = 0;
#endif

static SystemTable *firmware;
static char recent_log[4096];
static size_t recent_position, recent_length;
static unsigned panic_active;
static void panic_text(const char *text) {
  Character buffer[128];
  size_t length=0;
  while (*text) {
    unsigned char character=*text++;
    if (character=='\n') buffer[length++]='\r';
    buffer[length++]=(character>=32 && character<127) || character=='\n' || character=='\r' ? character : ' ';
    if (length>=125 || !*text) {
      buffer[length]=0;
      firmware->output->write(firmware->output,buffer);
      length=0;
    }
  }
}
static void panic_number(const char *name,uint64_t value) {
  char text[19]="0x";
  static const char digits[]="0123456789abcdef";
  for (unsigned index=0;index<16;++index) text[2+index]=digits[(value>>((15-index)*4))&15];
  text[18]=0;
  panic_text(name); panic_text(text); panic_text("\n");
}
static _Noreturn void runtime_panic(const char *reason,uintptr_t caller) {
  /* This path avoids the C heap, stdio locks and browser rendering after a fatal abort. */
  if (!panic_active++ && firmware && firmware->output) {
    Output *output=firmware->output;
    if (output->set_attribute) ((Status(UEFI_API *)(Output *,size_t))output->set_attribute)(output,0x1f);
    if (output->clear) output->clear(output);
    if (output->enable_cursor) ((Status(UEFI_API *)(Output *,uint8_t))output->enable_cursor)(output,0);
    panic_text("UEFI BROWSER - FATAL RUNTIME ERROR\n\n");
    panic_text(reason); panic_text("\nExecution has stopped. Reset the machine to restart.\n\n");
    uint64_t stack,root,address,flags;
    __asm__ volatile("mov %%rsp,%0; mov %%cr3,%1; mov %%cr2,%2; pushfq; popq %3"
        : "=r"(stack),"=r"(root),"=r"(address),"=r"(flags));
    panic_number("CALLER ",caller); panic_number("RSP    ",stack);
    panic_number("CR3    ",root); panic_number("CR2    ",address); panic_number("RFLAGS ",flags);
    panic_text("\nRecent diagnostic output:\n");
    /* Bound output to the last twelve lines so the fatal header remains visible. */
    size_t count=0,lines=0;
    while (count<recent_length) {
      char character=recent_log[(recent_position+sizeof(recent_log)-1-count)%sizeof(recent_log)];
      if (character=='\n' && ++lines>12) break;
      ++count;
    }
    char line[81];
    size_t used=0;
    for (size_t index=0;index<count;++index) {
      char character=recent_log[(recent_position+sizeof(recent_log)-count+index)%sizeof(recent_log)];
      if (character=='\n' || used<70) line[used++]=character;
      if (character=='\n' || index+1==count) {
        line[used]=0; panic_text(line); used=0;
      }
    }
  }
  for (;;) __asm__ volatile("cli; hlt");
}
static _Thread_local void *thread_keys[PTHREAD_KEYS_MAX];
static size_t auxiliary[] = {6, 4096, 0, 0};
_Static_assert(sizeof(struct pthread) <= 4096,
               "The EFI loader reserves one page for the thread structure.");
SystemTable *platform_firmware(void) { return firmware; }
void graphics_log(const char *text) {
  if (renderer_runtime_is_child()) { renderer_runtime_log(text); return; }
  while (*text) {
    recent_log[recent_position++]=*text;
    recent_position%=sizeof(recent_log);
    if (recent_length<sizeof(recent_log)) ++recent_length;
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
  /* The EFI entrypoint replaces musl CRT startup, including its program identity. */
  extern char *__progname, *__progname_full;
  static char application_name[] = "uefi-browser";
  __progname = __progname_full = application_name;
  pthread_t thread = __pthread_self();
  thread->self = thread;
  thread->prev = thread->next = thread;
  thread->tid = renderer_runtime_is_child() ? 2 : 1;
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
#ifdef ENABLE_VENUS_RUNTIME
  extern void firmware_venus_dump(void);
  firmware_venus_dump();
#endif
  /* The direct caller remains useful when optimized C frames cannot be unwound. */
  char caller[19] = "0x";
  uintptr_t return_address = (uintptr_t)__builtin_return_address(0);
  static const char digits[] = "0123456789abcdef";
  for (unsigned index = 0; index < 16; ++index)
    caller[2 + index] = digits[(return_address >> ((15 - index) * 4)) & 15];
  caller[18] = 0;
  graphics_log("RUNTIME: abort caller=");
  graphics_log(caller);
  graphics_log("\n");
#ifdef ENABLE_BLINK_COMPAT
  extern size_t firmware_collect_stack_trace(const void **, size_t);
  const void *addresses[24];
  size_t count = firmware_collect_stack_trace(addresses, 24);
  for (size_t index = 0; index < count; ++index) {
    char message[64];
    snprintf(message, sizeof(message), "RUNTIME: abort frame=%p\n", addresses[index]);
    graphics_log(message);
  }
#endif
  if (renderer_runtime_is_child()) {
    graphics_log("RENDERER: runtime abort\n");
    __builtin_trap();
  }
  graphics_log("FAIL: musl runtime aborted\n");
  runtime_panic("The privileged runtime called abort().",(uintptr_t)__builtin_return_address(0));
}
long __syscall_cp_c(long number, long first, long second, long third,
                    long fourth, long fifth, long sixth) {
  return firmware_syscall(number, first, second, third, fourth, fifth, sixth);
}

void *__dso_handle;

static _Thread_local struct {
  void (*callback)(void *);
  void *argument;
} thread_destructors[1024];
static _Thread_local size_t thread_destructor_count;
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
  /* Each execution context owns its C++ thread-local destructors. */
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
