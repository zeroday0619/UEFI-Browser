#include <functional>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
extern "C" void graphics_log(const char *);
static thread_local std::string thread_text;
extern "C" int run_cxx_probe() {
  std::vector<int> values{1, 2, 3, 4};
  std::function<int(int)> square = [](int value) { return value * value; };
  int sum = 0;
  for (int value : values)
    sum += square(value);
  std::unordered_map<std::string, int> dictionary{{"answer", sum}};
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << "value=" << dictionary.at("answer") << ' ' << std::fixed
         << std::setprecision(2) << 1.25;
  thread_text = output.str();
  std::istringstream input("42 3.125");
  int integer;
  double fraction;
  input >> integer >> fraction;
  std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  if (thread_text != "value=30 1.25" || integer != 42 || fraction != 3.125) {
    graphics_log("FAIL: native libc++ runtime\n");
    return 1;
  }
  graphics_log(
      "PASS: native libc++ containers, streams, locale, TLS and mutex\n");
  return 0;
}
