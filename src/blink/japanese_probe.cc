#include "japanese_input.h"
#include <cstdio>
extern "C" int run_mozc_probe(void) {
  using namespace uefi_browser;
  JapaneseInput input;
  JapaneseUpdate update;
  std::puts("MOZC PROBE: starting in-process dictionary session");
  std::fflush(stdout);
  for (char key : std::string("nihongo")) {
    update = input.Feed(JapaneseKey::Character, key);
    if (!update.error.empty() || !update.handled) {
      std::printf("FAIL: Mozc input: %s\n", update.error.c_str()); return 1;
    }
  }
  if (update.preedit != "にほんご") { std::printf("FAIL: Mozc preedit %s\n",update.preedit.c_str()); return 2; }
  update = input.Feed(JapaneseKey::Space);
  if (update.preedit != "日本語") { std::printf("FAIL: Mozc conversion %s\n",update.preedit.c_str()); return 3; }
  update = input.Feed(JapaneseKey::Space);
  if (update.candidates.empty()) { std::puts("FAIL: Mozc candidate window"); return 4; }
  bool found = false;
  for (const auto& candidate : update.candidates) {
    if (candidate.text == "日本語") {
      update = input.Select(candidate.id); found = true; break;
    }
  }
  if (!found || (!update.committed.empty() && update.committed != "日本語")) {
    std::puts("FAIL: Mozc candidate selection"); return 5;
  }
  if (update.committed.empty()) update = input.Finish();
  if (update.committed != "日本語") { std::printf("FAIL: Mozc commit %s\n",update.committed.c_str()); return 6; }
  unsigned short selector;
  __asm__ volatile("mov %%cs,%0" : "=r"(selector));
  std::printf("PASS: Mozc firmware romaji, dictionary conversion, candidate selection and commit 日本語 cpl=%u\n",
              selector & 3);
  std::fflush(stdout);
  return 0;
}
