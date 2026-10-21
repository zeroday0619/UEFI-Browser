#include "japanese_input.h"
#include "base/system_util.h"
#include "config/config_handler.h"
#include "data_manager/oss/oss_data_manager.h"
#include "engine/engine.h"
#include "protocol/commands.pb.h"
#include "session/session_handler.h"
namespace uefi_browser {
class JapaneseInput::State {
 public:
  bool Initialize() {
    if (handler_) return true;
#if defined(UEFI_MOZC)
    // Firmware has no Unix user database; all persistent files use the brokered profile.
    mozc::SystemUtil::SetUserProfileDirectory("/profile/mozc");
#endif
    // CREATE_SESSION does not consume input.config; the engine reads ConfigHandler.
    auto config = mozc::config::ConfigHandler::GetCopiedConfig();
    config.set_incognito_mode(true);
    config.set_history_learning_level(mozc::config::Config::NO_HISTORY);
    mozc::config::ConfigHandler::SetConfig(std::move(config));
    auto engine = mozc::Engine::CreateEngine(std::make_unique<const mozc::oss::OssDataManager>());
    if (!engine.ok()) { error_ = std::string(engine.status().message()); return false; }
    handler_ = std::make_unique<mozc::SessionHandler>(std::move(*engine));
    mozc::commands::Command command;
    command.mutable_input()->set_type(mozc::commands::Input::CREATE_SESSION);
    if (!handler_->EvalCommand(&command) || !command.output().id()) {
      error_ = "Mozc session creation failed"; handler_.reset(); return false;
    }
    session_ = command.output().id();
    auto update = Command(mozc::commands::SessionCommand::TURN_ON_IME);
    if (!update.error.empty()) { error_ = update.error; handler_.reset(); return false; }
    return true;
  }
  JapaneseUpdate Send(mozc::commands::Command& command) {
    command.mutable_input()->set_id(session_);
    if (!handler_->EvalCommand(&command) || command.output().error_code() != mozc::commands::Output::SESSION_SUCCESS)
      return {.error = "Mozc command failed"};
    const auto& output = command.output();
    JapaneseUpdate result;
    result.handled = output.consumed();
    result.committed = output.result().value();
    for (const auto& segment : output.preedit().segment()) result.preedit += segment.value();
    result.cursor = output.preedit().cursor();
    for (const auto& candidate : output.candidate_window().candidate())
      result.candidates.push_back({candidate.id(), candidate.value(),
          output.candidate_window().has_focused_index() &&
          candidate.index() == output.candidate_window().focused_index()});
    return result;
  }
  JapaneseUpdate Command(mozc::commands::SessionCommand::CommandType type, int candidate = 0) {
    mozc::commands::Command command;
    command.mutable_input()->set_type(mozc::commands::Input::SEND_COMMAND);
    auto* request = command.mutable_input()->mutable_command();
    request->set_type(type);
    if (type == mozc::commands::SessionCommand::TURN_ON_IME) request->set_composition_mode(mozc::commands::HIRAGANA);
    if (type == mozc::commands::SessionCommand::SELECT_CANDIDATE) request->set_id(candidate);
    return Send(command);
  }
  std::unique_ptr<mozc::SessionHandler> handler_;
  uint64_t session_ = 0;
  std::string error_;
};
JapaneseInput::JapaneseInput() = default;
JapaneseInput::~JapaneseInput() = default;
JapaneseUpdate JapaneseInput::Feed(JapaneseKey key, uint32_t character) {
  if (!state_) state_ = std::make_unique<State>();
  if (!state_->Initialize()) return {.error = state_->error_};
  mozc::commands::Command command;
  command.mutable_input()->set_type(mozc::commands::Input::SEND_KEY);
  auto* event = command.mutable_input()->mutable_key();
  using Key = mozc::commands::KeyEvent;
  switch (key) {
    case JapaneseKey::Character: event->set_key_code(character); break;
    case JapaneseKey::Space: event->set_special_key(Key::SPACE); break;
    case JapaneseKey::Enter: event->set_special_key(Key::ENTER); break;
    case JapaneseKey::Backspace: event->set_special_key(Key::BACKSPACE); break;
    case JapaneseKey::Escape: event->set_special_key(Key::ESCAPE); break;
    case JapaneseKey::Up: event->set_special_key(Key::UP); break;
    case JapaneseKey::Down: event->set_special_key(Key::DOWN); break;
    case JapaneseKey::Left: event->set_special_key(Key::LEFT); break;
    case JapaneseKey::Right: event->set_special_key(Key::RIGHT); break;
    case JapaneseKey::Delete: event->set_special_key(Key::DEL); break;
    case JapaneseKey::Home: event->set_special_key(Key::HOME); break;
    case JapaneseKey::End: event->set_special_key(Key::END); break;
    case JapaneseKey::Tab: event->set_special_key(Key::TAB); break;
  }
  return state_->Send(command);
}
JapaneseUpdate JapaneseInput::Finish() {
  return state_ && state_->handler_ ? state_->Command(mozc::commands::SessionCommand::SUBMIT) : JapaneseUpdate{};
}
JapaneseUpdate JapaneseInput::Select(int candidate) {
  return state_ && state_->handler_ ? state_->Command(mozc::commands::SessionCommand::SELECT_CANDIDATE, candidate) : JapaneseUpdate{};
}
void JapaneseInput::Clear() {
  if (state_ && state_->handler_) state_->Command(mozc::commands::SessionCommand::REVERT);
}
}
