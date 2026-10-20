#include "blink/web_storage.h"
#include "base/check.h"
#include "base/no_destructor.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote_set.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/boringssl/src/include/openssl/sha.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace blink {
namespace {
using Bytes = std::vector<uint8_t>;
using Entries = std::map<Bytes, Bytes>;
constexpr size_t kMaximumBytes =
    mojom::blink::StorageArea::kPerStorageAreaQuota +
    mojom::blink::StorageArea::kPerStorageAreaOverQuotaAllowance;
constexpr size_t kMaximumEntries = 16384;
constexpr size_t kMaximumAreas = 64;
Bytes Copy(const Vector<uint8_t> &bytes) {
  return Bytes(bytes.begin(), bytes.end());
}
Vector<uint8_t> Copy(const Bytes &bytes) { return Vector<uint8_t>(bytes); }
void Append(Bytes &output, uint64_t value, unsigned count) {
  for (unsigned index = 0; index < count; ++index)
    output.push_back(value >> (8 * index));
}
uint64_t Read(const Bytes &input, size_t &offset, unsigned count) {
  if (offset > input.size() || count > input.size() - offset) {
    offset = input.size() + 1;
    return 0;
  }
  uint64_t value = 0;
  for (unsigned index = 0; index < count; ++index)
    value |= uint64_t(input[offset++]) << (8 * index);
  return value;
}
std::string FileStem(const std::string &identity) {
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t *>(identity.data()), identity.size(),
         digest);
  constexpr char digits[] = "0123456789abcdef";
  std::string path = "/profile/local-";
  for (uint8_t byte : digest) {
    path += digits[byte >> 4];
    path += digits[byte & 15];
  }
  return path;
}
bool Transfer(int descriptor, void *memory, size_t length, bool writing) {
  auto *data = static_cast<uint8_t *>(memory);
  while (length) {
    ssize_t result = writing ? write(descriptor, data, length)
                             : read(descriptor, data, length);
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0)
      return false;
    data += result;
    length -= result;
  }
  return true;
}
struct Snapshot {
  uint64_t generation = 0;
  Entries entries;
};
Snapshot Load(const std::string &path, const std::string &identity) {
  Snapshot result;
  int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0)
    return result;
  off_t size = lseek(descriptor, 0, SEEK_END);
  if (size < 48 || size > static_cast<off_t>(kMaximumBytes + 1024 * 1024) ||
      lseek(descriptor, 0, SEEK_SET) < 0) {
    close(descriptor);
    return result;
  }
  Bytes bytes(size);
  bool read_ok = Transfer(descriptor, bytes.data(), bytes.size(), false);
  close(descriptor);
  if (!read_ok || std::memcmp(bytes.data(), "UEFILS01", 8))
    return result;
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(bytes.data(), bytes.size() - 32, digest);
  if (std::memcmp(digest, bytes.data() + bytes.size() - 32, 32))
    return result;
  bytes.resize(bytes.size() - 32);
  size_t offset = 8;
  uint64_t generation = Read(bytes, offset, 8);
  size_t name_length = Read(bytes, offset, 4);
  if (!generation || name_length != identity.size() || offset > bytes.size() ||
      name_length > bytes.size() - offset ||
      std::memcmp(bytes.data() + offset, identity.data(), name_length))
    return result;
  offset += name_length;
  size_t count = Read(bytes, offset, 4), total = 0;
  if (count > kMaximumEntries)
    return result;
  Entries entries;
  for (size_t index = 0; index < count; ++index) {
    size_t key_length = Read(bytes, offset, 4),
           value_length = Read(bytes, offset, 4);
    if (offset > bytes.size() || key_length > bytes.size() - offset ||
        value_length > bytes.size() - offset - key_length ||
        key_length + value_length > kMaximumBytes - total)
      return result;
    Bytes key(bytes.begin() + offset, bytes.begin() + offset + key_length);
    offset += key_length;
    Bytes value(bytes.begin() + offset, bytes.begin() + offset + value_length);
    offset += value_length;
    total += key_length + value_length;
    if (!entries.emplace(std::move(key), std::move(value)).second)
      return result;
  }
  if (offset != bytes.size())
    return result;
  return Snapshot{generation, std::move(entries)};
}
class Area final : public mojom::blink::StorageArea {
public:
  Area(std::string identity, bool persistent)
      : identity_(std::move(identity)), persistent_(persistent) {
    if (!persistent_)
      return;
    auto first = Load(FileStem(identity_) + "-0", identity_);
    auto second = Load(FileStem(identity_) + "-1", identity_);
    auto &current = first.generation > second.generation ? first : second;
    generation_ = current.generation;
    entries_ = std::move(current.entries);
    std::printf("STORAGE: loaded generation=%llu entries=%zu\n",
                static_cast<unsigned long long>(generation_), entries_.size());
  }
  std::unique_ptr<Area> CloneSession() const {
    auto result = std::make_unique<Area>(identity_, false);
    result->entries_ = entries_;
    return result;
  }
  void Bind(mojo::PendingReceiver<mojom::blink::StorageArea> receiver) {
    receivers_.Add(this, std::move(receiver));
  }
  void AddObserver(mojo::PendingRemote<mojom::blink::StorageAreaObserver>
                       observer) override {
    if (observer) {
      mojo::Remote<mojom::blink::StorageAreaObserver> remote(
          std::move(observer));
      if (persistent_)
        remote->ShouldSendOldValueOnMutations(false);
      observers_.Add(std::move(remote));
    }
  }
  void Put(const Vector<uint8_t> &key, const Vector<uint8_t> &value,
           const std::optional<Vector<uint8_t>> &,
           mojom::blink::StorageAreaSourcePtr source,
           PutCallback callback) override {
    Bytes stored_key = Copy(key), stored_value = Copy(value);
    auto found = entries_.find(stored_key);
    size_t total = 0;
    for (const auto &entry : entries_)
      total += entry.first.size() + entry.second.size();
    if (found != entries_.end())
      total -= found->first.size() + found->second.size();
    if (key.size() + value.size() > kMaximumBytes - total ||
        (found == entries_.end() && entries_.size() >= kMaximumEntries)) {
      if (persistent_)
        for (auto &observer : observers_)
          observer->KeyChangeFailed(key, source.Clone());
      std::move(callback).Run(false);
      return;
    }
    std::optional<Vector<uint8_t>> old;
    if (found != entries_.end())
      old = Copy(found->second);
    entries_[std::move(stored_key)] = std::move(stored_value);
    Save();
    if (persistent_)
      for (auto &observer : observers_)
        observer->KeyChanged(key, value, old, source.Clone());
    std::move(callback).Run(true);
  }
  void Delete(const Vector<uint8_t> &key,
              const std::optional<Vector<uint8_t>> &,
              mojom::blink::StorageAreaSourcePtr source,
              DeleteCallback callback) override {
    auto found = entries_.find(Copy(key));
    std::optional<Vector<uint8_t>> old;
    if (found != entries_.end()) {
      old = Copy(found->second);
      entries_.erase(found);
      Save();
    }
    if (persistent_)
      for (auto &observer : observers_)
        observer->KeyDeleted(key, old, source.Clone());
    std::move(callback).Run();
  }
  void
  DeleteAll(mojom::blink::StorageAreaSourcePtr source,
            mojo::PendingRemote<mojom::blink::StorageAreaObserver> observer,
            DeleteAllCallback callback) override {
    AddObserver(std::move(observer));
    bool nonempty = !entries_.empty();
    entries_.clear();
    Save();
    if (persistent_)
      for (auto &remote : observers_)
        remote->AllDeleted(nonempty, source.Clone());
    std::move(callback).Run();
  }
  void GetAll(mojo::PendingRemote<mojom::blink::StorageAreaObserver> observer,
              GetAllCallback callback) override {
    AddObserver(std::move(observer));
    Vector<mojom::blink::KeyValuePtr> data;
    for (const auto &entry : entries_)
      data.push_back(
          mojom::blink::KeyValue::New(Copy(entry.first), Copy(entry.second)));
    std::move(callback).Run(std::move(data));
  }

private:
  void Save() {
    if (!persistent_)
      return;
    // Alternate slots retain the preceding valid snapshot if writing is
    // interrupted.
    uint64_t next = generation_ + 1;
    if (!next)
      return;
    Bytes bytes = {'U', 'E', 'F', 'I', 'L', 'S', '0', '1'};
    Append(bytes, next, 8);
    Append(bytes, identity_.size(), 4);
    bytes.insert(bytes.end(), identity_.begin(), identity_.end());
    Append(bytes, entries_.size(), 4);
    for (const auto &entry : entries_) {
      Append(bytes, entry.first.size(), 4);
      Append(bytes, entry.second.size(), 4);
      bytes.insert(bytes.end(), entry.first.begin(), entry.first.end());
      bytes.insert(bytes.end(), entry.second.begin(), entry.second.end());
    }
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(bytes.data(), bytes.size(), digest);
    bytes.insert(bytes.end(), digest, digest + 32);
    std::string path = FileStem(identity_) + "-" + std::to_string(next & 1);
    int descriptor =
        open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    bool success = descriptor >= 0 &&
                   Transfer(descriptor, bytes.data(), bytes.size(), true) &&
                   fsync(descriptor) == 0;
    if (descriptor >= 0 && close(descriptor))
      success = false;
    if (success)
      generation_ = next;
    else
      std::puts(
          "STORAGE: persistence failed; changes remain in renderer memory");
  }
  std::string identity_;
  bool persistent_;
  uint64_t generation_ = 0;
  Entries entries_;
  mojo::ReceiverSet<mojom::blink::StorageArea> receivers_;
  mojo::RemoteSet<mojom::blink::StorageAreaObserver> observers_;
};
using AreaMap = std::map<std::string, std::unique_ptr<Area>>;
AreaMap &Areas() {
  static base::NoDestructor<AreaMap> areas;
  return *areas;
}
class Namespace final : public mojom::blink::SessionStorageNamespace {
public:
  explicit Namespace(std::string identity) : identity_(std::move(identity)) {}
  void
  Bind(mojo::PendingReceiver<mojom::blink::SessionStorageNamespace> receiver) {
    receivers_.Add(this, std::move(receiver));
  }
  void Clone(const String &destination) override {
    std::string target = destination.Utf8();
    if (target.empty() || target.size() > 256 || target == identity_)
      return;
    std::string source_prefix = "session:" + identity_ + ":";
    std::string target_prefix = "session:" + target + ":";
    AreaMap copies;
    for (const auto &entry : Areas()) {
      if (entry.first.starts_with(target_prefix))
        return;
      if (entry.first.starts_with(source_prefix))
        copies.emplace(target_prefix + entry.first.substr(source_prefix.size()),
                       entry.second->CloneSession());
    }
    if (copies.size() > kMaximumAreas - Areas().size()) {
      std::puts("STORAGE: namespace clone exceeds area limit");
      return;
    }
    Areas().merge(copies);
  }

private:
  std::string identity_;
  mojo::ReceiverSet<mojom::blink::SessionStorageNamespace> receivers_;
};
using NamespaceMap = std::map<std::string, std::unique_ptr<Namespace>>;
NamespaceMap &Namespaces() {
  static base::NoDestructor<NamespaceMap> namespaces;
  return *namespaces;
}

} // namespace
void BindFirmwareStorageArea(
    const BlinkStorageKey &key, const LocalFrameToken &token,
    const String &session,
    mojo::PendingReceiver<mojom::blink::StorageArea> receiver) {
  auto *frame = LocalFrame::FromFrameToken(token);
  if (!frame || !frame->DomWindow() ||
      !key.ExactMatchForTesting(frame->DomWindow()->GetStorageKey()) ||
      key.GetSecurityOrigin()->IsOpaque())
    return;
  StorageKey public_key = key;
  std::string identity = public_key.Serialize();
  std::string map_key = session.empty()
                            ? "local:" + identity
                            : "session:" + session.Utf8() + ":" + identity;
  auto &areas = Areas();
  auto found = areas.find(map_key);
  if (found == areas.end()) {
    if (areas.size() >= kMaximumAreas) {
      std::puts("STORAGE: area limit reached");
      return;
    }
    found =
        areas
            .emplace(map_key, std::make_unique<Area>(identity, session.empty()))
            .first;
  }
  found->second->Bind(std::move(receiver));
}
void BindFirmwareStorageNamespace(
    const String &session,
    mojo::PendingReceiver<mojom::blink::SessionStorageNamespace> receiver) {
  std::string identity = session.Utf8();
  if (identity.empty() || identity.size() > 256)
    return;
  auto &namespaces = Namespaces();
  auto found = namespaces.find(identity);
  if (found == namespaces.end()) {
    if (namespaces.size() >= kMaximumAreas)
      return;
    found = namespaces.emplace(identity, std::make_unique<Namespace>(identity))
                .first;
  }
  found->second->Bind(std::move(receiver));
}
void ReleaseFirmwareStorageNamespace(const std::string &session) {
  Namespaces().erase(session);
  std::string prefix = "session:" + session + ":";
  std::erase_if(Areas(), [&](const auto &item) {
    return item.first.starts_with(prefix);
  });
}
} // namespace blink
