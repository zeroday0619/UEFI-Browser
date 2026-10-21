#include "blink/devtools.h"
#include "blink/devtools_resources.h"
#include <cstdio>
#include <string>
#include <vector>
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/mojom/devtools/devtools_agent.mojom-blink.h"
#include "third_party/blink/public/mojom/devtools/devtools_frontend.mojom-blink.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "third_party/blink/renderer/controller/dev_tools_frontend_impl.h"
#include "third_party/blink/renderer/core/exported/web_dev_tools_agent_impl.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/inspector_protocol/crdtp/json.h"
#include "url/gurl.h"

namespace uefi_browser {
namespace {
namespace protocol = blink::mojom::blink;
template<class Interface, class Implementation>
mojo::PendingAssociatedRemote<Interface> HostEndpoint(
    mojo::AssociatedReceiver<Interface, Implementation>& receiver, bool dedicated = true) {
  mojo::PendingAssociatedRemote<Interface> remote;
  auto endpoint = remote.InitWithNewEndpointAndPassReceiver();
  if (dedicated) remote.EnableUnassociatedUsage();
  receiver.Bind(std::move(endpoint));
  return remote;
}
}
class DevToolsTarget::State final : public protocol::DevToolsAgentHost {
 public:
  explicit State(blink::WebLocalFrame* frame) {
    auto* agent = static_cast<blink::WebLocalFrameImpl*>(frame)->DevToolsAgentImpl(true);
    agent->BindReceiver(HostEndpoint(host_), remote.BindNewEndpointAndPassDedicatedReceiver());
  }
  void ChildTargetCreated(mojo::PendingRemote<protocol::DevToolsAgent>,
      mojo::PendingReceiver<protocol::DevToolsAgentHost>, const blink::KURL&,
      const blink::String&, const base::UnguessableToken&, bool,
      protocol::DevToolsExecutionContextType) override {
    std::puts("DEVTOOLS: child-target attachment is not connected");
  }
  void MainThreadDebuggerPaused() override { std::puts("DEVTOOLS: debugger paused"); }
  void MainThreadDebuggerResumed() override { std::puts("DEVTOOLS: debugger resumed"); }
  void BringToForeground() override {}
  mojo::AssociatedRemote<protocol::DevToolsAgent> remote;
 private:
  mojo::AssociatedReceiver<protocol::DevToolsAgentHost> host_{this};
};

class DevToolsFrontend::State final : public protocol::DevToolsFrontendHost,
                                     public protocol::DevToolsSessionHost {
 public:
  State(blink::WebLocalFrame* frame, DevToolsTarget::State& target) : frame_(frame) {
    // UEFI uses the packaged DevTools theme rather than Chrome's WebUI color service.
    preferences_.Set("chrome-theme-colors", "false");
    ResourceResponse compatibility{};
    CHECK_EQ(LoadDevToolsResource(
        GURL("https://devtools.uefi.invalid/front_end/devtools_compatibility.js"),
        &compatibility), 0u);
    CHECK_EQ(compatibility.status_code, 200u);
    auto* local = static_cast<blink::WebLocalFrameImpl*>(frame)->GetFrame();
    blink::DevToolsFrontendImpl::BindMojoRequest(
        local, frontend_.BindNewEndpointAndPassDedicatedReceiver());
    auto* implementation = blink::DevToolsFrontendImpl::From(local);
    // Setup must finish before navigation creates the frontend's main world.
    static_cast<protocol::DevToolsFrontend*>(implementation)->SetupDevToolsFrontend(
        blink::String::FromUtf8(std::string_view(
            reinterpret_cast<const char*>(compatibility.body), compatibility.length)),
        HostEndpoint(frontend_host_));
    resource_release(&compatibility);
    auto state = protocol::DevToolsSessionState::New();
    state->browser_originating_session_state = protocol::BrowserOriginatingSessionState::New();
    state->browser_originating_session_state->script_to_evaluate_on_load_once = blink::String("");
    state->browser_originating_session_state->paused_in_debugger_message = blink::String("");
    // Native authorization and bundled frontend code establish the trusted Inspector client.
    target.remote->AttachDevToolsSession(
        HostEndpoint(session_host_, false), session_.BindNewEndpointAndPassReceiver(),
        io_session_.BindNewPipeAndPassReceiver(), std::move(state),
        false, true, blink::String(""), false);
    session_.set_disconnect_handler(base::BindOnce(&State::Detached, weak_.GetWeakPtr()));
    std::puts("DEVTOOLS: inspector session requested");
  }
  void DispatchEmbedderMessage(base::DictValue message) override {
    // A reserved URL alone never grants access; only native-created tools own this receiver.
    if (!IsDevToolsOrigin(GURL(frame_->GetDocument().Url().GetString().Utf8()))) return;
    const auto* method = message.FindString("method");
    const base::ListValue empty_parameters;
    const auto* parameters = message.FindList("params");
    if (!method) return;
    // Chromium omits params for calls without arguments, including bootstrap queries.
    if (!parameters) parameters = &empty_parameters;
    if (embedder_messages_++ < 64)
      std::printf("DEVTOOLS: embedder method=%.128s\n", method->c_str());
    const int identifier = message.FindInt("id").value_or(0);
    if (*method == "dispatchProtocolMessage") {
      if (parameters->size() != 1 || !(*parameters)[0].is_string() || !session_) return;
      const auto& json = (*parameters)[0].GetString();
      if (json.size() > 4 * 1024 * 1024) return;
      auto command = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
      if (!command) return;
      auto call = command->FindInt("id");
      auto* name = command->FindString("method");
      if (!call || !name || name->size() > 256) return;
      std::vector<uint8_t> binary;
      if (!crdtp::json::ConvertJSONToCBOR(
          crdtp::span<uint8_t>(reinterpret_cast<const uint8_t*>(json.data()), json.size()),
          &binary).ok()) return;
      blink::String fallthrough("");
      if (*name == "Page.addScriptToEvaluateOnNewDocument" || *name == "Page.addScriptToEvaluateOnLoad") {
        // Blink requires the browser-generated identifier as protocol fallthrough data.
        fallthrough = blink::String::FromUtf8(std::to_string(++script_identifier_));
      }
      session_->DispatchProtocolCommand(*call, blink::String::FromUtf8(*name), binary, fallthrough);
      return;
    }
    if (*method == "getPreferences") { Ack(identifier, base::Value(preferences_.Clone())); return; }
    if (*method == "getHostConfig") { Ack(identifier, base::Value(base::DictValue())); return; }
    if (*method == "getSyncInformation") {
      base::DictValue config;
      config.Set("isSyncActive", false); config.Set("arePreferencesSynced", false);
      Ack(identifier, base::Value(std::move(config))); return;
    }
    if (*method == "setPreference" && parameters->size() == 2 &&
        (*parameters)[0].is_string() && (*parameters)[1].is_string()) {
      const auto& name = (*parameters)[0].GetString();
      const auto& value = (*parameters)[1].GetString();
      if (name.size() <= 256 && value.size() <= 65536 && preferences_.size() < 256)
        preferences_.Set(name, value);
    } else if (*method == "removePreference" && parameters->size() == 1 &&
               (*parameters)[0].is_string()) {
      preferences_.Remove((*parameters)[0].GetString());
    } else if (*method == "clearPreferences") preferences_.clear();
    else if (*method == "loadCompleted") {
      Call("setUseSoftMenu", base::Value(true));
      Call("setInitialTargetId", base::Value("main"));
      std::puts("DEVTOOLS: frontend loaded");
    } else if (*method == "requestFileSystems") {
      Call("fileSystemsLoaded", base::Value(base::ListValue()));
    } else if (*method == "loadNetworkResource") {
      base::DictValue response;
      response.Set("statusCode", 404);
      response.Set("netError", -2);
      response.Set("netErrorName", "External DevTools resource loading is not connected.");
      Ack(identifier, base::Value(std::move(response))); return;
    }
    if (identifier) Ack(identifier, base::Value());
  }
  void DispatchProtocolResponse(protocol::DevToolsMessagePtr message, int32_t,
      protocol::RendererOriginatingSessionStatePtr) override {
    Deliver(std::move(message));
  }
  void DispatchProtocolNotification(protocol::DevToolsMessagePtr message,
      protocol::RendererOriginatingSessionStatePtr) override {
    Deliver(std::move(message));
  }
 private:
  void Detached() {
    session_.reset();
    Call("dispatchMessage", base::Value("{\"method\":\"Inspector.detached\",\"params\":{\"reason\":\"Target closed\"}}"));
    std::puts("DEVTOOLS: target session detached");
  }
  void Deliver(protocol::DevToolsMessagePtr message) {
    if (!message || message->data.size() > 16 * 1024 * 1024) return;
    Call("dispatchMessage", base::Value(std::string(
        reinterpret_cast<const char*>(message->data.data()), message->data.size())));
  }
  void Ack(int identifier, base::Value argument) {
    auto json = base::WriteJson(argument);
    if (json) Queue("DevToolsAPI.embedderMessageAck(" + std::to_string(identifier) + "," + *json + ");");
  }
  void Call(const char* method, base::Value argument) {
    auto json = base::WriteJson(argument);
    if (json) Queue(std::string("DevToolsAPI.") + method + "(" + *json + ");");
  }
  void Queue(std::string script) {
    // Protocol payloads are quoted as JSON and run on a later task to avoid callback reentry.
    if (script.size() > 32 * 1024 * 1024 - queued_bytes_) {
      session_.reset();
      std::puts("DEVTOOLS: response queue limit reached");
      return;
    }
    queued_bytes_ += script.size();
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&State::Execute, weak_.GetWeakPtr(), std::move(script)));
  }
  void Execute(std::string script) {
    queued_bytes_ -= script.size();
    if (IsDevToolsOrigin(GURL(frame_->GetDocument().Url().GetString().Utf8())))
      frame_->ExecuteScript(blink::WebScriptSource(blink::WebString::FromUtf8(script)));
  }
  blink::WebLocalFrame* frame_;
  base::DictValue preferences_;
  size_t queued_bytes_ = 0;
  unsigned embedder_messages_ = 0;
  uint64_t script_identifier_ = 0;
  mojo::AssociatedRemote<protocol::DevToolsFrontend> frontend_;
  mojo::AssociatedReceiver<protocol::DevToolsFrontendHost> frontend_host_{this};
  mojo::AssociatedRemote<protocol::DevToolsSession> session_;
  mojo::Remote<protocol::DevToolsSession> io_session_;
  mojo::AssociatedReceiver<protocol::DevToolsSessionHost> session_host_{this};
  base::WeakPtrFactory<State> weak_{this};
};
DevToolsTarget::DevToolsTarget(blink::WebLocalFrame* frame) : state_(std::make_unique<State>(frame)) {}
DevToolsTarget::~DevToolsTarget() = default;
DevToolsFrontend::DevToolsFrontend(blink::WebLocalFrame* frame, DevToolsTarget& target)
    : state_(std::make_unique<State>(frame, *target.state_)) {}
DevToolsFrontend::~DevToolsFrontend() = default;
}
