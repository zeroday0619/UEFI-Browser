#define _GNU_SOURCE
#include "platform/runtime.h"
#include "renderer_runtime.h"
#include "firmware_io.h"
#include "firmware_audio.h"
#include <errno.h>
#include <fcntl.h>
#include <fenv.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wchar.h>
extern void musl_initialize(SystemTable *, void *, size_t);
extern void graphics_log(const char *);
extern int run_platform_probe(void);
#ifdef ENABLE_BLINK_COMPAT
extern int run_blink_compatibility_probe(void);
#endif
extern void platform_release_pages(void);
extern void runtime_release_all(void);
#ifdef ENABLE_BLINK
extern int run_blink_browser(SystemTable *);
#endif
#ifdef ENABLE_V8
extern int run_v8_probe(void);
#endif
#ifdef ENABLE_SKIA
extern int run_skia_probe(SystemTable *);
#endif
#ifdef ENABLE_CPP
extern int run_cxx_probe(void);
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
extern void __funcs_on_exit(void);
extern void runtime_finalize_threads(void);
#endif
#ifdef ENABLE_NETWORK
extern int run_network_probe(SystemTable *);
#endif
static int shared_memory_probe(void) {
  extern int platform_memory_is_read_only(const void *);
  int descriptor=syscall(SYS_memfd_create,"probe",3|8);
  if(descriptor<0 || ftruncate(descriptor,8192)) return 1;
  if(fcntl(descriptor,F_ADD_SEALS,F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_SEAL) || ftruncate(descriptor,4096)!=-1 || errno!=EPERM) return 2;
  char path[64];snprintf(path,sizeof(path),"/proc/self/fd/%d",descriptor);
  int readonly=open(path,O_RDONLY|O_CLOEXEC),copy=dup(descriptor);
  if(readonly<0 || copy<0) {printf("SHARED: readonly=%d duplicate=%d errno=%d\n",readonly,copy,errno);return 3;}
  unsigned char *first=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_SHARED,descriptor,0);
  unsigned char *second=mmap(NULL,8192,PROT_READ,MAP_SHARED,readonly,0);
  if(first==MAP_FAILED || second==MAP_FAILED || first==second) return 4;
  first[0]=37;first[8191]=91;
  if(second[0]!=37 || second[8191]!=91 || !platform_memory_is_read_only(second)) return 5;
  if(mprotect(second,8192,PROT_READ|PROT_WRITE)!=-1 || errno!=EACCES || mprotect(first,8192,PROT_EXEC)!=-1 || errno!=EACCES) return 6;
  if(madvise(second,8192,MADV_DONTNEED)!=-1 || errno!=ENOTSUP || second[0]!=37) return 9;
  if(close(descriptor) || close(copy) || close(readonly) || second[8191]!=91) return 7;
  if(munmap(first,8192) || second[0]!=37 || munmap(second,8192)) return 8;
  graphics_log("PASS: shared memory aliases, read-only pages, size seals and descriptor lifetime\n");
  return 0;
}
static _Thread_local unsigned thread_value = 17;
static int checks;
#define CHECK(condition)                                                       \
  do {                                                                         \
    checks++;                                                                  \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL: musl check %d at line %d\n", checks, __LINE__);   \
      return 1;                                                                \
    }                                                                          \
  } while (0)
static pthread_mutex_t thread_probe_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t thread_probe_condition = PTHREAD_COND_INITIALIZER;
static pthread_key_t thread_probe_key;
static unsigned thread_probe_stage, thread_probe_destructors;
static void thread_probe_destructor(void *value) {
  if (value == (void *)37) ++thread_probe_destructors;
}
static void *thread_probe_entry(void *argument) {
  if (thread_value != 17 || pthread_getspecific(thread_probe_key)) return (void *)1;
  thread_value = 99;
  errno = EDOM;
  if (fesetround(FE_DOWNWARD)) return (void *)3;
  feclearexcept(FE_ALL_EXCEPT);
  feraiseexcept(FE_INVALID);
  pthread_setspecific(thread_probe_key, (void *)37);
  pthread_mutex_lock(&thread_probe_mutex);
  thread_probe_stage = 1;
  pthread_cond_signal(&thread_probe_condition);
  while (thread_probe_stage != 2)
    pthread_cond_wait(&thread_probe_condition, &thread_probe_mutex);
  pthread_mutex_unlock(&thread_probe_mutex);
  if (thread_value != 99 || errno != EDOM || fegetround() != FE_DOWNWARD || !(fetestexcept(FE_ALL_EXCEPT) & FE_INVALID)) return (void *)2;
  return argument;
}
static unsigned broadcast_ready;
static int broadcast_release;
static void *thread_broadcast_entry(void *argument) {
  pthread_mutex_lock(&thread_probe_mutex);
  ++broadcast_ready;
  pthread_cond_broadcast(&thread_probe_condition);
  while (!broadcast_release)
    pthread_cond_wait(&thread_probe_condition, &thread_probe_mutex);
  pthread_mutex_unlock(&thread_probe_mutex);
  return argument;
}
static int run_thread_checks(void) {
  pthread_t thread;
  void *result = NULL;
  CHECK(fesetround(FE_TONEAREST) == 0);
  feclearexcept(FE_ALL_EXCEPT);
  feraiseexcept(FE_DIVBYZERO);
  CHECK(pthread_key_create(&thread_probe_key, thread_probe_destructor) == 0);
  CHECK(pthread_create(&thread, NULL, thread_probe_entry, (void *)73) == 0);
  pthread_mutex_lock(&thread_probe_mutex);
  while (!thread_probe_stage)
    CHECK(pthread_cond_wait(&thread_probe_condition, &thread_probe_mutex) == 0);
  CHECK(thread_value == 18 && !pthread_getspecific(thread_probe_key));
  thread_probe_stage = 2;
  CHECK(pthread_cond_signal(&thread_probe_condition) == 0);
  CHECK(pthread_mutex_unlock(&thread_probe_mutex) == 0);
  CHECK(pthread_join(thread, &result) == 0 && result == (void *)73);
  CHECK(thread_probe_destructors == 1 && fegetround() == FE_TONEAREST &&
        (fetestexcept(FE_ALL_EXCEPT) & (FE_INVALID | FE_DIVBYZERO)) == FE_DIVBYZERO);
  CHECK(pthread_key_delete(thread_probe_key) == 0);
  feclearexcept(FE_ALL_EXCEPT);
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_nsec += 10000000;
  if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
  CHECK(pthread_mutex_lock(&thread_probe_mutex) == 0);
  CHECK(pthread_cond_timedwait(&thread_probe_condition, &thread_probe_mutex, &deadline) == ETIMEDOUT);
  CHECK(pthread_mutex_unlock(&thread_probe_mutex) == 0);
  pthread_t peers[3];
  for (unsigned index = 0; index < 3; ++index)
    CHECK(pthread_create(&peers[index], NULL, thread_broadcast_entry, (void *)(uintptr_t)(index + 100)) == 0);
  CHECK(pthread_mutex_lock(&thread_probe_mutex) == 0);
  while (broadcast_ready < 3)
    CHECK(pthread_cond_wait(&thread_probe_condition, &thread_probe_mutex) == 0);
  broadcast_release = 1;
  CHECK(pthread_cond_broadcast(&thread_probe_condition) == 0);
  CHECK(pthread_mutex_unlock(&thread_probe_mutex) == 0);
  for (unsigned index = 0; index < 3; ++index)
    CHECK(pthread_join(peers[index], &result) == 0 && result == (void *)(uintptr_t)(index + 100));
  graphics_log("PASS: firmware threads, independent TLS, condition wake/broadcast, timed wait, join, FP state and TSD destructors\n");
  return 0;
}
static int run_checks(void) {
  CHECK(thread_value++ == 17 && thread_value == 18);
  CHECK(run_platform_probe() == 0);
#ifdef ENABLE_BLINK_COMPAT
  CHECK(run_blink_compatibility_probe() == 0);
#endif
  char buffer[128];
  CHECK(snprintf(buffer, 4, "%s", "abcdef") == 6 && !strcmp(buffer, "abc"));
  CHECK(snprintf(buffer, sizeof(buffer), "%.3f %lld %s", 1.25,
                 9223372036854775807LL, "musl") == 30);
  CHECK(!strcmp(buffer, "1.250 9223372036854775807 musl"));
  double number = 0;
  long long integer = 0;
  CHECK(sscanf("3.125 -9223372036854775807", "%lf %lld", &number, &integer) ==
        2);
  CHECK(number == 3.125 && integer == -9223372036854775807LL);
  char *end;
  CHECK(strtod("0x1.8p+2!", &end) == 6.0 && *end == '!');
  errno = 0;
  CHECK(strtoull("18446744073709551616", &end, 10) == ~0ULL && errno == ERANGE);
  CHECK(setlocale(LC_ALL, "C.UTF-8") != NULL);
  const char *text = "한글 العربية 日本語 😀";
  wchar_t wide[64];
  size_t count = mbstowcs(wide, text, 64);
  CHECK(count != (size_t)-1 && count > 10);
  CHECK(wcstombs(buffer, wide, sizeof(buffer)) == strlen(text));
  CHECK(!strcmp(buffer, text));
  mbstate_t state = {0};
  wchar_t character;
  CHECK(mbrtowc(&character, "\xed", 1, &state) == (size_t)-2);
  CHECK(mbrtowc(&character, "\x95\x9c", 2, &state) == 2 && character == 0xd55c);
  memset(&state, 0, sizeof(state));
  errno = 0;
  CHECK(mbrtowc(&character, "\xc0\xaf", 2, &state) == (size_t)-1 &&
        errno == EILSEQ);
  locale_t locale = newlocale(LC_ALL_MASK, "C", NULL);
  CHECK(locale != NULL);
  locale_t previous = uselocale(locale);
  CHECK(previous != NULL && MB_CUR_MAX == 1);
  CHECK(uselocale(previous) == locale);
  freelocale(locale);
  char *memory = NULL;
  size_t length = 0;
  FILE *stream = open_memstream(&memory, &length);
  CHECK(stream != NULL);
  CHECK(fprintf(stream, "%s %.2f", "buffer", 42.5) == 12);
  CHECK(fclose(stream) == 0 && length == 12 && !strcmp(memory, "buffer 42.50"));
  free(memory);
  pthread_key_t key;
  CHECK(pthread_key_create(&key, NULL) == 0);
  CHECK(pthread_setspecific(key, &thread_value) == 0);
  CHECK(pthread_getspecific(key) == &thread_value);
  CHECK(pthread_key_delete(key) == 0);
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  CHECK(pthread_mutex_lock(&mutex) == 0);
  CHECK(pthread_mutex_trylock(&mutex) == EBUSY);
  CHECK(pthread_mutex_unlock(&mutex) == 0);
  CHECK(pthread_mutex_destroy(&mutex) == 0);
  pthread_attr_t attribute;
  CHECK(pthread_getattr_np(pthread_self(), &attribute) == 0);
  void *stack;
  size_t stack_size;
  CHECK(pthread_attr_getstack(&attribute, &stack, &stack_size) == 0 && stack &&
        stack_size == 4 * 1024 * 1024);
  CHECK(pthread_attr_destroy(&attribute) == 0);
  pthread_t unused_thread;
  CHECK(pthread_create(&unused_thread, NULL, NULL, NULL) == EINVAL);
  CHECK(run_thread_checks() == 0);
  void *pages = (void *)syscall(SYS_mmap, NULL, 4096L, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1L, 0L);
  CHECK(pages != MAP_FAILED);
  memset(pages, 0x5a, 4096);
  CHECK(syscall(SYS_madvise, pages, 4096L, MADV_DONTNEED) == 0 &&
        !*(unsigned char *)pages);
  CHECK(syscall(SYS_munmap, pages, 4096L) == 0);
  unsigned char random[32];
  CHECK(getrandom(random, sizeof(random), 0) == sizeof(random));
  errno = 0;
  CHECK(getrandom(random, sizeof(random), 0x80000000u) == -1 &&
        errno == EINVAL);
  errno = 0;
  CHECK(fopen("/unsupported-file", "r") == NULL && errno == EACCES);
  errno = 0;
  CHECK(syscall(0x7fffffff) == -1 && errno == ENOSYS);
  CHECK(fabsl(expm1l(0.5L) - 0.64872127070012814684865L) < 1e-18L);
  CHECK(fabs(sin(0.5) - 0.479425538604203) < 1e-15);
  CHECK(printf("PASS: musl stdio, UTF-8, locale, TLS, mutex, RNG and math (%d "
               "checks)\n",
               checks + 1) > 0);
  CHECK(fflush(NULL) == 0);
  return 0;
}
static void console_message(SystemTable *table, const char *text) {
  Character buffer[256];
  size_t count = 0;
  while (*text && count < 255)
    buffer[count++] = (unsigned char)*text++;
  buffer[count] = 0;
  table->output->write(table->output, buffer);
}
int engine_entry(Handle image, SystemTable *table, void *stack, RendererBootstrap *bootstrap) {
  (void)image;
  renderer_runtime_initialize(bootstrap);
  table = renderer_runtime_table(table);
  musl_initialize(table, stack, 4 * 1024 * 1024);
  runtime_initialize(table);
  firmware_file_initialize(image);
  if (!renderer_runtime_is_child()) table->output->clear(table->output);
  if (!renderer_runtime_is_child()) console_message(table, "UEFI BROWSER\r\n\r\nRunning musl checks...\r\n");
#ifdef ENABLE_CPP
  for (void (**callback)(void) = __init_array_start;
       callback < __init_array_end; callback++)
    (*callback)();
#endif
  int result = run_checks();
  if(!result) {result=shared_memory_probe();if(result) printf("FAIL: shared memory probe %d\n",result);}
#ifdef ENABLE_SERVICE_PROBE
  extern int run_firmware_io_probe(void);
  if (!result) result = run_firmware_io_probe();
#endif
#if defined(ENABLE_CPP) && !defined(ENABLE_BLINK)
  if (!result)
    result = run_cxx_probe();
#endif
#ifdef ENABLE_BLINK
  if (!result)
    result = run_blink_browser(table);
#endif
#ifdef ENABLE_V8
  if (!result) {
    console_message(table, "Running native V8 JavaScript...\r\n");
    result = run_v8_probe();
    console_message(table, result ? "V8 execution failed.\r\n"
                                  : "V8 JavaScript passed.\r\n");
  }
#endif
#ifdef ENABLE_CPP
#ifdef ENABLE_SKIA
  if (!result)
    result = run_skia_probe(table);
#endif
  runtime_finalize_threads();
  __funcs_on_exit();
#endif
#ifdef ENABLE_NETWORK
  if (!result)
    result = run_network_probe(table);
#endif
  if (!renderer_runtime_is_child()) { firmware_file_reset(0); firmware_socket_reset(0); firmware_audio_reset(0); }
  fflush(NULL);
  platform_release_pages();
  runtime_release_all();
  if (runtime_memory_used())
    result = 1;
  if (!result)
    graphics_log("PASS: engine teardown and pool cleanup\n");
#ifdef ENABLE_BLINK
  if (renderer_runtime_is_child()) return result;
  const char *scope = "Blink browser session ended.";
#elif defined(ENABLE_V8)
  const char *scope =
      "V8 and Skia runtime validation. Blink is not integrated yet.";
#else
  const char *scope =
      "C/C++ runtime validation. The browser is not integrated yet.";
#endif
  char summary[256];
  snprintf(summary, sizeof(summary),
           "\r\n%s: %d runtime checks reached.\r\n"
           "%s\r\n"
           "\r\nTests finished. Press any key to return to firmware.\r\n",
           result ? "FAIL" : "PASS", checks, scope);
  console_message(table, summary);
  Key key;
  while (table->input->read(table->input, &key) == ERROR(6))
    ((Status(UEFI_API *)(size_t))table->boot->function[28])(50000);
  return result;
}
