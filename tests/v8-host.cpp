#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <sys/random.h>
extern "C" int run_v8_probe();
extern "C" void graphics_log(const char *text) { std::fputs(text, stdout); }
extern "C" bool firmware_entropy(unsigned char *buffer, size_t size) {
  while (size) {
    ssize_t count = getrandom(buffer, size, 0);
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (count == 0)
      return false;
    buffer += count;
    size -= count;
  }
  return true;
}
int main() { return run_v8_probe(); }
