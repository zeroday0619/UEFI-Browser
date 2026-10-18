#include <cstdlib>
#include <new>
void *operator new(std::size_t size) {
  void *pointer = malloc(size);
  if (!pointer)
    abort();
  return pointer;
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *pointer) noexcept { free(pointer); }
void operator delete[](void *pointer) noexcept { free(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { free(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept { free(pointer); }
namespace std {
void __throw_out_of_range_fmt(const char *, ...) { abort(); }
void __throw_bad_alloc() { abort(); }
void __throw_length_error(const char *) { abort(); }
void __throw_logic_error(const char *) { abort(); }
void __throw_out_of_range(const char *) { abort(); }
void __throw_bad_array_new_length() { abort(); }
void __throw_bad_function_call() { abort(); }
} // namespace std
extern "C" void __cxa_pure_virtual() { abort(); }

#include "include/private/SkLog.h"
#include <cstdarg>
#include <cstdio>
extern "C" void graphics_log(const char *);
void SkLogVAList(SkLogPriority, const char *format, va_list arguments) {
  char buffer[512];
  vsnprintf(buffer, sizeof(buffer), format, arguments);
  graphics_log(buffer);
}

namespace std {
void __throw_bad_cast() { abort(); }
void __throw_bad_typeid() { abort(); }
void __throw_bad_exception() { abort(); }
void __throw_range_error(const char *) { abort(); }
void __throw_domain_error(const char *) { abort(); }
void __throw_runtime_error(const char *) { abort(); }
void __throw_overflow_error(const char *) { abort(); }
void __throw_underflow_error(const char *) { abort(); }
void __throw_invalid_argument(const char *) { abort(); }

void __throw_ios_failure(const char *) { abort(); }
void __throw_ios_failure(const char *, int) { abort(); }
} // namespace std

extern "C" void __cxa_throw(void *, void *, void (*)(void *)) { abort(); }
extern "C" void *__cxa_allocate_exception(std::size_t) { abort(); }
extern "C" void __cxa_free_exception(void *) { abort(); }
extern "C" void *__cxa_begin_catch(void *) { abort(); }
extern "C" void __cxa_end_catch() { abort(); }
extern "C" void __cxa_rethrow() { abort(); }
extern "C" void __gxx_personality_v0() { abort(); }
extern "C" void _Unwind_Resume(void *) { abort(); }

extern "C" void __cxa_call_unexpected(void *) { abort(); }

extern "C" void *__cxa_get_exception_ptr(void *) { abort(); }
namespace std {
bool uncaught_exception() noexcept { return false; }
int uncaught_exceptions() noexcept { return 0; }
}
