#include "japanese_input.h"
#include "base/system_util.h"
#include "config/config_handler.h"
#include <cstdlib>
#include <iostream>
using namespace uefi_browser;
static void Require(bool condition, const char* message) {
  if (!condition) { std::cerr << message << '\n'; std::abort(); }
}
static JapaneseUpdate Type(JapaneseInput& input, const char* text) {
  JapaneseUpdate update;
  for (; *text; ++text) {
    update = input.Feed(JapaneseKey::Character, *text);
    Require(update.error.empty() && update.handled, "Character input failed");
  }
  return update;
}
static int Selected(const JapaneseUpdate& update) {
  for (const auto& candidate : update.candidates)
    if (candidate.selected) return candidate.id;
  return -1;
}
int main(int count, char** arguments) {
  Require(count == 2, "A temporary profile directory is required");
  mozc::SystemUtil::SetUserProfileDirectory(arguments[1]);
  JapaneseInput input;
  Require(Type(input,"nihongo").preedit == "にほんご", "Romaji composition failed");
  auto config = mozc::config::ConfigHandler::GetCopiedConfig();
  Require(config.incognito_mode() && config.history_learning_level() == mozc::config::Config::NO_HISTORY,
          "Browser text history learning was not disabled");
  auto converted = input.Feed(JapaneseKey::Space);
  Require(converted.preedit == "日本語", "Dictionary conversion failed");
  Require(input.Finish().committed == "日本語", "Conversion commit failed");
  Require(Type(input,"kanji").preedit == "かんじ", "Second composition failed");
  Require(input.Feed(JapaneseKey::Space).preedit == "感じ",
          "Pinned dictionary first candidate for kanji changed");
  auto choices = input.Feed(JapaneseKey::Space);
  Require(!choices.candidates.empty(), "Candidate window is empty");
  Require(Selected(choices) >= 0, "Candidate focus is missing");
  auto moved = input.Feed(JapaneseKey::Down);
  Require(Selected(moved) >= 0 && Selected(moved) != Selected(choices),
          "Candidate down-key selection did not move");
  Require(Selected(input.Feed(JapaneseKey::Up)) == Selected(choices),
          "Candidate up-key selection did not return");
  int selected = choices.candidates.front().id;
  auto choice = choices.candidates.front().text;
  auto selection = input.Select(selected);
  Require(selection.error.empty() && (selection.committed == choice || selection.preedit == choice), "Candidate selection failed");
  input.Clear();
  Require(Type(input,"kana").preedit == "かな", "Reset failed");
  Require(input.Feed(JapaneseKey::Backspace).preedit == "か", "Backspace failed");
  input.Clear();
  Require(input.Finish().committed.empty(), "Revert retained composition");
  Type(input,"nihon");
  Require(input.Finish().committed == "にほｎ", "Incomplete romaji did not match upstream behavior");
  JapaneseInput other;
  Type(input,"nihonn"); Type(other,"tokyo");
  auto first = input.Finish();
  Require(first.committed == "にほん", "Session isolation failed");
  Require(other.Finish().committed == "ときょ", "Independent session was modified");
  std::cout << "PASS: Mozc romaji, dictionary conversion, candidates, selection, commit, revert, deletion and independent sessions\n";
}
