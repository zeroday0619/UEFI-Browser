#include "blink/web_socket.h"
#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/i18n/streaming_utf8_validator.h"
#include "base/memory/weak_ptr.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "blink/cookies.h"
#include "firmware_io.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/net_errors.h"
#include "net/base/port_util.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/websockets/websocket_frame_parser.h"
#include "net/websockets/websocket_handshake_challenge.h"
#include "services/network/public/mojom/websocket.mojom.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"
#include "third_party/boringssl/src/include/openssl/x509.h"
#include "uefi.h"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
extern "C" SystemTable *platform_firmware(void);
namespace uefi_browser {
namespace {
using Bytes = std::vector<uint8_t>;
using MessageType = network::mojom::WebSocketMessageType;
constexpr size_t kQueueLimit = 8 * 1024 * 1024;
constexpr size_t kConnectionLimit = 16;

bool LoadTrust(SSL_CTX *context) {
  auto *table = platform_firmware();
  if (!table || !table->runtime)
    return false;
  auto **entries = reinterpret_cast<void **>(
      static_cast<uint8_t *>(table->runtime) + sizeof(TableHeader));
  using GetVariable =
      Status(UEFI_API *)(Character *, Guid *, uint32_t *, size_t *, void *);
  auto get_variable = reinterpret_cast<GetVariable>(entries[6]);
  Guid identifier = {0xfd2340d0,
                     0x3dab,
                     0x4349,
                     {0xa6, 0xc7, 0x3b, 0x4f, 0x12, 0xb4, 0x8e, 0xae}};
  Character name[] = {'T', 'l', 's', 'C', 'a', 'C', 'e', 'r', 't',
                      'i', 'f', 'i', 'c', 'a', 't', 'e', 0};
  size_t size = 0;
  if (!get_variable ||
      get_variable(name, &identifier, nullptr, &size, nullptr) != ERROR(5) ||
      size > 2 * 1024 * 1024)
    return false;
  Bytes data(size);
  if (get_variable(name, &identifier, nullptr, &size, data.data()) ||
      size > data.size())
    return false;
  const Guid x509_identifier = {
      0xa5c059a1,
      0x94e4,
      0x4aa7,
      {0x87, 0xb5, 0xab, 0x15, 0x5c, 0x2b, 0xf0, 0x72}};
  size_t offset = 0, certificates = 0;
  while (offset < size) {
    if (size - offset < 28)
      return false;
    uint32_t list_size, header_size, signature_size;
    std::memcpy(&list_size, data.data() + offset + 16, 4);
    std::memcpy(&header_size, data.data() + offset + 20, 4);
    std::memcpy(&signature_size, data.data() + offset + 24, 4);
    if (list_size < 28 || list_size > size - offset ||
        header_size > list_size - 28 || signature_size <= 16 ||
        (list_size - 28 - header_size) % signature_size)
      return false;
    if (!std::memcmp(data.data() + offset, &x509_identifier, 16)) {
      for (size_t position = offset + 28 + header_size;
           position < offset + list_size; position += signature_size) {
        const uint8_t *encoded = data.data() + position + 16;
        const uint8_t *end = encoded + signature_size - 16;
        bssl::UniquePtr<X509> certificate(
            d2i_X509(nullptr, &encoded, signature_size - 16));
        if (!certificate || encoded != end)
          return false;
        if (X509_STORE_add_cert(SSL_CTX_get_cert_store(context),
                                certificate.get()))
          ++certificates;
      }
    }
    offset += list_size;
  }
  return certificates != 0;
}

class Connection final : public network::mojom::WebSocket {
public:
  Connection(
      GURL url, url::Origin origin, std::vector<std::string> protocols,
      std::string agent, const net::SiteForCookies &site_for_cookies,
      mojo::PendingRemote<network::mojom::WebSocketHandshakeClient> client)
      : url_(std::move(url)), origin_(std::move(origin)),
        protocols_(std::move(protocols)), handshake_(std::move(client)),
        deadline_(base::TimeTicks::Now() + base::Seconds(30)) {
    handshake_.set_disconnect_handler(
        base::BindOnce(&Connection::Stop, weak_.GetWeakPtr()));
    GURL::Replacements replacements;
    replacements.SetSchemeStr(url_.SchemeIs("wss") ? "https" : "http");
    http_url_ = url_.ReplaceComponents(replacements);
    cookies_allowed_ = origin_.IsSameOriginWith(http_url_) &&
                       site_for_cookies.IsFirstParty(http_url_);
    uint8_t numeric[4];
    if (inet_pton(AF_INET, std::string(url_.host()).c_str(), numeric) == 1) {
      if (!ConnectSocket(numeric))
        return;
    } else {
      std::string hostname(url_.host());
      dns_handle_ = firmware_dns_request(0, 0, hostname.data());
      if (dns_handle_ < 0) {
        Fail("WebSocket DNS initialization failed", net::ERR_NAME_NOT_RESOLVED);
        return;
      }
    }
    std::array<uint8_t, 16> nonce;
    base::RandBytes(nonce);
    std::string key = base::Base64Encode(nonce);
    accept_ = net::ComputeSecWebSocketAccept(key);
    std::string request =
        "GET " + url_.PathForRequest() +
        " HTTP/1.1\r\nHost: " + std::string(url_.host()) + ":" +
        std::to_string(url_.EffectiveIntPort()) +
        "\r\nUpgrade: websocket\r\nConnection: "
        "Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
        key + "\r\nOrigin: " + origin_.Serialize() + "\r\n";
    if (!agent.empty() && net::HttpUtil::IsValidHeaderValue(agent))
      request += "User-Agent: " + agent + "\r\n";
    if (!protocols_.empty()) {
      request += "Sec-WebSocket-Protocol: ";
      for (size_t index = 0; index < protocols_.size(); ++index) {
        if (index)
          request += ", ";
        request += protocols_[index];
      }
      request += "\r\n";
    }
    // Same-origin requests use the existing cookie policy without widening
    // cross-site access.
    if (cookies_allowed_) {
      auto cookies = CookieHeader(http_url_, false);
      if (!cookies.empty())
        request += "Cookie: " + cookies + "\r\n";
    }
    request += "\r\n";
    outgoing_.emplace_back(request.begin(), request.end());
    queued_outgoing_ = request.size();
    auto info = network::mojom::WebSocketHandshakeRequest::New();
    info->url = url_;
    info->headers_text = request;
    handshake_->OnOpeningHandshakeStarted(std::move(info));
    Schedule();
  }
  ~Connection() override { Stop(); }
  bool finished() const { return finished_; }
  void SendMessage(MessageType type, uint64_t length) override {
    if (finished_ || closing_)
      return;
    if ((type != MessageType::TEXT && type != MessageType::BINARY) ||
        length > kQueueLimit || queued_messages_ > kQueueLimit - length ||
        pending_.size() >= 1024) {
      Fail("WebSocket send limit exceeded");
      return;
    }
    pending_.push_back({type, static_cast<size_t>(length), {}});
    queued_messages_ += length;
  }
  void StartReceiving() override { receiving_ = true; }
  void StartClosingHandshake(uint16_t code,
                             const std::string &reason) override {
    if (finished_ || closing_)
      return;
    if (reason.size() > 123 ||
        !base::StreamingUtf8Validator::Validate(reason) ||
        !(code == 1000 || (code >= 3000 && code <= 4999) || code == 1001 ||
          code == 1005)) {
      Fail("Invalid WebSocket close request");
      return;
    }
    closing_ = true;
    close_code_ = code;
    close_reason_ = reason;
    deadline_ = base::TimeTicks::Now() + base::Seconds(5);
    // The close frame follows all messages already accepted from the data pipe.
  }

private:
  bool ConnectSocket(const uint8_t address_bytes[4]) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(url_.EffectiveIntPort());
    std::memcpy(&address.sin_addr, address_bytes, 4);
    address_ = net::IPAddress(base::span(address_bytes, size_t{4}));
    descriptor_ =
        socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (descriptor_ < 0 ||
        (connect(descriptor_, reinterpret_cast<sockaddr *>(&address),
                 sizeof(address)) < 0 &&
         errno != EINPROGRESS)) {
      Fail("WebSocket TCP connection failed", net::ERR_CONNECTION_FAILED);
      return false;
    }
    if (url_.SchemeIs("wss")) {
      context_.reset(SSL_CTX_new(TLS_method()));
      if (!context_ || !LoadTrust(context_.get())) {
        Fail("WebSocket trust store unavailable",
             net::ERR_CERT_AUTHORITY_INVALID);
        return false;
      }
      SSL_CTX_set_verify(context_.get(), SSL_VERIFY_PEER, nullptr);
      SSL_CTX_set_min_proto_version(context_.get(), TLS1_2_VERSION);
      tls_.reset(SSL_new(context_.get()));
      std::string hostname(url_.host());
      uint8_t numeric[4];
      bool identity_valid = tls_ && SSL_set_fd(tls_.get(), descriptor_);
      if (identity_valid && inet_pton(AF_INET, hostname.c_str(), numeric) == 1)
        identity_valid = X509_VERIFY_PARAM_set1_ip_asc(
            SSL_get0_param(tls_.get()), hostname.c_str());
      else if (identity_valid)
        identity_valid = SSL_set1_host(tls_.get(), hostname.c_str()) &&
                         SSL_set_tlsext_host_name(tls_.get(), hostname.c_str());
      if (!identity_valid) {
        Fail("WebSocket TLS identity initialization failed",
             net::ERR_SSL_PROTOCOL_ERROR);
        return false;
      }
      SSL_set_connect_state(tls_.get());
    }
    return true;
  }
  struct Pending {
    MessageType type;
    size_t size;
    Bytes bytes;
  };
  struct Received {
    MessageType type;
    Bytes bytes;
    size_t offset = 0;
    bool announced = false;
  };
  void Schedule() {
    if (!finished_)
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE, base::BindOnce(&Connection::Tick, weak_.GetWeakPtr()),
          base::Milliseconds(2));
  }
  void Stop() {
    finished_ = true;
    weak_.InvalidateWeakPtrs();
    if (dns_handle_ > 0) {
      firmware_dns_request(2, dns_handle_, nullptr);
      dns_handle_ = 0;
    }
    tls_.reset();
    context_.reset();
    if (descriptor_ >= 0)
      close(std::exchange(descriptor_, -1));
    socket_.reset();
    handshake_.reset();
    client_.reset();
    incoming_pipe_.reset();
    outgoing_pipe_.reset();
    outgoing_.clear();
    pending_.clear();
    received_.clear();
    message_.clear();
    frame_.clear();
  }
  void Fail(const std::string &message,
            int error = net::ERR_WS_PROTOCOL_ERROR) {
    std::printf("WEBSOCKET: %s (%d)\n", message.c_str(), error);
    if (handshake_)
      handshake_->OnFailure(message, error, -1);
    if (client_)
      client_->OnDropChannel(false, 1006, "");
    Stop();
  }
  int Transfer(void *data, size_t size, bool writing) {
    if (tls_) {
      int result = writing ? SSL_write(tls_.get(), data, size)
                           : SSL_read(tls_.get(), data, size);
      if (result > 0)
        return result;
      int error = SSL_get_error(tls_.get(), result);
      if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
        return -2;
      return error == SSL_ERROR_ZERO_RETURN ? 0 : -1;
    }
    int result = writing ? send(descriptor_, data, size, MSG_NOSIGNAL)
                         : recv(descriptor_, data, size, 0);
    return result < 0 &&
                   (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
               ? -2
               : result;
  }
  bool Frame(unsigned opcode, const Bytes &bytes) {
    net::WebSocketFrameHeader header(opcode);
    header.final = true;
    header.masked = true;
    header.payload_length = bytes.size();
    auto mask = net::GenerateWebSocketMaskingKey();
    size_t length = net::GetWebSocketFrameHeaderSize(header);
    if (bytes.size() + length > kQueueLimit - queued_outgoing_ ||
        outgoing_.size() >= 1024) {
      Fail("WebSocket wire queue exceeded");
      return false;
    }
    Bytes output(length + bytes.size());
    if (net::WriteWebSocketFrameHeader(header, &mask, output) < 0) {
      Fail("WebSocket frame serialization failed");
      return false;
    }
    std::copy(bytes.begin(), bytes.end(), output.begin() + length);
    net::MaskWebSocketFramePayload(mask, 0, base::span(output).subspan(length));
    queued_outgoing_ += output.size();
    outgoing_.push_back(std::move(output));
    return true;
  }
  bool Establish() {
    auto end = reply_.find("\r\n\r\n");
    if (end == std::string::npos)
      return true;
    auto headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        net::HttpUtil::AssembleRawHeaders(reply_.substr(0, end + 4)));
    auto selected = headers->GetNormalizedHeader("sec-websocket-protocol");
    if (headers->response_code() != 101 ||
        headers->GetHttpVersion() != net::HttpVersion(1, 1) ||
        !headers->HasHeaderValue("upgrade", "websocket") ||
        !headers->HasHeaderValue("connection", "upgrade") ||
        headers->GetNormalizedHeader("sec-websocket-accept") != accept_ ||
        headers->HasHeader("sec-websocket-extensions") ||
        (selected && std::find(protocols_.begin(), protocols_.end(),
                               *selected) == protocols_.end())) {
      Fail("WebSocket opening handshake rejected");
      return false;
    }
    mojo::ScopedDataPipeConsumerHandle readable;
    mojo::ScopedDataPipeProducerHandle writable;
    MojoCreateDataPipeOptions options{
        sizeof(options), MOJO_CREATE_DATA_PIPE_FLAG_NONE, 1, 65536};
    if (mojo::CreateDataPipe(&options, incoming_pipe_, readable) !=
            MOJO_RESULT_OK ||
        mojo::CreateDataPipe(&options, writable, outgoing_pipe_) !=
            MOJO_RESULT_OK) {
      Fail("WebSocket data pipe allocation failed");
      return false;
    }
    auto response = network::mojom::WebSocketHandshakeResponse::New();
    response->url = url_;
    response->http_version = headers->GetHttpVersion();
    response->status_code = 101;
    response->status_text = headers->GetStatusText();
    response->headers_text = reply_.substr(0, end + 4);
    response->remote_endpoint =
        net::IPEndPoint(address_, url_.EffectiveIntPort());
    response->selected_protocol = selected.value_or("");
    size_t iterator = 0;
    std::string name, value;
    while (headers->EnumerateHeaderLines(&iterator, &name, &value)) {
      if (base::EqualsCaseInsensitiveASCII(name, "set-cookie")) {
        if (cookies_allowed_)
          StoreCookie(http_url_, value, false);
        continue;
      }
      response->headers.push_back(network::mojom::HttpHeader::New(name, value));
    }
    auto remote = socket_.BindNewPipeAndPassRemote();
    socket_.set_disconnect_handler(
        base::BindOnce(&Connection::Stop, weak_.GetWeakPtr()));
    auto client_receiver = client_.BindNewPipeAndPassReceiver();
    client_.set_disconnect_handler(
        base::BindOnce(&Connection::Stop, weak_.GetWeakPtr()));
    handshake_->OnConnectionEstablished(
        std::move(remote), std::move(client_receiver), std::move(response),
        std::move(readable), std::move(writable));
    handshake_.reset();
    established_ = true;
    Bytes remaining(reply_.begin() + end + 4, reply_.end());
    reply_.clear();
    std::puts("WEBSOCKET: opening handshake accepted");
    return Decode(remaining);
  }
  bool Decode(base::span<uint8_t> bytes) {
    std::vector<std::unique_ptr<net::WebSocketFrameChunk>> chunks;
    if (!parser_.Decode(bytes, &chunks)) {
      Fail("Malformed WebSocket frame");
      return false;
    }
    for (auto &chunk : chunks) {
      if (chunk->header) {
        auto &header = *chunk->header;
        opcode_ = header.opcode;
        final_ = header.final;
        frame_.clear();
        if (header.masked || header.reserved1 || header.reserved2 ||
            header.reserved3 || header.payload_length > kQueueLimit ||
            !(opcode_ == 0 || opcode_ == 1 || opcode_ == 2 || opcode_ == 8 ||
              opcode_ == 9 || opcode_ == 10) ||
            (opcode_ >= 8 && (!final_ || header.payload_length > 125)) ||
            (opcode_ == 0 && !fragmented_) ||
            ((opcode_ == 1 || opcode_ == 2) && fragmented_)) {
          Fail("Invalid WebSocket frame header");
          return false;
        }
      }
      frame_.insert(frame_.end(), chunk->payload.begin(), chunk->payload.end());
      if (!chunk->final_chunk)
        continue;
      if (opcode_ == 8) {
        auto close = net::ParseCloseFrame(base::span(
            reinterpret_cast<const char *>(frame_.data()), frame_.size()));
        if (close.error) {
          Fail("Invalid WebSocket close frame");
          return false;
        }
        close_received_ = true;
        close_code_ = close.code;
        close_reason_ = close.reason;
        if (!closing_) {
          closing_ = true;
          client_->OnClosingHandshake();
          deadline_ = base::TimeTicks::Now() + base::Seconds(5);
        }
        pending_.clear();
        queued_messages_ = 0;
        if (!close_sent_) {
          if (!Frame(8, frame_))
            return false;
          close_sent_ = true;
        }
        continue;
      }
      if (opcode_ == 9) {
        if (!close_sent_ && !Frame(10, frame_))
          return false;
        continue;
      }
      if (opcode_ == 10 || closing_)
        continue;
      if (opcode_ != 0)
        message_type_ = opcode_ == 1 ? MessageType::TEXT : MessageType::BINARY;
      if (frame_.size() > kQueueLimit - message_.size()) {
        Fail("WebSocket message limit exceeded");
        return false;
      }
      message_.insert(message_.end(), frame_.begin(), frame_.end());
      fragmented_ = !final_;
      if (final_) {
        if (message_type_ == MessageType::TEXT &&
            !base::StreamingUtf8Validator::Validate(
                std::string(message_.begin(), message_.end()))) {
          Fail("Invalid WebSocket UTF-8");
          return false;
        }
        if (message_.size() > kQueueLimit - queued_received_ ||
            received_.size() >= 1024) {
          Fail("WebSocket receive queue exceeded");
          return false;
        }
        queued_received_ += message_.size();
        received_.push_back({message_type_, std::move(message_)});
        message_.clear();
      }
    }
    return true;
  }
  void Tick() {
    if (finished_)
      return;
    if ((!established_ || closing_) && base::TimeTicks::Now() > deadline_) {
      Fail("WebSocket timeout", net::ERR_TIMED_OUT);
      return;
    }
    if (dns_handle_ > 0) {
      uint8_t address[4];
      long result = firmware_dns_request(1, dns_handle_, address);
      if (!result) {
        Schedule();
        return;
      }
      firmware_dns_request(2, dns_handle_, nullptr);
      dns_handle_ = 0;
      if (result < 0) {
        Fail("WebSocket DNS lookup failed", net::ERR_NAME_NOT_RESOLVED);
        return;
      }
      std::puts("WEBSOCKET: DNS lookup completed");
      if (!ConnectSocket(address))
        return;
    }
    pollfd state{descriptor_, POLLIN | POLLOUT, 0};
    if (poll(&state, 1, 0) < 0 || (state.revents & (POLLERR | POLLNVAL))) {
      Fail("WebSocket socket error");
      return;
    }
    if (!connected_) {
      if (!(state.revents & POLLOUT)) {
        Schedule();
        return;
      }
      int error = 0;
      socklen_t length = sizeof(error);
      if (getsockopt(descriptor_, SOL_SOCKET, SO_ERROR, &error, &length) < 0 ||
          error) {
        Fail("WebSocket connect failed");
        return;
      }
      connected_ = true;
    }
    if (tls_ && !tls_ready_) {
      int result = SSL_connect(tls_.get());
      if (result != 1) {
        int error = SSL_get_error(tls_.get(), result);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
          Schedule();
          return;
        }
        Fail("WebSocket TLS certificate or handshake failed",
             net::ERR_SSL_PROTOCOL_ERROR);
        return;
      }
      tls_ready_ = true;
    }
    if (established_) {
      for (unsigned budget = 0; budget < 4 && !pending_.empty(); ++budget) {
        auto &item = pending_.front();
        size_t remaining = item.size - item.bytes.size();
        if (remaining) {
          std::array<uint8_t, 16384> data;
          size_t count = 0;
          auto result = outgoing_pipe_->ReadData(
              MOJO_READ_DATA_FLAG_NONE,
              base::span(data).first(std::min(remaining, data.size())), count);
          if (result == MOJO_RESULT_SHOULD_WAIT)
            break;
          if (result != MOJO_RESULT_OK) {
            Fail("WebSocket outgoing pipe closed");
            return;
          }
          item.bytes.insert(item.bytes.end(), data.begin(),
                            data.begin() + count);
        }
        if (item.bytes.size() != item.size)
          continue;
        if (!Frame(item.type == MessageType::TEXT ? 1 : 2, item.bytes))
          return;
        queued_messages_ -= item.size;
        pending_.pop_front();
      }
      if (closing_ && !close_sent_ && pending_.empty()) {
        Bytes payload;
        if (close_code_ != 1005) {
          payload = {uint8_t(close_code_ >> 8), uint8_t(close_code_)};
          payload.insert(payload.end(), close_reason_.begin(),
                         close_reason_.end());
        }
        if (!Frame(8, payload))
          return;
        close_sent_ = true;
      }
      for (unsigned budget = 0; receiving_ && budget < 4 && !received_.empty();
           ++budget) {
        auto &item = received_.front();
        if (!item.announced) {
          client_->OnDataFrame(true, item.type, item.bytes.size());
          item.announced = true;
        }
        if (item.offset < item.bytes.size()) {
          size_t count = 0;
          auto result = incoming_pipe_->WriteData(
              base::span(item.bytes).subspan(item.offset),
              MOJO_WRITE_DATA_FLAG_NONE, count);
          if (result == MOJO_RESULT_SHOULD_WAIT)
            break;
          if (result != MOJO_RESULT_OK) {
            Fail("WebSocket incoming pipe closed");
            return;
          }
          item.offset += count;
        }
        if (item.offset == item.bytes.size()) {
          queued_received_ -= item.bytes.size();
          received_.pop_front();
        }
      }
    }
    // Bounded work leaves the renderer event loop available between network
    // polls.
    if (!outgoing_.empty()) {
      auto &bytes = outgoing_.front();
      size_t length = std::min<size_t>(16384, bytes.size() - write_offset_);
      int count = Transfer(bytes.data() + write_offset_, length, true);
      if (count != -2 && count <= 0) {
        Fail("WebSocket write failed");
        return;
      }
      if (count > 0) {
        write_offset_ += count;
        queued_outgoing_ -= count;
        if (write_offset_ == bytes.size()) {
          outgoing_.pop_front();
          write_offset_ = 0;
        }
      }
    }
    std::array<uint8_t, 16384> bytes;
    int count = Transfer(bytes.data(), bytes.size(), false);
    if (count == 0) {
      if (close_received_ && close_sent_ && outgoing_.empty() &&
          received_.empty()) {
        client_->OnDropChannel(true, close_code_, close_reason_);
        Stop();
        return;
      }
      if (close_received_ && (!outgoing_.empty() || !received_.empty())) {
        Schedule();
        return;
      }
      Fail("WebSocket peer closed without a close handshake");
      return;
    }
    if (count == -1) {
      Fail("WebSocket read failed");
      return;
    }
    if (count > 0) {
      if (!established_) {
        reply_.append(reinterpret_cast<const char *>(bytes.data()), count);
        if (reply_.size() > 65536) {
          Fail("WebSocket HTTP headers exceed limit");
          return;
        }
        if (!Establish())
          return;
      } else if (!Decode(base::span(bytes).first(static_cast<size_t>(count))))
        return;
    }
    // Firmware close aborts pending transmit tokens, so wait for POLLOUT after
    // sending Close.
    if (close_received_ && close_sent_ && outgoing_.empty() &&
        received_.empty()) {
      pollfd drained{descriptor_, POLLOUT, 0};
      poll(&drained, 1, 0);
      if (drained.revents & POLLOUT) {
        client_->OnDropChannel(true, close_code_, close_reason_);
        Stop();
        return;
      }
    }
    Schedule();
  }
  GURL url_, http_url_;
  url::Origin origin_;
  std::vector<std::string> protocols_;
  net::IPAddress address_;
  mojo::Remote<network::mojom::WebSocketHandshakeClient> handshake_;
  mojo::Remote<network::mojom::WebSocketClient> client_;
  mojo::Receiver<network::mojom::WebSocket> socket_{this};
  mojo::ScopedDataPipeProducerHandle incoming_pipe_;
  mojo::ScopedDataPipeConsumerHandle outgoing_pipe_;
  bssl::UniquePtr<SSL_CTX> context_;
  bssl::UniquePtr<SSL> tls_;
  int descriptor_ = -1;
  long dns_handle_ = 0;
  bool cookies_allowed_ = false;
  bool finished_ = false, connected_ = false, tls_ready_ = false,
       established_ = false, receiving_ = false;
  bool closing_ = false, close_sent_ = false, close_received_ = false,
       fragmented_ = false, final_ = false;
  unsigned opcode_ = 0;
  MessageType message_type_ = MessageType::TEXT;
  uint16_t close_code_ = 1000;
  std::string accept_, reply_, close_reason_;
  Bytes frame_, message_;
  std::deque<Bytes> outgoing_;
  std::deque<Pending> pending_;
  std::deque<Received> received_;
  size_t queued_outgoing_ = 0, queued_messages_ = 0, queued_received_ = 0,
         write_offset_ = 0;
  net::WebSocketFrameParser parser_;
  base::TimeTicks deadline_;
  base::WeakPtrFactory<Connection> weak_{this};
};
class Service final : public WebSocketService,
                      public blink::mojom::WebSocketConnector {
public:
  explicit Service(blink::LocalFrameToken token) : token_(token) {}
  void Bind(mojo::PendingReceiver<blink::mojom::WebSocketConnector> receiver)
      override {
    receivers_.Add(this, std::move(receiver));
  }
  void
  Connect(const GURL &url, const std::vector<std::string> &protocols,
          const std::optional<std::string> &agent,
          mojo::PendingRemote<network::mojom::WebSocketHandshakeClient> client,
          const std::optional<base::UnguessableToken> &,
          network::mojom::IPAddressSpace) override {
    auto *frame = blink::LocalFrame::FromFrameToken(token_);
    std::erase_if(connections_,
                  [](const auto &item) { return item->finished(); });
    if (!frame || !frame->DomWindow() || !url.is_valid() ||
        !(url.SchemeIs("ws") || url.SchemeIs("wss")) || url.has_username() ||
        url.has_password() || url.has_ref() || protocols.size() > 32 ||
        !net::IsPortAllowedForScheme(url.EffectiveIntPort(), url.scheme()) ||
        connections_.size() >= kConnectionLimit ||
        std::any_of(
            protocols.begin(), protocols.end(), [](const auto &protocol) {
              return protocol.size() > 256 || !net::HttpUtil::IsToken(protocol);
            })) {
      mojo::Remote<network::mojom::WebSocketHandshakeClient> remote(
          std::move(client));
      remote->OnFailure("WebSocket request is not permitted",
                        net::ERR_ACCESS_DENIED, -1);
      return;
    }
    auto origin = frame->DomWindow()->GetSecurityOrigin()->ToUrlOrigin();
    if (origin.scheme() == "https" && url.SchemeIs("ws")) {
      mojo::Remote<network::mojom::WebSocketHandshakeClient> remote(
          std::move(client));
      remote->OnFailure("Insecure WebSocket from a secure document",
                        net::ERR_ACCESS_DENIED, -1);
      return;
    }
    connections_.push_back(std::make_unique<Connection>(
        url, std::move(origin), protocols, agent.value_or(""),
        frame->DomWindow()->document()->SiteForCookies(), std::move(client)));
  }

private:
  blink::LocalFrameToken token_;
  mojo::ReceiverSet<blink::mojom::WebSocketConnector> receivers_;
  std::vector<std::unique_ptr<Connection>> connections_;
};
} // namespace
std::unique_ptr<WebSocketService>
CreateWebSocketService(blink::LocalFrameToken token) {
  return std::make_unique<Service>(token);
}
} // namespace uefi_browser
