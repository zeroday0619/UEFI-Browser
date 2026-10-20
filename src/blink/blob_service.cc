#include "blink/blob_service.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/uuid.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/data_pipe_getter.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "third_party/blink/public/mojom/blob/blob.mojom.h"
#include "third_party/blink/public/mojom/blob/data_element.mojom.h"
#include "third_party/blink/public/mojom/blob/serialized_blob.mojom.h"
#include <algorithm>
#include <limits>

namespace uefi_browser {
namespace {
constexpr size_t kMaximumBlob = 8 * 1024 * 1024;
constexpr size_t kMaximumLiveBytes = 64 * 1024 * 1024;
constexpr size_t kMaximumTransfers = 64;
size_t live_bytes = 0;
size_t transfers = 0;
struct Content {
  BlobBytes bytes;
  bool ready = false;
  bool valid = false;
  std::string uuid = base::Uuid::GenerateRandomV4().AsLowercaseString();
  std::vector<base::OnceClosure> waiting;
  ~Content() {
    if (valid)
      live_bytes -= bytes.size();
  }
  void Finish(std::optional<BlobBytes> value) {
    if (ready)
      return;
    ready = true;
    valid = value && value->size() <= kMaximumBlob &&
            value->size() <= kMaximumLiveBytes - live_bytes;
    if (valid) {
      bytes = std::move(*value);
      live_bytes += bytes.size();
    }
    auto callbacks = std::move(waiting);
    for (auto &callback : callbacks)
      std::move(callback).Run();
  }
};
// Pipe work stays on the renderer sequence because firmware has no general
// thread pool.
class PipeWriter {
public:
  PipeWriter(std::shared_ptr<Content> content, size_t offset, size_t length,
             mojo::ScopedDataPipeProducerHandle pipe,
             mojo::PendingRemote<blink::mojom::BlobReaderClient> client)
      : content_(std::move(content)), offset_(offset), length_(length),
        pipe_(std::move(pipe)), client_(std::move(client)) {
    ++transfers;
    if (client_)
      client_->OnCalculatedSize(content_->bytes.size(), length_);
    watcher_.Watch(
        pipe_.get(), MOJO_HANDLE_SIGNAL_WRITABLE,
        base::BindRepeating(&PipeWriter::Write, base::Unretained(this)));
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&PipeWriter::Complete, weak_.GetWeakPtr(),
                       net::ERR_TIMED_OUT),
        base::Seconds(30));
  }
  void Start() { Write(MOJO_RESULT_OK); }

private:
  void Complete(int status) {
    if (client_)
      client_->OnComplete(status, sent_);
    delete this;
  }
  void Write(MojoResult) {
    for (unsigned count = 0; count < 4 && sent_ < length_; ++count) {
      size_t size = std::min<size_t>(16384, length_ - sent_);
      auto result = pipe_->WriteData(
          base::span(content_->bytes).subspan(offset_ + sent_, size),
          MOJO_WRITE_DATA_FLAG_NONE, size);
      if (result == MOJO_RESULT_SHOULD_WAIT) {
        watcher_.ArmOrNotify();
        return;
      }
      if (result != MOJO_RESULT_OK) {
        Complete(net::ERR_FAILED);
        return;
      }
      sent_ += size;
    }
    if (sent_ == length_) {
      Complete(net::OK);
      return;
    }
    watcher_.ArmOrNotify();
  }
  ~PipeWriter() { --transfers; }
  std::shared_ptr<Content> content_;
  size_t offset_, length_, sent_ = 0;
  mojo::ScopedDataPipeProducerHandle pipe_;
  mojo::Remote<blink::mojom::BlobReaderClient> client_;
  mojo::SimpleWatcher watcher_{FROM_HERE,
                               mojo::SimpleWatcher::ArmingPolicy::MANUAL};
  base::WeakPtrFactory<PipeWriter> weak_{this};
};
class PipeReader final : public blink::mojom::BlobReaderClient {
public:
  PipeReader(mojo::ScopedDataPipeConsumerHandle pipe, BlobReadCallback callback,
             mojo::PendingRemote<blink::mojom::Blob> blob = {})
      : pipe_(std::move(pipe)), callback_(std::move(callback)),
        blob_(std::move(blob)) {
    ++transfers;
    require_status_ = blob_.is_bound();
  }
  void Start(uint64_t offset, uint64_t length) {
    if (blob_)
      blob_->ReadRange(offset, length, std::move(producer_),
                       receiver_.BindNewPipeAndPassRemote());
    watcher_.Watch(
        pipe_.get(), MOJO_HANDLE_SIGNAL_READABLE,
        base::BindRepeating(&PipeReader::Read, base::Unretained(this)));
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(&PipeReader::Fail, weak_.GetWeakPtr()),
        base::Seconds(30));
    watcher_.ArmOrNotify();
  }
  mojo::ScopedDataPipeProducerHandle producer_;
  void OnCalculatedSize(uint64_t, uint64_t size) override {
    if (size > kMaximumBlob) {
      Fail();
      return;
    }
    expected_ = size;
  }
  void OnComplete(int32_t status, uint64_t size) override {
    if (status != net::OK || size != expected_) {
      Fail();
      return;
    }
    status_received_ = true;
    MaybeFinish();
  }

private:
  ~PipeReader() override { --transfers; }
  void Fail() {
    auto callback = std::move(callback_);
    delete this;
    std::move(callback).Run(std::nullopt);
  }
  void MaybeFinish() {
    if (!eof_ || (require_status_ && !status_received_))
      return;
    if (require_status_ && bytes_.size() != expected_) {
      Fail();
      return;
    }
    auto callback = std::move(callback_);
    auto bytes = std::move(bytes_);
    delete this;
    std::move(callback).Run(std::move(bytes));
  }
  void Read(MojoResult) {
    for (unsigned count = 0; count < 4; ++count) {
      uint8_t buffer[16384];
      size_t size = sizeof(buffer);
      auto result =
          pipe_->ReadData(MOJO_READ_DATA_FLAG_NONE, base::span(buffer), size);
      if (result == MOJO_RESULT_SHOULD_WAIT) {
        watcher_.ArmOrNotify();
        return;
      }
      if (result == MOJO_RESULT_FAILED_PRECONDITION) {
        eof_ = true;
        watcher_.Cancel();
        pipe_.reset();
        MaybeFinish();
        return;
      }
      if (result != MOJO_RESULT_OK || size > kMaximumBlob - bytes_.size()) {
        Fail();
        return;
      }
      bytes_.insert(bytes_.end(), buffer, buffer + size);
    }
    watcher_.ArmOrNotify();
  }
  mojo::ScopedDataPipeConsumerHandle pipe_;
  BlobReadCallback callback_;
  mojo::Remote<blink::mojom::Blob> blob_;
  mojo::Receiver<blink::mojom::BlobReaderClient> receiver_{this};
  mojo::SimpleWatcher watcher_{FROM_HERE,
                               mojo::SimpleWatcher::ArmingPolicy::MANUAL};
  BlobBytes bytes_;
  uint64_t expected_ = 0;
  bool require_status_ = false, status_received_ = false, eof_ = false;
  base::WeakPtrFactory<PipeReader> weak_{this};
};
class Blob final : public blink::mojom::Blob,
                   public network::mojom::DataPipeGetter {
public:
  explicit Blob(std::shared_ptr<Content> content)
      : content_(std::move(content)) {}
  void Clone(mojo::PendingReceiver<blink::mojom::Blob> receiver) override {
    mojo::MakeSelfOwnedReceiver(std::make_unique<Blob>(content_),
                                std::move(receiver));
  }
  void Clone(
      mojo::PendingReceiver<network::mojom::DataPipeGetter> receiver) override {
    mojo::MakeSelfOwnedReceiver(std::make_unique<Blob>(content_),
                                std::move(receiver));
  }
  void AsDataPipeGetter(
      mojo::PendingReceiver<network::mojom::DataPipeGetter> receiver) override {
    Clone(std::move(receiver));
  }
  void
  ReadAll(mojo::ScopedDataPipeProducerHandle pipe,
          mojo::PendingRemote<blink::mojom::BlobReaderClient> client) override {
    ReadRange(0, std::numeric_limits<uint64_t>::max(), std::move(pipe),
              std::move(client));
  }
  void ReadRange(
      uint64_t offset, uint64_t length, mojo::ScopedDataPipeProducerHandle pipe,
      mojo::PendingRemote<blink::mojom::BlobReaderClient> client) override {
    if (!content_->ready) {
      content_->waiting.push_back(
          base::BindOnce(&Blob::ReadRange, weak_.GetWeakPtr(), offset, length,
                         std::move(pipe), std::move(client)));
      return;
    }
    if (!content_->valid || offset > content_->bytes.size() ||
        transfers >= kMaximumTransfers) {
      if (client) {
        mojo::Remote<blink::mojom::BlobReaderClient> remote(std::move(client));
        remote->OnComplete(net::ERR_FAILED, 0);
      }
      return;
    }
    auto *writer = new PipeWriter(
        content_, offset,
        std::min<uint64_t>(length, content_->bytes.size() - offset),
        std::move(pipe), std::move(client));
    writer->Start();
  }
  void Read(mojo::ScopedDataPipeProducerHandle pipe,
            network::mojom::DataPipeGetter::ReadCallback callback) override {
    if (!content_->ready) {
      content_->waiting.push_back(
          base::BindOnce(&Blob::Read, weak_.GetWeakPtr(), std::move(pipe),
                         std::move(callback)));
      return;
    }
    if (!content_->valid) {
      std::move(callback).Run(base::unexpected(net::ERR_FAILED));
      return;
    }
    std::move(callback).Run(base::ByteSize(content_->bytes.size()));
    ReadAll(std::move(pipe), {});
  }
  void
  Load(mojo::PendingReceiver<network::mojom::URLLoader>, const std::string &,
       const net::HttpRequestHeaders &,
       mojo::PendingRemote<network::mojom::URLLoaderClient> client) override {
    mojo::Remote<network::mojom::URLLoaderClient> remote(std::move(client));
    remote->OnComplete(
        network::URLLoaderCompletionStatus(net::ERR_NOT_IMPLEMENTED));
  }
  void ReadSideData(ReadSideDataCallback callback) override {
    std::move(callback).Run(std::nullopt);
  }
  void CaptureSnapshot(CaptureSnapshotCallback callback) override {
    if (!content_->ready) {
      content_->waiting.push_back(base::BindOnce(
          &Blob::CaptureSnapshot, weak_.GetWeakPtr(), std::move(callback)));
      return;
    }
    std::move(callback).Run(content_->bytes.size(), std::nullopt);
  }
  void GetInternalUUID(GetInternalUUIDCallback callback) override {
    std::move(callback).Run(content_->uuid);
  }

private:
  std::shared_ptr<Content> content_;
  base::WeakPtrFactory<Blob> weak_{this};
};
// Construction is sequential so slices and asynchronously supplied bytes
// preserve order.
class Builder {
public:
  Builder(std::shared_ptr<Content> content,
          std::vector<blink::mojom::DataElementPtr> elements)
      : content_(std::move(content)), elements_(std::move(elements)) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(&Builder::Finish, weak_.GetWeakPtr(), false),
        base::Seconds(30));
  }
  void Start() { Next(); }

private:
  void Finish(bool success) {
    content_->Finish(success ? std::optional<BlobBytes>(std::move(bytes_))
                             : std::nullopt);
    delete this;
  }
  void Received(std::optional<BlobBytes> bytes) {
    if (!bytes || bytes->size() != expected_ ||
        bytes->size() > kMaximumBlob - bytes_.size()) {
      Finish(false);
      return;
    }
    bytes_.insert(bytes_.end(), bytes->begin(), bytes->end());
    Next();
  }
  void Next() {
    provider_.reset();
    if (index_ == elements_.size()) {
      Finish(true);
      return;
    }
    auto &element = elements_[index_++];
    if (element->is_bytes()) {
      auto &value = element->get_bytes();
      expected_ = value->length;
      if (expected_ > kMaximumBlob - bytes_.size()) {
        Finish(false);
        return;
      }
      if (value->embedded_data) {
        Received(std::move(*value->embedded_data));
        return;
      }
      provider_.Bind(std::move(value->data));
      provider_.set_disconnect_handler(
          base::BindOnce(&Builder::Finish, weak_.GetWeakPtr(), false));
      provider_->RequestAsReply(base::BindOnce(
          [](base::WeakPtr<Builder> builder, const BlobBytes &bytes) {
            if (builder)
              builder->Received(bytes);
          },
          weak_.GetWeakPtr()));
    } else if (element->is_blob()) {
      auto &value = element->get_blob();
      expected_ = value->length;
      if (expected_ > kMaximumBlob - bytes_.size()) {
        Finish(false);
        return;
      }
      ReadBlob(std::move(value->blob), value->offset, value->length,
               base::BindOnce(&Builder::Received, weak_.GetWeakPtr()));
    } else {
      Finish(false);
    }
  }
  std::shared_ptr<Content> content_;
  std::vector<blink::mojom::DataElementPtr> elements_;
  size_t index_ = 0;
  uint64_t expected_ = 0;
  BlobBytes bytes_;
  mojo::Remote<blink::mojom::BytesProvider> provider_;
  base::WeakPtrFactory<Builder> weak_{this};
};
class Registry final : public blink::mojom::BlobRegistry {
public:
  mojo::ReceiverSet<blink::mojom::BlobRegistry> receivers;
  void Register(mojo::PendingReceiver<blink::mojom::Blob> receiver,
                const std::string &,
                std::vector<blink::mojom::DataElementPtr> elements,
                RegisterCallback callback) override {
    auto content = std::make_shared<Content>();
    mojo::MakeSelfOwnedReceiver(std::make_unique<Blob>(content),
                                std::move(receiver));
    std::move(callback).Run(content->uuid);
    if (elements.size() > 1024) {
      content->Finish(std::nullopt);
      return;
    }
    auto *builder = new Builder(std::move(content), std::move(elements));
    builder->Start();
  }
  void RegisterFromStream(
      const std::string &type, uint64_t hint,
      mojo::ScopedDataPipeConsumerHandle pipe,
      mojo::PendingAssociatedRemote<blink::mojom::ProgressClient> progress,
      RegisterFromStreamCallback callback) override {
    if (hint > kMaximumBlob || transfers >= kMaximumTransfers) {
      std::move(callback).Run(nullptr);
      return;
    }
    auto *reader = new PipeReader(
        std::move(pipe),
        base::BindOnce(
            [](std::string type,
               mojo::PendingAssociatedRemote<blink::mojom::ProgressClient>
                   progress,
               RegisterFromStreamCallback callback,
               std::optional<BlobBytes> bytes) {
              if (!bytes) {
                std::move(callback).Run(nullptr);
                return;
              }
              if (progress) {
                mojo::AssociatedRemote<blink::mojom::ProgressClient> remote(
                    std::move(progress));
                remote->OnProgress(bytes->size());
              }
              std::move(callback).Run(MakeBlob(std::move(*bytes), type));
            },
            type, std::move(progress), std::move(callback)));
    reader->Start(0, 0);
  }
};
} // namespace
void BindBlobRegistry(
    mojo::PendingReceiver<blink::mojom::BlobRegistry> receiver) {
  static base::NoDestructor<Registry> registry;
  registry->receivers.Add(registry.get(), std::move(receiver));
}
blink::mojom::SerializedBlobPtr MakeBlob(BlobBytes bytes,
                                         const std::string &type) {
  auto content = std::make_shared<Content>();
  content->Finish(std::move(bytes));
  if (!content->valid)
    return nullptr;
  mojo::PendingRemote<blink::mojom::Blob> remote;
  mojo::MakeSelfOwnedReceiver(std::make_unique<Blob>(content),
                              remote.InitWithNewPipeAndPassReceiver());
  return blink::mojom::SerializedBlob::New(
      content->uuid, type, content->bytes.size(), std::move(remote));
}
void ReadBlob(mojo::PendingRemote<blink::mojom::Blob> blob, uint64_t offset,
              uint64_t length, BlobReadCallback callback) {
  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  if (!blob || transfers >= kMaximumTransfers ||
      mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  auto *reader =
      new PipeReader(std::move(consumer), std::move(callback), std::move(blob));
  reader->producer_ = std::move(producer);
  reader->Start(offset, length);
}
} // namespace uefi_browser
