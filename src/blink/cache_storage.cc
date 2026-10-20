#include "blink/cache_storage.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "blink/blob_service.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "services/network/public/mojom/cache_storage_side_data_writer.mojom.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/blob/blob.mojom.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/boringssl/src/include/openssl/sha.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <memory>
#include <unistd.h>

namespace uefi_browser {
namespace {
namespace api = blink::mojom;
using Error = api::CacheStorageError;
constexpr size_t kMaximumOriginBytes = 8 * 1024 * 1024;
constexpr size_t kMaximumCacheBytes = 64 * 1024 * 1024;
size_t cache_bytes = 0;
size_t live_entry_bytes = 0;
size_t live_caches = 0;
constexpr char kFormat[] = "UEFICS01:158.0.8110.0";
struct Entry {
  size_t accounted = 0;
  ~Entry() { live_entry_bytes -= accounted; }
  bool Account(size_t size) {
    if (size > kMaximumCacheBytes - (live_entry_bytes - accounted))
      return false;
    live_entry_bytes = live_entry_bytes - accounted + size;
    accounted = size;
    return true;
  }
  api::FetchAPIRequestPtr request;
  api::FetchAPIResponsePtr response;
  std::optional<BlobBytes> body;
};
struct CacheData {
  CacheData() { ++live_caches; }
  ~CacheData() { --live_caches; }
  std::u16string name;
  std::vector<std::shared_ptr<Entry>> entries;
};
void Append(BlobBytes &bytes, uint64_t value, unsigned length = 8) {
  for (unsigned index = 0; index < length; ++index)
    bytes.push_back(value >> (index * 8));
}
void AppendBytes(BlobBytes &bytes, base::span<const uint8_t> value) {
  Append(bytes, value.size());
  bytes.insert(bytes.end(), value.begin(), value.end());
}
struct Reader {
  base::span<const uint8_t> bytes;
  bool valid = true;
  uint64_t Number() {
    if (bytes.size() < 8) {
      valid = false;
      return 0;
    }
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
      value |= uint64_t(bytes[index]) << (8 * index);
    bytes = bytes.subspan(8u);
    return value;
  }
  base::span<const uint8_t> Part() {
    uint64_t size = Number();
    if (!valid || size > bytes.size()) {
      valid = false;
      return {};
    }
    auto value = bytes.first(size);
    bytes = bytes.subspan(size);
    return value;
  }
};
bool Transfer(int descriptor, BlobBytes &bytes, bool writing) {
  size_t offset = 0;
  while (offset < bytes.size()) {
    ssize_t result =
        writing
            ? write(descriptor, bytes.data() + offset, bytes.size() - offset)
            : read(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0)
      return false;
    offset += result;
  }
  return true;
}
std::string Stem(const std::string &identity) {
  uint8_t hash[32];
  SHA256(reinterpret_cast<const uint8_t *>(identity.data()), identity.size(),
         hash);
  std::string path = "/profile/cache-";
  const char *digits = "0123456789abcdef";
  for (auto byte : hash) {
    path += digits[byte >> 4];
    path += digits[byte & 15];
  }
  return path;
}
template <typename Headers>
std::optional<std::string> Header(const Headers &headers,
                                  std::string_view name) {
  for (const auto &[key, value] : headers)
    if (base::EqualsCaseInsensitiveASCII(key, name))
      return value;
  return std::nullopt;
}
bool Matches(const Entry &entry, const api::FetchAPIRequest *request,
             const api::CacheQueryOptions &options) {
  if (!request)
    return true;
  if (!options.ignore_method && request->method != "GET")
    return false;
  GURL::Replacements replacements;
  replacements.ClearRef();
  if (options.ignore_search)
    replacements.ClearQuery();
  if (entry.request->url.ReplaceComponents(replacements) !=
      request->url.ReplaceComponents(replacements))
    return false;
  if (!options.ignore_vary) {
    for (const auto &header : base::SplitString(
             Header(entry.response->headers, "vary").value_or(""), ",",
             base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      if (header == "*" || Header(entry.request->headers, header) !=
                               Header(request->headers, header))
        return false;
    }
  }
  return true;
}
template <typename T>
mojo::StructPtr<T> CopyMetadata(const mojo::StructPtr<T> &value) {
  // Stored metadata has no handles; bodies are owned separately and
  // reconstructed on reads.
  auto &input = const_cast<mojo::StructPtr<T> &>(value);
  auto message = T::SerializeAsMessage(&input);
  mojo::StructPtr<T> result;
  CHECK(T::DeserializeFromMessage(std::move(message), &result));
  return result;
}
api::FetchAPIResponsePtr Response(const Entry &entry) {
  auto response = CopyMetadata(entry.response);
  if (entry.body) {
    response->blob = MakeBlob(*entry.body, response->mime_type.value_or(""));
    if (!response->blob)
      return nullptr;
  }
  response->response_source =
      network::mojom::FetchResponseSource::kCacheStorage;
  return response;
}
api::CacheStorage::MatchResult
MatchEntry(const std::shared_ptr<CacheData> &cache,
           const api::FetchAPIRequest *request,
           const api::CacheQueryOptions &options) {
  for (const auto &entry : cache->entries)
    if (Matches(*entry, request, options)) {
      auto response = Response(*entry);
      if (!response)
        return base::unexpected(Error::kErrorQuotaExceeded);
      return api::MatchResponse::NewResponse(std::move(response));
    }
  return base::unexpected(Error::kErrorNotFound);
}
struct Origin {
  std::string identity;
  std::vector<std::shared_ptr<CacheData>> caches;
  uint64_t generation = 0;
  size_t accounted = 0;
  std::deque<base::OnceClosure> operations;
  bool busy = false;
  explicit Origin(std::string identity) : identity(std::move(identity)) {
    Load();
  }
  void Schedule(base::OnceClosure callback) {
    operations.push_back(std::move(callback));
    if (!busy)
      Next();
  }
  void Next() {
    busy = !operations.empty();
    if (!busy)
      return;
    auto callback = std::move(operations.front());
    operations.pop_front();
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, std::move(callback));
  }
  std::shared_ptr<CacheData> Find(const std::u16string &name) {
    for (auto &cache : caches)
      if (cache->name == name)
        return cache;
    return {};
  }
  BlobBytes Encode() {
    BlobBytes bytes(kFormat, kFormat + sizeof(kFormat));
    Append(bytes, generation + 1);
    AppendBytes(bytes, base::as_byte_span(identity));
    Append(bytes, caches.size());
    for (const auto &cache : caches) {
      AppendBytes(bytes, base::as_byte_span(cache->name));
      Append(bytes, cache->entries.size());
      for (const auto &entry : cache->entries) {
        auto request = CopyMetadata(entry->request);
        auto request_message =
            api::FetchAPIRequest::SerializeAsMessage(&request);
        AppendBytes(bytes, base::span(static_cast<const uint8_t *>(
                                          request_message.payload()),
                                      request_message.payload_num_bytes()));
        auto response = CopyMetadata(entry->response);
        auto response_message =
            api::FetchAPIResponse::SerializeAsMessage(&response);
        AppendBytes(bytes, base::span(static_cast<const uint8_t *>(
                                          response_message.payload()),
                                      response_message.payload_num_bytes()));
        Append(bytes, entry->body.has_value());
        if (bytes.size() > kMaximumOriginBytes ||
            (entry->body &&
             entry->body->size() > kMaximumOriginBytes - bytes.size()))
          return {};
        AppendBytes(bytes, entry->body ? base::span(*entry->body)
                                       : base::span<const uint8_t>());
      }
    }
    uint8_t hash[32];
    SHA256(bytes.data(), bytes.size(), hash);
    bytes.insert(bytes.end(), hash, hash + 32);
    return bytes;
  }
  Error Save() {
    auto bytes = Encode();
    if (bytes.empty() || bytes.size() > kMaximumOriginBytes ||
        bytes.size() > kMaximumCacheBytes - (cache_bytes - accounted))
      return Error::kErrorQuotaExceeded;
    std::string path =
        Stem(identity) + "-" + std::to_string((generation + 1) % 2);
    int descriptor =
        open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (descriptor < 0)
      return Error::kErrorStorage;
    bool success = Transfer(descriptor, bytes, true) && fsync(descriptor) == 0;
    if (close(descriptor) != 0)
      success = false;
    if (!success)
      return Error::kErrorStorage;
    cache_bytes = cache_bytes - accounted + bytes.size();
    accounted = bytes.size();
    ++generation;
    return Error::kSuccess;
  }
  void Load() {
    for (unsigned slot = 0; slot < 2; ++slot) {
      std::string path = Stem(identity) + "-" + std::to_string(slot);
      int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
      if (descriptor < 0)
        continue;
      off_t size = lseek(descriptor, 0, SEEK_END);
      if (size < 64 || size > static_cast<off_t>(kMaximumOriginBytes) ||
          lseek(descriptor, 0, SEEK_SET) < 0) {
        close(descriptor);
        continue;
      }
      BlobBytes bytes(size);
      bool success = Transfer(descriptor, bytes, false);
      close(descriptor);
      if (!success || memcmp(bytes.data(), kFormat, sizeof(kFormat)))
        continue;
      uint8_t hash[32];
      SHA256(bytes.data(), bytes.size() - 32, hash);
      if (memcmp(hash, bytes.data() + bytes.size() - 32, 32))
        continue;
      Reader reader{base::span(bytes).subspan(
          sizeof(kFormat), bytes.size() - sizeof(kFormat) - 32)};
      uint64_t next_generation = reader.Number();
      auto stored_identity = reader.Part();
      if (!reader.valid || next_generation <= generation ||
          stored_identity != base::as_byte_span(identity))
        continue;
      size_t count = reader.Number();
      if (count > 32 || count > 128 - std::min<size_t>(128, live_caches))
        continue;
      std::vector<std::shared_ptr<CacheData>> loaded;
      for (size_t index = 0; index < count && reader.valid; ++index) {
        auto name = reader.Part();
        if (name.size() % 2 || name.size() > 8192) {
          reader.valid = false;
          break;
        }
        auto cache = std::make_shared<CacheData>();
        cache->name.resize(name.size() / 2);
        memcpy(cache->name.data(), name.data(), name.size());
        for (const auto &previous : loaded)
          if (previous->name == cache->name)
            reader.valid = false;
        size_t entries = reader.Number();
        if (entries > 256) {
          reader.valid = false;
          break;
        }
        for (size_t item = 0; item < entries && reader.valid; ++item) {
          auto entry = std::make_shared<Entry>();
          auto request = reader.Part();
          auto response = reader.Part();
          bool has_body = reader.Number();
          auto body = reader.Part();
          if (!reader.valid ||
              !api::FetchAPIRequest::Deserialize(request, &entry->request) ||
              !api::FetchAPIResponse::Deserialize(response, &entry->response) ||
              !entry->request || !entry->response ||
              entry->request->method != "GET" ||
              !entry->request->url.SchemeIsHTTPOrHTTPS() ||
              entry->request->blob || entry->request->body ||
              entry->response->blob || entry->response->side_data_blob) {
            reader.valid = false;
            break;
          }
          if (has_body)
            entry->body = BlobBytes(body.begin(), body.end());
          if (!entry->Account(request.size() + response.size() + body.size())) {
            reader.valid = false;
            break;
          }
          cache->entries.push_back(std::move(entry));
        }
        loaded.push_back(std::move(cache));
      }
      if (reader.valid && reader.bytes.empty() &&
          bytes.size() <= kMaximumCacheBytes - (cache_bytes - accounted)) {
        caches = std::move(loaded);
        generation = next_generation;
        cache_bytes = cache_bytes - accounted + bytes.size();
        accounted = bytes.size();
      }
    }
    std::printf("CACHE: loaded generation=%llu caches=%zu\n",
                static_cast<unsigned long long>(generation), caches.size());
  }
};
class Batch {
public:
  Batch(Origin *origin, std::shared_ptr<CacheData> cache,
        std::vector<api::BatchOperationPtr> operations,
        api::CacheStorageCache::BatchCallback callback)
      : origin_(origin), cache_(std::move(cache)),
        operations_(std::move(operations)), callback_(std::move(callback)),
        candidate_(cache_->entries) {}
  void Start() { Next(); }

private:
  void Finish(Error error) {
    auto callback = std::move(callback_);
    Origin *origin = origin_;
    delete this;
    std::move(callback).Run(
        api::CacheStorageVerboseError::New(error, std::nullopt));
    origin->Next();
  }
  void Received(std::shared_ptr<Entry> entry, std::optional<BlobBytes> bytes) {
    if (!bytes) {
      Finish(Error::kErrorStorage);
      return;
    }
    if (!entry->Account(entry->accounted + bytes->size())) {
      Finish(Error::kErrorQuotaExceeded);
      return;
    }
    entry->body = std::move(bytes);
    candidate_.push_back(std::move(entry));
    Next();
  }
  void Next() {
    if (index_ == operations_.size()) {
      auto previous = std::move(cache_->entries);
      cache_->entries = std::move(candidate_);
      Error error = origin_->Save();
      if (error != Error::kSuccess)
        cache_->entries = std::move(previous);
      Finish(error);
      return;
    }
    auto &operation = operations_[index_++];
    if (!operation->request || !operation->request->url.SchemeIsHTTPOrHTTPS()) {
      Finish(Error::kErrorStorage);
      return;
    }
    api::CacheQueryOptions options;
    if (operation->operation_type == api::OperationType::kDelete) {
      if (operations_.size() != 1 || !operation->match_options) {
        Finish(Error::kErrorStorage);
        return;
      }
      size_t previous = candidate_.size();
      std::erase_if(candidate_, [&](const auto &entry) {
        return Matches(*entry, operation->request.get(),
                       *operation->match_options);
      });
      if (previous == candidate_.size()) {
        Finish(Error::kErrorNotFound);
        return;
      }
      Next();
      return;
    }
    if (operation->request->blob || operation->request->body ||
        operation->operation_type != api::OperationType::kPut ||
        !operation->response || operation->request->method != "GET" ||
        operation->response->status_code == 206) {
      Finish(Error::kErrorStorage);
      return;
    }
    for (const auto &header : base::SplitString(
             Header(operation->response->headers, "vary").value_or(""), ",",
             base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      if (header == "*") {
        Finish(Error::kErrorStorage);
        return;
      }
    }
    for (const auto &entry : inserted_)
      if (Matches(*entry, operation->request.get(), options)) {
        Finish(Error::kErrorDuplicateOperation);
        return;
      }
    std::erase_if(candidate_, [&](const auto &entry) {
      return Matches(*entry, operation->request.get(), options);
    });
    if (candidate_.size() >= 256) {
      Finish(Error::kErrorQuotaExceeded);
      return;
    }
    auto entry = std::make_shared<Entry>();
    entry->request = std::move(operation->request);
    entry->response = std::move(operation->response);
    size_t metadata_size = entry->request->url.spec().size() +
                           entry->response->status_text.size() + 1024;
    for (const auto &[name, value] : entry->request->headers)
      metadata_size += name.size() + value.size();
    for (const auto &[name, value] : entry->response->headers)
      metadata_size += name.size() + value.size();
    if (metadata_size > 65536 || !entry->Account(metadata_size)) {
      Finish(Error::kErrorQuotaExceeded);
      return;
    }
    inserted_.push_back(entry);
    auto blob = std::move(entry->response->blob);
    entry->response->side_data_blob.reset();
    entry->response->side_data_for_cache_put.reset();
    entry->response->cache_storage_side_data_writer.reset();
    if (blob) {
      ReadBlob(std::move(blob->blob), 0, blob->size,
               base::BindOnce(&Batch::Received, base::Unretained(this), entry));
      return;
    }
    candidate_.push_back(std::move(entry));
    Next();
  }
  Origin *origin_;
  std::shared_ptr<CacheData> cache_;
  std::vector<api::BatchOperationPtr> operations_;
  api::CacheStorageCache::BatchCallback callback_;
  std::vector<std::shared_ptr<Entry>> candidate_, inserted_;
  size_t index_ = 0;
};
class Cache final : public api::CacheStorageCache {
public:
  Cache(Origin *origin, std::shared_ptr<CacheData> data)
      : origin_(origin), data_(std::move(data)) {}
  void Match(api::FetchAPIRequestPtr request, api::CacheQueryOptionsPtr options,
             bool, bool, int64_t, MatchCallback callback) override {
    origin_->Schedule(base::BindOnce(
        [](Origin *origin, std::shared_ptr<CacheData> data,
           api::FetchAPIRequestPtr request, api::CacheQueryOptionsPtr options,
           MatchCallback callback) {
          std::move(callback).Run(MatchEntry(data, request.get(), *options));
          origin->Next();
        },
        origin_, data_, std::move(request), std::move(options),
        std::move(callback)));
  }
  void MatchAll(api::FetchAPIRequestPtr request,
                api::CacheQueryOptionsPtr options, int64_t,
                MatchAllCallback callback) override {
    origin_->Schedule(base::BindOnce(
        [](Origin *origin, std::shared_ptr<CacheData> data,
           api::FetchAPIRequestPtr request, api::CacheQueryOptionsPtr options,
           MatchAllCallback callback) {
          std::vector<api::FetchAPIResponsePtr> result;
          for (const auto &entry : data->entries)
            if (Matches(*entry, request.get(), *options)) {
              auto response = Response(*entry);
              if (!response) {
                std::move(callback).Run(
                    base::unexpected(Error::kErrorQuotaExceeded));
                origin->Next();
                return;
              }
              result.push_back(std::move(response));
            }
          std::move(callback).Run(std::move(result));
          origin->Next();
        },
        origin_, data_, std::move(request), std::move(options),
        std::move(callback)));
  }
  void GetAllMatchedEntries(api::FetchAPIRequestPtr, api::CacheQueryOptionsPtr,
                            int64_t,
                            GetAllMatchedEntriesCallback callback) override {
    // This browser-internal inspection API is not exposed by the JavaScript
    // Cache API.
    std::move(callback).Run(base::unexpected(Error::kErrorStorage));
  }
  void Keys(api::FetchAPIRequestPtr request, api::CacheQueryOptionsPtr options,
            int64_t, KeysCallback callback) override {
    origin_->Schedule(base::BindOnce(
        [](Origin *origin, std::shared_ptr<CacheData> data,
           api::FetchAPIRequestPtr request, api::CacheQueryOptionsPtr options,
           KeysCallback callback) {
          std::vector<api::FetchAPIRequestPtr> result;
          for (const auto &entry : data->entries)
            if (Matches(*entry, request.get(), *options))
              result.push_back(CopyMetadata(entry->request));
          std::move(callback).Run(std::move(result));
          origin->Next();
        },
        origin_, data_, std::move(request), std::move(options),
        std::move(callback)));
  }
  void Batch(std::vector<api::BatchOperationPtr> operations, int64_t,
             BatchCallback callback) override {
    if (operations.empty() || operations.size() > 256 ||
        origin_->operations.size() >= 64) {
      std::move(callback).Run(api::CacheStorageVerboseError::New(
          Error::kErrorQuotaExceeded, std::nullopt));
      return;
    }
    origin_->Schedule(base::BindOnce(
        [](Origin *origin, std::shared_ptr<CacheData> data,
           std::vector<api::BatchOperationPtr> operations,
           BatchCallback callback) {
          auto *batch = new uefi_browser::Batch(origin, std::move(data),
                                                std::move(operations),
                                                std::move(callback));
          batch->Start();
        },
        origin_, data_, std::move(operations), std::move(callback)));
  }

private:
  Origin *origin_;
  std::shared_ptr<CacheData> data_;
};
class Storage final : public api::CacheStorage {
public:
  explicit Storage(Origin *origin) : origin_(origin) {}
  void Has(const std::u16string &name, int64_t, HasCallback callback) override {
    origin_->Schedule(base::BindOnce(
        [](Origin *origin, std::u16string name, HasCallback callback) {
          std::move(callback).Run(origin->Find(name) ? Error::kSuccess
                                                     : Error::kErrorNotFound);
          origin->Next();
        },
        origin_, name, std::move(callback)));
  }
  void Delete(const std::u16string &name, int64_t,
              DeleteCallback callback) override {
    origin_->Schedule(base::BindOnce(
        [](Origin *origin, std::u16string name, DeleteCallback callback) {
          Error error = Error::kErrorNotFound;
          if (origin->Find(name)) {
            auto previous = origin->caches;
            std::erase_if(origin->caches, [&](const auto &cache) {
              return cache->name == name;
            });
            error = origin->Save();
            if (error != Error::kSuccess)
              origin->caches = std::move(previous);
          }
          std::move(callback).Run(error);
          origin->Next();
        },
        origin_, name, std::move(callback)));
  }
  void Keys(int64_t, KeysCallback callback) override {
    origin_->Schedule(base::BindOnce(
        [](Origin *origin, KeysCallback callback) {
          std::vector<std::u16string> names;
          for (auto &cache : origin->caches)
            names.push_back(cache->name);
          std::move(callback).Run(names);
          origin->Next();
        },
        origin_, std::move(callback)));
  }
  void Match(api::FetchAPIRequestPtr request,
             api::MultiCacheQueryOptionsPtr options, bool, bool, int64_t,
             MatchCallback callback) override {
    origin_->Schedule(base::BindOnce(
        [](Origin *origin, api::FetchAPIRequestPtr request,
           api::MultiCacheQueryOptionsPtr options, MatchCallback callback) {
          for (const auto &cache : origin->caches)
            if (!options->cache_name || cache->name == *options->cache_name) {
              auto result =
                  MatchEntry(cache, request.get(), *options->query_options);
              if (result.has_value() ||
                  result.error() != Error::kErrorNotFound) {
                std::move(callback).Run(std::move(result));
                origin->Next();
                return;
              }
            }
          std::move(callback).Run(base::unexpected(Error::kErrorNotFound));
          origin->Next();
        },
        origin_, std::move(request), std::move(options), std::move(callback)));
  }
  void Open(const std::u16string &name, int64_t,
            OpenCallback callback) override {
    origin_->Schedule(base::BindOnce(
        [](Origin *origin, std::u16string name, OpenCallback callback) {
          auto cache = origin->Find(name);
          if (!cache) {
            if (origin->caches.size() >= 32 || live_caches >= 128 ||
                name.size() > 4096) {
              std::move(callback).Run(
                  base::unexpected(Error::kErrorQuotaExceeded));
              origin->Next();
              return;
            }
            cache = std::make_shared<CacheData>();
            cache->name = name;
            origin->caches.push_back(cache);
            auto error = origin->Save();
            if (error != Error::kSuccess) {
              origin->caches.pop_back();
              std::move(callback).Run(base::unexpected(error));
              origin->Next();
              return;
            }
          }
          mojo::PendingAssociatedRemote<api::CacheStorageCache> remote;
          mojo::MakeSelfOwnedAssociatedReceiver(
              std::make_unique<Cache>(origin, cache),
              remote.InitWithNewEndpointAndPassReceiver());
          std::move(callback).Run(std::move(remote));
          origin->Next();
        },
        origin_, name, std::move(callback)));
  }

private:
  Origin *origin_;
};
} // namespace
void BindCacheStorage(blink::LocalFrameToken token,
                      mojo::PendingReceiver<api::CacheStorage> receiver) {
  auto *frame = blink::LocalFrame::FromFrameToken(token);
  if (!frame || !frame->DomWindow())
    return;
  auto key = frame->DomWindow()->GetStorageKey();
  if (key.GetSecurityOrigin()->IsOpaque() ||
      !key.GetSecurityOrigin()->IsPotentiallyTrustworthy())
    return;
  std::string identity = static_cast<blink::StorageKey>(key).Serialize();
  static base::NoDestructor<std::map<std::string, std::unique_ptr<Origin>>>
      origins;
  auto found = origins->find(identity);
  if (found == origins->end()) {
    if (origins->size() >= 32)
      return;
    found =
        origins->emplace(identity, std::make_unique<Origin>(identity)).first;
  }
  mojo::MakeSelfOwnedReceiver(std::make_unique<Storage>(found->second.get()),
                              std::move(receiver));
}
} // namespace uefi_browser
