#include "blink/indexed_db.h"
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "blink/blob_service.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/common/indexeddb/indexed_db_default_mojom_traits.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/blob/blob.mojom.h"
#include "third_party/blink/public/mojom/blob/serialized_blob.mojom.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_transfer_token.mojom.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/boringssl/src/include/openssl/sha.h"

namespace uefi_browser {
namespace {
namespace api = blink::mojom;
using Key = blink::IndexedDBKey;
using Range = blink::IndexedDBKeyRange;
using Mode = api::IDBTransactionMode;
using Exception = api::IDBException;
using Metadata = blink::IndexedDBDatabaseMetadata;
constexpr size_t kMaximumSnapshot = 8 * 1024 * 1024;
constexpr size_t kMaximumRecords = 4096;
constexpr char kFormat[] = "UEFIIDB1:158.0.8110.0";
api::IDBErrorPtr Error(
    Exception code,
    const std::u16string& message = u"IndexedDB operation failed") {
  return api::IDBError::New(code, message);
}
void Post(base::OnceClosure callback) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, std::move(callback));
}
struct External {
  BlobBytes bytes;
  std::u16string type;
  api::IDBFileInfoPtr file;
};
struct Record {
  Key key;
  BlobBytes bits;
  std::vector<std::shared_ptr<External>> external;
  std::map<int64_t, std::shared_ptr<std::vector<Key>>> indexes;
};
struct Store {
  uint64_t generator = 1;
  std::vector<std::shared_ptr<Record>> records;
};
struct Data {
  Metadata metadata;
  std::map<int64_t, Store> stores;
};
struct Buffer {
  BlobBytes bytes;
  bool valid = true;
  void Number(uint64_t value) {
    if (bytes.size() > kMaximumSnapshot - 8) {
      valid = false;
      return;
    }
    for (unsigned index = 0; index < 8; ++index)
      bytes.push_back(value >> (8 * index));
  }
  void Part(base::span<const uint8_t> value) {
    Number(value.size());
    if (!valid || value.size() > kMaximumSnapshot - bytes.size()) {
      valid = false;
      return;
    }
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
  template <class Schema, class Value>
  void Object(Value& value) {
    auto message = [&] {
      if constexpr (std::is_same_v<Schema, api::IDBKey>) {
        Range range(value.Clone(), value.Clone(), false, false);
        return api::IDBKeyRange::SerializeAsMessage(&range);
      } else
        return Schema::SerializeAsMessage(&value);
    }();
    Part(base::span(static_cast<const uint8_t*>(message.payload()),
                    message.payload_num_bytes()));
  }
};
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
    auto result = bytes.first(size);
    bytes = bytes.subspan(size);
    return result;
  }
  template <class Schema, class Value>
  void Object(Value& value) {
    auto part = Part();
    if (!valid)
      return;
    if constexpr (std::is_same_v<Schema, api::IDBKey>) {
      Range range;
      if (!api::IDBKeyRange::Deserialize(part, &range))
        valid = false;
      else
        value = range.lower().Clone();
    } else if (!Schema::Deserialize(part, &value))
      valid = false;
  }
};
struct Slot;
struct Origin {
  std::string identity;
  std::map<std::u16string, std::shared_ptr<Data>> databases;
  std::map<std::u16string, std::unique_ptr<Slot>> slots;
  uint64_t generation = 0;
  explicit Origin(std::string identity);
  ~Origin();
  Exception Save();
  void Load();
  Slot* Get(const std::u16string& name);
};
class Connection;
class Transaction;
struct OpenRequest {
  mojo::AssociatedRemote<api::IDBFactoryClient> client;
  mojo::PendingAssociatedRemote<api::IDBDatabaseCallbacks> callbacks;
  mojo::PendingAssociatedReceiver<api::IDBTransaction> transaction;
  int64_t version = 0, transaction_id = 0;
  bool deleting = false, notified = false, started = false;
  bool blocked_sent = false;
};
struct Slot {
  Origin* origin;
  std::u16string name;
  std::map<uint64_t, std::unique_ptr<Connection>> connections;
  std::deque<std::shared_ptr<OpenRequest>> opens;
  std::deque<std::shared_ptr<Transaction>> transactions;
  uint64_t next_connection = 1;
  bool active = false;
  bool closing = false;
  Slot(Origin* origin, std::u16string name)
      : origin(origin), name(std::move(name)) {}
  std::shared_ptr<Data> Current() {
    auto found = origin->databases.find(name);
    if (found != origin->databases.end())
      return found->second;
    auto data = std::make_shared<Data>();
    data->metadata.name = name;
    data->metadata.version = 0;
    return data;
  }
  void ProcessOpen();
  void Remove(uint64_t identifier);
  void StartTransaction();
  void Finished(Transaction* transaction);
};
bool Contains(const Range& range, const Key& key) {
  return (!range.lower().IsValid() ||
          (!key.IsLessThan(range.lower()) &&
           (!range.lower_open() || !key.Equals(range.lower())))) &&
         (!range.upper().IsValid() ||
          (!range.upper().IsLessThan(key) &&
           (!range.upper_open() || !key.Equals(range.upper()))));
}
api::IDBValuePtr Value(const Record& record) {
  auto value = api::IDBValue::New();
  value->bits = mojo_base::BigBuffer(base::span(record.bits));
  for (const auto& external : record.external) {
    auto blob = MakeBlob(external->bytes, "");
    if (!blob)
      return nullptr;
    auto info = api::IDBBlobInfo::New(
        std::move(blob->blob), external->type, external->bytes.size(),
        external->file ? external->file->Clone() : nullptr);
    value->external_objects.push_back(
        api::IDBExternalObject::NewBlobOrFile(std::move(info)));
  }
  return value;
}
api::IDBReturnValuePtr ReturnValue(
    api::IDBValuePtr value,
    const Record& record,
    const blink::IndexedDBObjectStoreMetadata& metadata) {
  bool inject = metadata.auto_increment &&
                metadata.key_path.type() != api::IDBKeyPathType::Null;
  return api::IDBReturnValue::New(
      std::move(value), inject ? record.key.Clone() : Key(), metadata.key_path);
}
struct Row {
  Key key;
  std::shared_ptr<Record> record;
};
std::vector<Row> Rows(Data& data,
                      int64_t store_id,
                      int64_t index_id,
                      const Range& range,
                      api::IDBCursorDirection direction) {
  std::vector<Row> result;
  auto found = data.stores.find(store_id);
  if (found == data.stores.end())
    return result;
  for (auto& record : found->second.records) {
    if (index_id == -1) {
      if (Contains(range, record->key))
        result.push_back({record->key.Clone(), record});
    } else if (auto keys = record->indexes.find(index_id);
               keys != record->indexes.end()) {
      for (auto& key : *keys->second)
        if (Contains(range, key))
          result.push_back({key.Clone(), record});
    }
  }
  std::sort(result.begin(), result.end(),
            [](const Row& left, const Row& right) {
              if (!left.key.Equals(right.key))
                return left.key.IsLessThan(right.key);
              return left.record->key.IsLessThan(right.record->key);
            });
  bool reverse = direction == api::IDBCursorDirection::Prev ||
                 direction == api::IDBCursorDirection::PrevNoDuplicate;
  if (direction == api::IDBCursorDirection::NextNoDuplicate ||
      direction == api::IDBCursorDirection::PrevNoDuplicate) {
    result.erase(std::unique(result.begin(), result.end(),
                             [](auto& left, auto& right) {
                               return left.key.Equals(right.key);
                             }),
                 result.end());
  }
  if (reverse)
    std::reverse(result.begin(), result.end());
  return result;
}
class Transaction final : public api::IDBTransaction,
                          public std::enable_shared_from_this<Transaction> {
 public:
  Transaction(Slot* slot,
              uint64_t connection,
              int64_t identifier,
              Mode mode,
              std::set<int64_t> scope)
      : slot(slot),
        connection(connection),
        identifier(identifier),
        mode(mode),
        scope(std::move(scope)) {}
  Slot* slot;
  uint64_t connection;
  int64_t identifier;
  Mode mode;
  std::set<int64_t> scope;
  std::shared_ptr<Data> data;
  std::shared_ptr<OpenRequest> opening;
  mojo::AssociatedReceiver<api::IDBTransaction> receiver{this};
  bool running = false, busy = false, finished = false;
  uint64_t errors = 0;
  std::deque<base::OnceClosure> operations, index_operations;
  unsigned pending_indexes = 0;
  void Queue(base::OnceClosure callback, bool preemptive = false) {
    if (finished)
      return;
    (preemptive ? index_operations : operations).push_back(std::move(callback));
    Pump();
  }
  void Pump() {
    auto& queue = pending_indexes || !index_operations.empty()
                      ? index_operations
                      : operations;
    if (!running || busy || finished || queue.empty())
      return;
    busy = true;
    auto callback = std::move(queue.front());
    queue.pop_front();
    Post(base::BindOnce(
        [](std::shared_ptr<Transaction> self, base::OnceClosure callback) {
          if (!self->finished)
            std::move(callback).Run();
        },
        shared_from_this(), std::move(callback)));
  }
  void Done() {
    busy = false;
    Pump();
  }
  bool Allowed(int64_t store, bool write = false) const {
    return !finished && data && data->stores.contains(store) &&
           (mode == Mode::VersionChange || scope.contains(store)) &&
           (!write || mode != Mode::ReadOnly);
  }
  void Finish(Exception error);
  void CreateObjectStore(int64_t store,
                         const std::u16string& name,
                         const blink::IndexedDBKeyPath& path,
                         bool automatic) override {
    Queue(base::BindOnce(
        [](std::shared_ptr<Transaction> self, int64_t store,
           std::u16string name, blink::IndexedDBKeyPath path, bool automatic) {
          if (self->mode != Mode::VersionChange || store <= 0 ||
              self->data->stores.contains(store) ||
              self->data->stores.size() >= 64) {
            self->Finish(Exception::kConstraintError);
            return;
          }
          for (const auto& [_, metadata] : self->data->metadata.object_stores)
            if (metadata.name == name) {
              self->Finish(Exception::kConstraintError);
              return;
            }
          self->data->stores.emplace(store, Store());
          self->data->metadata.object_stores.emplace(
              store, blink::IndexedDBObjectStoreMetadata(name, store, path,
                                                         automatic));
          self->data->metadata.max_object_store_id =
              std::max(self->data->metadata.max_object_store_id, store);
          self->Done();
        },
        shared_from_this(), store, name, path, automatic));
  }
  void DeleteObjectStore(int64_t store) override {
    Queue(base::BindOnce(
        [](std::shared_ptr<Transaction> self, int64_t store) {
          if (self->mode != Mode::VersionChange ||
              !self->Allowed(store, true)) {
            self->Finish(Exception::kUnknownError);
            return;
          }
          self->data->stores.erase(store);
          self->data->metadata.object_stores.erase(store);
          self->Done();
        },
        shared_from_this(), store));
  }
  void Put(int64_t store,
           api::IDBValuePtr value,
           Key key,
           api::IDBPutMode mode,
           std::vector<blink::IndexedDBIndexKeys> indexes,
           PutCallback callback) override;
  void SetIndexKeys(int64_t store,
                    Key key,
                    blink::IndexedDBIndexKeys indexes) override;
  void SetIndexKeysDone() override {
    Queue(base::BindOnce(
              [](std::shared_ptr<Transaction> self) {
                if (!self->pending_indexes) {
                  self->Finish(Exception::kUnknownError);
                  return;
                }
                --self->pending_indexes;
                self->Done();
              },
              shared_from_this()),
          true);
  }
  void Commit(int64_t handled) override {
    Queue(base::BindOnce(
        [](std::shared_ptr<Transaction> self, int64_t handled) {
          self->Finish(handled < 0 || uint64_t(handled) < self->errors
                           ? Exception::kAbortError
                           : Exception::kNoError);
        },
        shared_from_this(), handled));
  }
};
class Connection final : public api::IDBDatabase {
 public:
  Connection(Slot* slot,
             uint64_t identifier,
             mojo::PendingAssociatedRemote<api::IDBDatabaseCallbacks> callbacks)
      : callbacks_(std::move(callbacks)),
        slot_(slot),
        identifier_(identifier) {}
  ~Connection() override {
    auto transactions = std::move(transactions_);
    for (auto& [_, transaction] : transactions)
      transaction->Finish(Exception::kAbortError);
  }
  mojo::AssociatedReceiver<api::IDBDatabase> receiver{this};
  mojo::AssociatedRemote<api::IDBDatabaseCallbacks> callbacks_;
  std::map<int64_t, std::shared_ptr<Transaction>> transactions_;
  std::shared_ptr<Transaction> Find(int64_t identifier) {
    auto found = transactions_.find(identifier);
    return found == transactions_.end() ? nullptr : found->second;
  }
  std::shared_ptr<Transaction> Begin(
      int64_t identifier,
      Mode mode,
      const std::vector<int64_t>& scope,
      mojo::PendingAssociatedReceiver<api::IDBTransaction>
          transaction_receiver) {
    if (transactions_.contains(identifier) || transactions_.size() >= 64 ||
        slot_->transactions.size() >= 128)
      return {};
    auto transaction = std::make_shared<Transaction>(
        slot_, identifier_, identifier, mode,
        std::set<int64_t>(scope.begin(), scope.end()));
    transaction->receiver.Bind(std::move(transaction_receiver));
    transactions_.emplace(identifier, transaction);
    slot_->transactions.push_back(transaction);
    return transaction;
  }
  void CreateTransaction(
      mojo::PendingAssociatedReceiver<api::IDBTransaction> transaction_receiver,
      int64_t identifier,
      const std::vector<int64_t>& scope,
      Mode mode,
      api::IDBTransactionDurability) override {
    auto transaction =
        Begin(identifier, mode, scope, std::move(transaction_receiver));
    if (!transaction || mode == Mode::VersionChange || scope.empty()) {
      if (transaction)
        transaction->Finish(Exception::kUnknownError);
      else
        callbacks_->Abort(identifier, Exception::kQuotaError,
                          u"Transaction limit exceeded");
      return;
    }
    slot_->StartTransaction();
  }
  void RenameObjectStore(int64_t identifier,
                         int64_t store,
                         const std::u16string& name) override {
    auto transaction = Find(identifier);
    if (!transaction)
      return;
    transaction->Queue(base::BindOnce(
        [](std::shared_ptr<Transaction> self, int64_t store,
           std::u16string name) {
          if (self->mode != Mode::VersionChange ||
              !self->Allowed(store, true)) {
            self->Finish(Exception::kUnknownError);
            return;
          }
          for (const auto& [key, metadata] : self->data->metadata.object_stores)
            if (key != store && metadata.name == name) {
              self->Finish(Exception::kConstraintError);
              return;
            }
          self->data->metadata.object_stores.at(store).name = name;
          self->Done();
        },
        transaction, store, name));
  }
  void VersionChangeIgnored() override {
    if (slot_->opens.empty())
      return;
    auto request = slot_->opens.front();
    if (request->notified && !request->started && !request->blocked_sent) {
      request->blocked_sent = true;
      request->client->Blocked(slot_->Current()->metadata.version);
    }
  }
  void Get(int64_t identifier,
           int64_t store,
           int64_t index,
           Range range,
           bool key_only,
           GetCallback callback) override {
    auto transaction = Find(identifier);
    if (!transaction) {
      std::move(callback).Run(api::IDBDatabaseGetResult::NewErrorResult(
          Error(Exception::kAbortError)));
      return;
    }
    transaction->Queue(base::BindOnce(
        [](std::shared_ptr<Transaction> self, int64_t store, int64_t index,
           Range range, bool key_only, GetCallback callback) {
          if (!self->Allowed(store)) {
            std::move(callback).Run(api::IDBDatabaseGetResult::NewErrorResult(
                Error(Exception::kUnknownError)));
            self->Done();
            return;
          }
          auto rows = Rows(*self->data, store, index, range,
                           api::IDBCursorDirection::Next);
          if (rows.empty())
            std::move(callback).Run(api::IDBDatabaseGetResult::NewEmpty(true));
          else if (key_only)
            std::move(callback).Run(
                api::IDBDatabaseGetResult::NewKey(rows[0].record->key.Clone()));
          else {
            auto value = Value(*rows[0].record);
            if (!value)
              std::move(callback).Run(api::IDBDatabaseGetResult::NewErrorResult(
                  Error(Exception::kQuotaError)));
            else
              std::move(callback).Run(api::IDBDatabaseGetResult::NewValue(
                  ReturnValue(std::move(value), *rows[0].record,
                              self->data->metadata.object_stores.at(store))));
          }
          self->Done();
        },
        transaction, store, index, std::move(range), key_only,
        std::move(callback)));
  }
  void GetAll(int64_t identifier,
              int64_t store,
              int64_t index,
              Range range,
              api::IDBGetAllResultType type,
              uint32_t maximum,
              api::IDBCursorDirection direction,
              GetAllCallback callback) override;
  void OpenCursor(int64_t identifier,
                  int64_t store,
                  int64_t index,
                  Range range,
                  api::IDBCursorDirection direction,
                  bool key_only,
                  api::IDBTaskType,
                  OpenCursorCallback callback) override;
  void Count(int64_t identifier,
             int64_t store,
             int64_t index,
             Range range,
             CountCallback callback) override {
    auto transaction = Find(identifier);
    if (!transaction) {
      std::move(callback).Run(false, 0);
      return;
    }
    transaction->Queue(base::BindOnce(
        [](std::shared_ptr<Transaction> self, int64_t store, int64_t index,
           Range range, CountCallback callback) {
          bool valid = self->Allowed(store);
          std::move(callback).Run(
              valid, valid ? static_cast<uint32_t>(
                                 Rows(*self->data, store, index, range,
                                      api::IDBCursorDirection::Next)
                                     .size())
                           : 0);
          self->Done();
        },
        transaction, store, index, std::move(range), std::move(callback)));
  }
  void DeleteRange(int64_t identifier,
                   int64_t store,
                   Range range,
                   DeleteRangeCallback callback) override {
    auto transaction = Find(identifier);
    if (!transaction) {
      std::move(callback).Run(false);
      return;
    }
    transaction->Queue(base::BindOnce(
        [](std::shared_ptr<Transaction> self, int64_t store, Range range,
           DeleteRangeCallback callback) {
          bool valid = self->Allowed(store, true);
          if (valid)
            std::erase_if(self->data->stores.at(store).records,
                          [&](const auto& record) {
                            return Contains(range, record->key);
                          });
          std::move(callback).Run(valid);
          self->Done();
        },
        transaction, store, std::move(range), std::move(callback)));
  }
  void GetKeyGeneratorCurrentNumber(
      int64_t identifier,
      int64_t store,
      GetKeyGeneratorCurrentNumberCallback callback) override {
    auto transaction = Find(identifier);
    if (!transaction) {
      std::move(callback).Run(-1, Error(Exception::kAbortError));
      return;
    }
    transaction->Queue(base::BindOnce(
        [](std::shared_ptr<Transaction> self, int64_t store,
           GetKeyGeneratorCurrentNumberCallback callback) {
          bool valid = self->Allowed(store);
          std::move(callback).Run(
              valid ? self->data->stores.at(store).generator : -1,
              valid ? nullptr : Error(Exception::kUnknownError));
          self->Done();
        },
        transaction, store, std::move(callback)));
  }
  void Clear(int64_t identifier,
             int64_t store,
             ClearCallback callback) override {
    auto transaction = Find(identifier);
    if (!transaction) {
      std::move(callback).Run(false);
      return;
    }
    transaction->Queue(base::BindOnce(
        [](std::shared_ptr<Transaction> self, int64_t store,
           ClearCallback callback) {
          bool valid = self->Allowed(store, true);
          if (valid)
            self->data->stores.at(store).records.clear();
          std::move(callback).Run(valid);
          self->Done();
        },
        transaction, store, std::move(callback)));
  }
  void CreateIndex(int64_t identifier,
                   int64_t store,
                   const blink::IndexedDBIndexMetadata& index) override {
    auto transaction = Find(identifier);
    if (!transaction)
      return;
    transaction->Queue(base::BindOnce(
        [](std::shared_ptr<Transaction> self, int64_t store,
           blink::IndexedDBIndexMetadata index) {
          if (!self->Allowed(store, true) ||
              self->mode != Mode::VersionChange) {
            self->Finish(Exception::kUnknownError);
            return;
          }
          auto& metadata = self->data->metadata.object_stores.at(store);
          if (index.id <= 0 || metadata.indexes.contains(index.id) ||
              metadata.indexes.size() >= 64) {
            self->Finish(Exception::kConstraintError);
            return;
          }
          for (const auto& [_, other] : metadata.indexes)
            if (other.name == index.name) {
              self->Finish(Exception::kConstraintError);
              return;
            }
          metadata.max_index_id = std::max(metadata.max_index_id, index.id);
          metadata.indexes.emplace(index.id, std::move(index));
          self->Done();
        },
        transaction, store, index));
  }
  void DeleteIndex(int64_t identifier, int64_t store, int64_t index) override {
    auto transaction = Find(identifier);
    if (!transaction)
      return;
    transaction->Queue(base::BindOnce(
        [](std::shared_ptr<Transaction> self, int64_t store, int64_t index) {
          if (!self->Allowed(store, true) ||
              self->mode != Mode::VersionChange) {
            self->Finish(Exception::kUnknownError);
            return;
          }
          self->data->metadata.object_stores.at(store).indexes.erase(index);
          for (auto& record : self->data->stores.at(store).records) {
            auto copy = std::make_shared<Record>();
            copy->key = record->key.Clone();
            copy->bits = record->bits;
            copy->external = record->external;
            copy->indexes = record->indexes;
            copy->indexes.erase(index);
            record = std::move(copy);
          }
          self->Done();
        },
        transaction, store, index));
  }
  void RenameIndex(int64_t identifier,
                   int64_t store,
                   int64_t index,
                   const std::u16string& name) override {
    auto transaction = Find(identifier);
    if (!transaction)
      return;
    transaction->Queue(base::BindOnce(
        [](std::shared_ptr<Transaction> self, int64_t store, int64_t index,
           std::u16string name) {
          if (!self->Allowed(store, true) ||
              self->mode != Mode::VersionChange) {
            self->Finish(Exception::kUnknownError);
            return;
          }
          auto& indexes = self->data->metadata.object_stores.at(store).indexes;
          if (!indexes.contains(index)) {
            self->Finish(Exception::kUnknownError);
            return;
          }
          for (auto& [key, metadata] : indexes)
            if (key != index && metadata.name == name) {
              self->Finish(Exception::kConstraintError);
              return;
            }
          indexes.at(index).name = name;
          self->Done();
        },
        transaction, store, index, name));
  }
  void Abort(int64_t identifier) override {
    if (auto transaction = Find(identifier))
      transaction->Finish(Exception::kAbortError);
  }
  void DidBecomeInactive() override {}
  void UpdatePriority(int32_t) override {}

 private:
  Slot* slot_;
  uint64_t identifier_;
};
void Transaction::Finish(Exception error) {
  if (finished)
    return;
  auto keep_alive = shared_from_this();
  finished = true;
  operations.clear();
  index_operations.clear();
  if (error == Exception::kNoError && mode != Mode::ReadOnly) {
    auto previous = slot->origin->databases.find(slot->name);
    auto old =
        previous == slot->origin->databases.end() ? nullptr : previous->second;
    slot->origin->databases[slot->name] = data;
    error = slot->origin->Save();
    if (error != Exception::kNoError) {
      if (old)
        slot->origin->databases[slot->name] = old;
      else
        slot->origin->databases.erase(slot->name);
    }
  }
  auto owner = slot->connections.find(connection);
  if (owner != slot->connections.end()) {
    if (error == Exception::kNoError)
      owner->second->callbacks_->Complete(identifier);
    else
      owner->second->callbacks_->Abort(identifier, error,
                                       u"Transaction was rolled back");
    owner->second->transactions_.erase(identifier);
  }
  if (opening) {
    if (error == Exception::kNoError)
      opening->client->OpenSuccess({}, data->metadata);
    else
      opening->client->Error(Exception::kAbortError,
                             u"Database upgrade was rolled back");
    if (!slot->opens.empty() && slot->opens.front() == opening)
      slot->opens.pop_front();
  }
  receiver.reset();
  slot->Finished(this);
}
void Slot::StartTransaction() {
  if (active || transactions.empty())
    return;
  auto transaction = transactions.front();
  active = true;
  transaction->data = std::make_shared<Data>(*Current());
  if (transaction->opening)
    transaction->data->metadata.version = transaction->opening->version;
  transaction->running = true;
  transaction->Pump();
}
void Slot::Finished(Transaction* transaction) {
  bool first =
      !transactions.empty() && transactions.front().get() == transaction;
  std::erase_if(transactions,
                [&](const auto& value) { return value.get() == transaction; });
  if (first)
    active = false;
  StartTransaction();
  ProcessOpen();
}
void Slot::Remove(uint64_t identifier) {
  auto found = connections.find(identifier);
  if (found == connections.end())
    return;
  auto connection = std::move(found->second);
  connections.erase(found);
  closing = true;
  connection.reset();
  closing = false;
  ProcessOpen();
}
void Slot::ProcessOpen() {
  if (closing || opens.empty() || opens.front()->started)
    return;
  auto request = opens.front();
  auto current = Current();
  int64_t version = request->version;
  if (!request->deleting && version == -1)
    version = std::max<int64_t>(1, current->metadata.version);
  if (!request->deleting &&
      (version < current->metadata.version || version <= 0)) {
    request->client->Error(Exception::kVersionError,
                           u"The requested database version is older");
    opens.pop_front();
    ProcessOpen();
    return;
  }
  bool upgrade = !request->deleting && version > current->metadata.version;
  if ((upgrade || request->deleting) && !connections.empty()) {
    if (!request->notified) {
      request->notified = true;
      for (auto& [_, connection] : connections)
        connection->callbacks_->VersionChange(current->metadata.version,
                                              request->deleting ? 0 : version);
    }
    return;
  }
  if (request->deleting) {
    auto previous = origin->databases.extract(name);
    auto error = origin->Save();
    if (error == Exception::kNoError)
      request->client->DeleteSuccess(current->metadata.version);
    else {
      if (!previous.empty())
        origin->databases.insert(std::move(previous));
      request->client->Error(error, u"Database deletion failed");
    }
    opens.pop_front();
    ProcessOpen();
    return;
  }
  if (connections.size() >= 32) {
    request->client->Error(Exception::kQuotaError,
                           u"Connection limit exceeded");
    opens.pop_front();
    ProcessOpen();
    return;
  }
  uint64_t identifier = next_connection++;
  auto connection = std::make_unique<Connection>(this, identifier,
                                                 std::move(request->callbacks));
  mojo::PendingAssociatedRemote<api::IDBDatabase> remote;
  connection->receiver.Bind(remote.InitWithNewEndpointAndPassReceiver());
  connection->receiver.set_disconnect_handler(
      base::BindOnce(&Slot::Remove, base::Unretained(this), identifier));
  auto* pointer = connection.get();
  connections.emplace(identifier, std::move(connection));
  if (upgrade) {
    request->started = true;
    request->version = version;
    auto transaction =
        pointer->Begin(request->transaction_id, Mode::VersionChange, {},
                       std::move(request->transaction));
    if (!transaction) {
      request->client->Error(Exception::kQuotaError,
                             u"Transaction limit exceeded");
      opens.pop_front();
      Remove(identifier);
      return;
    }
    transaction->opening = request;
    StartTransaction();
    request->client->UpgradeNeeded(std::move(remote), current->metadata.version,
                                   api::IDBDataLoss::None, "",
                                   transaction->data->metadata);
  } else {
    request->client->OpenSuccess(std::move(remote), current->metadata);
    opens.pop_front();
    ProcessOpen();
  }
}
bool ValidIndexes(Data& data, int64_t store, const Record& candidate) {
  const auto& metadata = data.metadata.object_stores.at(store);
  for (const auto& [identifier, keys] : candidate.indexes) {
    auto index = metadata.indexes.find(identifier);
    if (index == metadata.indexes.end())
      return false;
    for (const auto& key : *keys) {
      if (!key.IsValid())
        return false;
      if (!index->second.unique)
        continue;
      for (const auto& record : data.stores.at(store).records) {
        if (record->key.Equals(candidate.key))
          continue;
        auto existing = record->indexes.find(identifier);
        if (existing != record->indexes.end())
          for (const auto& other : *existing->second)
            if (key.Equals(other))
              return false;
      }
    }
  }
  return true;
}
// Blob reads keep the transaction busy so later operations cannot overtake a
// put.
class PendingPut : public std::enable_shared_from_this<PendingPut> {
 public:
  std::shared_ptr<Transaction> transaction;
  int64_t store;
  api::IDBValuePtr input;
  std::shared_ptr<Record> record;
  api::IDBPutMode mode;
  api::IDBTransaction::PutCallback callback;
  size_t next = 0, bytes = 0;
  uint64_t generator;
  void Finish(Exception error) {
    if (transaction->finished)
      return;
    if (error != Exception::kNoError) {
      ++transaction->errors;
      std::move(callback).Run(
          api::IDBTransactionPutResult::NewErrorResult(Error(error)));
    } else {
      auto& target = transaction->data->stores.at(store);
      auto found = std::find_if(
          target.records.begin(), target.records.end(),
          [&](const auto& other) { return other->key.Equals(record->key); });
      if (found == target.records.end())
        target.records.push_back(record);
      else
        *found = record;
      target.generator = generator;
      std::move(callback).Run(
          api::IDBTransactionPutResult::NewKey(record->key.Clone()));
    }
    transaction->Done();
  }
  void ReadNext() {
    if (transaction->finished)
      return;
    if (next == input->external_objects.size()) {
      Finish(Exception::kNoError);
      return;
    }
    auto& object = input->external_objects[next++];
    if (!object->is_blob_or_file()) {
      Finish(Exception::kDataError);
      return;
    }
    auto& blob = object->get_blob_or_file();
    if (blob->size < 0 || uint64_t(blob->size) > kMaximumSnapshot - bytes ||
        blob->mime_type.size() > 4096) {
      Finish(Exception::kQuotaError);
      return;
    }
    auto external = std::make_shared<External>();
    external->type = blob->mime_type;
    external->file = std::move(blob->file);
    uint64_t expected = blob->size;
    ReadBlob(std::move(blob->blob), 0, expected,
             base::BindOnce(
                 [](std::shared_ptr<PendingPut> self,
                    std::shared_ptr<External> external, uint64_t expected,
                    std::optional<BlobBytes> contents) {
                   if (!contents || contents->size() != expected) {
                     self->Finish(Exception::kDataError);
                     return;
                   }
                   self->bytes += contents->size();
                   external->bytes = std::move(*contents);
                   self->record->external.push_back(std::move(external));
                   self->ReadNext();
                 },
                 shared_from_this(), external, expected));
  }
};
void Transaction::Put(int64_t store,
                      api::IDBValuePtr value,
                      Key key,
                      api::IDBPutMode put_mode,
                      std::vector<blink::IndexedDBIndexKeys> indexes,
                      PutCallback callback) {
  Queue(base::BindOnce(
      [](std::shared_ptr<Transaction> self, int64_t store,
         api::IDBValuePtr value, Key key, api::IDBPutMode put_mode,
         std::vector<blink::IndexedDBIndexKeys> indexes, PutCallback callback) {
        auto pending = std::make_shared<PendingPut>();
        pending->transaction = self;
        pending->store = store;
        pending->input = std::move(value);
        pending->callback = std::move(callback);
        pending->mode = put_mode;
        if (!self->Allowed(store, true)) {
          pending->Finish(Exception::kUnknownError);
          return;
        }
        auto& target = self->data->stores.at(store);
        auto& metadata = self->data->metadata.object_stores.at(store);
        pending->generator = target.generator;
        if (!key.IsValid() && metadata.auto_increment &&
            put_mode != api::IDBPutMode::CursorUpdate) {
          if (target.generator > 9007199254740992ULL) {
            pending->Finish(Exception::kConstraintError);
            return;
          }
          key = Key(double(target.generator), api::IDBKeyType::Number);
          pending->generator++;
        }
        if (!key.IsValid()) {
          pending->Finish(Exception::kDataError);
          return;
        }
        if (metadata.auto_increment &&
            put_mode != api::IDBPutMode::CursorUpdate &&
            key.type() == api::IDBKeyType::Number &&
            key.number() >= double(pending->generator)) {
          pending->generator = key.number() >= 9007199254740992.0
                                   ? 9007199254740993ULL
                                   : uint64_t(std::floor(key.number())) + 1;
        }
        auto found = std::find_if(
            target.records.begin(), target.records.end(),
            [&](const auto& other) { return other->key.Equals(key); });
        if (found != target.records.end() &&
            put_mode == api::IDBPutMode::AddOnly) {
          pending->Finish(Exception::kConstraintError);
          return;
        }
        if ((found == target.records.end() &&
             target.records.size() >= kMaximumRecords) ||
            pending->input->bits.size() > kMaximumSnapshot ||
            pending->input->external_objects.size() > 256) {
          pending->Finish(Exception::kQuotaError);
          return;
        }
        pending->record = std::make_shared<Record>();
        pending->record->key = std::move(key);
        pending->record->bits.assign(
            pending->input->bits.data(),
            pending->input->bits.data() + pending->input->bits.size());
        pending->bytes = pending->record->bits.size();
        for (auto& index : indexes) {
          if (index.keys.size() > kMaximumRecords) {
            pending->Finish(Exception::kQuotaError);
            return;
          }
          auto keys = std::make_shared<std::vector<Key>>();
          for (auto& index_key : index.keys) {
            auto filled = index_key.HasHoles()
                              ? index_key.FillHoles(pending->record->key)
                              : std::move(index_key);
            if (std::none_of(keys->begin(), keys->end(), [&](const Key& other) {
                  return other.Equals(filled);
                }))
              keys->push_back(std::move(filled));
          }
          pending->record->indexes[index.id] = std::move(keys);
        }
        if (!ValidIndexes(*self->data, store, *pending->record)) {
          pending->Finish(Exception::kConstraintError);
          return;
        }
        pending->ReadNext();
      },
      shared_from_this(), store, std::move(value), std::move(key), put_mode,
      std::move(indexes), std::move(callback)));
}
void Transaction::SetIndexKeys(int64_t store,
                               Key key,
                               blink::IndexedDBIndexKeys indexes) {
  Queue(
      base::BindOnce(
          [](std::shared_ptr<Transaction> self, int64_t store, Key key,
             blink::IndexedDBIndexKeys indexes) {
            if (self->mode != Mode::VersionChange ||
                !self->Allowed(store, true)) {
              self->Finish(Exception::kUnknownError);
              return;
            }
            if (indexes.keys.size() > kMaximumRecords) {
              self->Finish(Exception::kQuotaError);
              return;
            }
            for (auto& record : self->data->stores.at(store).records)
              if (record->key.Equals(key)) {
                auto copy = std::make_shared<Record>();
                copy->key = record->key.Clone();
                copy->bits = record->bits;
                copy->external = record->external;
                copy->indexes = record->indexes;
                copy->indexes[indexes.id] =
                    std::make_shared<std::vector<Key>>(std::move(indexes.keys));
                if (!ValidIndexes(*self->data, store, *copy)) {
                  self->Finish(Exception::kConstraintError);
                  return;
                }
                record = std::move(copy);
                break;
              }
            self->Done();
          },
          shared_from_this(), store, std::move(key), std::move(indexes)),
      true);
}
class Cursor final : public api::IDBCursor {
 public:
  Cursor(std::shared_ptr<Transaction> transaction,
         int64_t store,
         int64_t index,
         Range range,
         api::IDBCursorDirection direction,
         bool key_only,
         bool preemptive,
         const Row& first)
      : transaction_(transaction),
        store_(store),
        index_(index),
        range_(std::move(range)),
        direction_(direction),
        key_only_(key_only),
        preemptive_(preemptive),
        key_(first.key.Clone()),
        primary_(first.record->key.Clone()) {}
  void Advance(uint32_t count, AdvanceCallback callback) override {
    Move(count, Key(), Key(), false, std::move(callback));
  }
  void Continue(Key key, Key primary, ContinueCallback callback) override {
    Move(1, std::move(key), std::move(primary), false, std::move(callback));
  }
  void Prefetch(int32_t count, PrefetchCallback callback) override {
    Move(count > 0 ? std::min<int32_t>(count, 1000) : 0, Key(), Key(), true,
         std::move(callback));
  }
  void PrefetchReset(int32_t used) override {
    // Reset arrives before subsequent cursor work on the associated message
    // pipe.
    if (used < 0 || size_t(used) >= prefetched_.size())
      return;
    key_ = prefetched_[used].first.Clone();
    primary_ = prefetched_[used].second.Clone();
    prefetched_.clear();
  }

 private:
  void Move(uint32_t count,
            Key target,
            Key primary,
            bool prefetch,
            AdvanceCallback callback) {
    auto transaction = transaction_.lock();
    if (!transaction || transaction->finished) {
      std::move(callback).Run(
          api::IDBCursorResult::NewErrorResult(Error(Exception::kAbortError)));
      return;
    }
    transaction->Queue(
        base::BindOnce(
            [](base::WeakPtr<Cursor> cursor, std::shared_ptr<Transaction> self,
               uint32_t count, Key target, Key primary, bool prefetch,
               AdvanceCallback callback) {
              if (!cursor || !count || !self->Allowed(cursor->store_)) {
                std::move(callback).Run(api::IDBCursorResult::NewErrorResult(
                    Error(Exception::kUnknownError)));
                self->Done();
                return;
              }
              bool reverse =
                  cursor->direction_ == api::IDBCursorDirection::Prev ||
                  cursor->direction_ ==
                      api::IDBCursorDirection::PrevNoDuplicate;
              bool unique = cursor->direction_ ==
                                api::IDBCursorDirection::NextNoDuplicate ||
                            cursor->direction_ ==
                                api::IDBCursorDirection::PrevNoDuplicate;
              auto before = [reverse](const Key& left, const Key& right) {
                return reverse ? right.IsLessThan(left)
                               : left.IsLessThan(right);
              };
              auto rows = Rows(*self->data, cursor->store_, cursor->index_,
                               cursor->range_, cursor->direction_);
              auto output = api::IDBCursorValue::New();
              cursor->prefetched_.clear();
              if (prefetch)
                cursor->prefetched_.emplace_back(cursor->key_.Clone(),
                                                 cursor->primary_.Clone());
              for (auto& row : rows) {
                if (!before(cursor->key_, row.key) &&
                    (!cursor->key_.Equals(row.key) || unique ||
                     !before(cursor->primary_, row.record->key)))
                  continue;
                if (target.IsValid() &&
                    (before(row.key, target) ||
                     (target.Equals(row.key) && primary.IsValid() &&
                      before(row.record->key, primary))))
                  continue;
                if (!prefetch && --count)
                  continue;
                auto value = cursor->key_only_ ? api::IDBValue::New()
                                               : Value(*row.record);
                if (!value) {
                  std::move(callback).Run(api::IDBCursorResult::NewErrorResult(
                      Error(Exception::kQuotaError)));
                  self->Done();
                  return;
                }
                output->keys.push_back(row.key.Clone());
                output->primary_keys.push_back(row.record->key.Clone());
                output->values.push_back(std::move(value));
                cursor->key_ = row.key.Clone();
                cursor->primary_ = row.record->key.Clone();
                if (!prefetch)
                  break;
                cursor->prefetched_.emplace_back(row.key.Clone(),
                                                 row.record->key.Clone());
                if (!--count)
                  break;
              }
              std::move(callback).Run(
                  output->keys.empty()
                      ? api::IDBCursorResult::NewEmpty(true)
                      : api::IDBCursorResult::NewValues(std::move(output)));
              self->Done();
            },
            weak_.GetWeakPtr(), transaction, count, std::move(target),
            std::move(primary), prefetch, std::move(callback)),
        preemptive_);
  }
  std::weak_ptr<Transaction> transaction_;
  int64_t store_, index_;
  Range range_;
  api::IDBCursorDirection direction_;
  bool key_only_, preemptive_;
  Key key_, primary_;
  std::vector<std::pair<Key, Key>> prefetched_;
  base::WeakPtrFactory<Cursor> weak_{this};
};
void Connection::OpenCursor(int64_t identifier,
                            int64_t store,
                            int64_t index,
                            Range range,
                            api::IDBCursorDirection direction,
                            bool key_only,
                            api::IDBTaskType task_type,
                            OpenCursorCallback callback) {
  auto transaction = Find(identifier);
  if (!transaction) {
    std::move(callback).Run(api::IDBDatabaseOpenCursorResult::NewErrorResult(
        Error(Exception::kAbortError)));
    return;
  }
  transaction->Queue(base::BindOnce(
      [](std::shared_ptr<Transaction> self, int64_t store, int64_t index,
         Range range, api::IDBCursorDirection direction, bool key_only,
         api::IDBTaskType task_type, OpenCursorCallback callback) {
        if (!self->Allowed(store)) {
          std::move(callback).Run(
              api::IDBDatabaseOpenCursorResult::NewErrorResult(
                  Error(Exception::kUnknownError)));
          self->Done();
          return;
        }
        bool preemptive = task_type == api::IDBTaskType::Preemptive;
        if (preemptive)
          ++self->pending_indexes;
        auto rows = Rows(*self->data, store, index, range, direction);
        if (rows.empty())
          std::move(callback).Run(
              api::IDBDatabaseOpenCursorResult::NewEmpty(true));
        else {
          auto value = key_only ? nullptr : Value(*rows[0].record);
          if (!key_only && !value) {
            std::move(callback).Run(
                api::IDBDatabaseOpenCursorResult::NewErrorResult(
                    Error(Exception::kQuotaError)));
            self->Done();
            return;
          }
          mojo::PendingAssociatedRemote<api::IDBCursor> remote;
          mojo::MakeSelfOwnedAssociatedReceiver(
              std::make_unique<Cursor>(self, store, index, std::move(range),
                                       direction, key_only, preemptive,
                                       rows[0]),
              remote.InitWithNewEndpointAndPassReceiver());
          std::move(callback).Run(api::IDBDatabaseOpenCursorResult::NewValue(
              api::IDBDatabaseOpenCursorValue::New(
                  std::move(remote), rows[0].key.Clone(),
                  rows[0].record->key.Clone(), std::move(value))));
        }
        self->Done();
      },
      transaction, store, index, std::move(range), direction, key_only,
      task_type, std::move(callback)));
}
void Connection::GetAll(int64_t identifier,
                        int64_t store,
                        int64_t index,
                        Range range,
                        api::IDBGetAllResultType type,
                        uint32_t maximum,
                        api::IDBCursorDirection direction,
                        GetAllCallback callback) {
  auto transaction = Find(identifier);
  if (!transaction) {
    mojo::AssociatedRemote<api::IDBDatabaseGetAllResultSink> sink;
    std::move(callback).Run({}, sink.BindNewEndpointAndPassReceiver());
    sink->OnError(Error(Exception::kAbortError));
    return;
  }
  transaction->Queue(base::BindOnce(
      [](std::shared_ptr<Transaction> self, int64_t store, int64_t index,
         Range range, api::IDBGetAllResultType type, uint32_t maximum,
         api::IDBCursorDirection direction, GetAllCallback callback) {
        mojo::AssociatedRemote<api::IDBDatabaseGetAllResultSink> sink;
        if (!self->Allowed(store)) {
          std::move(callback).Run({}, sink.BindNewEndpointAndPassReceiver());
          sink->OnError(Error(Exception::kUnknownError));
          self->Done();
          return;
        }
        auto rows = Rows(*self->data, store, index, range, direction);
        if (maximum && rows.size() > maximum)
          rows.resize(maximum);
        std::vector<api::IDBRecordPtr> records;
        bool streamed = false;
        for (size_t position = 0; position < rows.size(); ++position) {
          auto& row = rows[position];
          auto record = api::IDBRecord::New();
          if (type != api::IDBGetAllResultType::Values)
            record->primary_key = row.record->key.Clone();
          if (type != api::IDBGetAllResultType::Keys) {
            auto value = Value(*row.record);
            if (!value) {
              if (!streamed)
                std::move(callback).Run({},
                                        sink.BindNewEndpointAndPassReceiver());
              sink->OnError(Error(Exception::kQuotaError));
              self->Done();
              return;
            }
            record->return_value =
                ReturnValue(std::move(value), *row.record,
                            self->data->metadata.object_stores.at(store));
          }
          if (index != -1 && type == api::IDBGetAllResultType::Records)
            record->index_key = row.key.Clone();
          records.push_back(std::move(record));
          if (records.size() == api::kIDBGetAllChunkSize &&
              position + 1 < rows.size()) {
            if (!streamed) {
              std::move(callback).Run(std::move(records),
                                      sink.BindNewEndpointAndPassReceiver());
              streamed = true;
            } else
              sink->ReceiveResults(std::move(records), false);
            records.clear();
          }
        }
        if (streamed)
          sink->ReceiveResults(std::move(records), true);
        else
          std::move(callback).Run(std::move(records), {});
        self->Done();
      },
      transaction, store, index, std::move(range), type, maximum, direction,
      std::move(callback)));
}
std::string Stem(const std::string& identity) {
  uint8_t hash[32];
  SHA256(reinterpret_cast<const uint8_t*>(identity.data()), identity.size(),
         hash);
  std::string result = "/profile/idb-";
  for (uint8_t byte : hash) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}
bool Transfer(int descriptor, BlobBytes& bytes, bool writing) {
  size_t position = 0;
  while (position < bytes.size()) {
    ssize_t count = writing ? write(descriptor, bytes.data() + position,
                                    bytes.size() - position)
                            : read(descriptor, bytes.data() + position,
                                   bytes.size() - position);
    if (count <= 0)
      return false;
    position += count;
  }
  return true;
}
Origin::Origin(std::string identity) : identity(std::move(identity)) {
  Load();
}
Origin::~Origin() = default;
Slot* Origin::Get(const std::u16string& name) {
  auto found = slots.find(name);
  if (found != slots.end())
    return found->second.get();
  if (slots.size() >= 32 || name.size() > 4096)
    return nullptr;
  return slots.emplace(name, std::make_unique<Slot>(this, name))
      .first->second.get();
}
Exception Origin::Save() {
  Buffer buffer;
  buffer.bytes.assign(kFormat, kFormat + sizeof(kFormat));
  buffer.Number(generation + 1);
  buffer.Part(base::as_byte_span(identity));
  buffer.Number(databases.size());
  for (auto& [name, data] : databases) {
    buffer.Object<api::IDBDatabaseMetadata>(data->metadata);
    buffer.Number(data->stores.size());
    for (auto& [identifier, store] : data->stores) {
      buffer.Number(identifier);
      buffer.Number(store.generator);
      buffer.Number(store.records.size());
      for (auto& record : store.records) {
        buffer.Object<api::IDBKey>(record->key);
        buffer.Part(record->bits);
        buffer.Number(record->external.size());
        for (auto& external : record->external) {
          buffer.Part(base::as_byte_span(external->type));
          buffer.Number(bool(external->file));
          if (external->file)
            buffer.Object<api::IDBFileInfo>(external->file);
          buffer.Part(external->bytes);
        }
        buffer.Number(record->indexes.size());
        for (auto& [index, keys] : record->indexes) {
          buffer.Number(index);
          buffer.Number(keys->size());
          for (auto& key : *keys)
            buffer.Object<api::IDBKey>(key);
        }
        if (!buffer.valid)
          return Exception::kQuotaError;
      }
    }
  }
  if (!buffer.valid || buffer.bytes.size() > kMaximumSnapshot - 32)
    return Exception::kQuotaError;
  uint8_t hash[32];
  SHA256(buffer.bytes.data(), buffer.bytes.size(), hash);
  buffer.bytes.insert(buffer.bytes.end(), hash, hash + 32);
  std::string path =
      Stem(identity) + "-" + std::to_string((generation + 1) % 2);
  int descriptor =
      open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
  if (descriptor < 0)
    return Exception::kUnknownError;
  bool success =
      Transfer(descriptor, buffer.bytes, true) && fsync(descriptor) == 0;
  if (close(descriptor) != 0)
    success = false;
  if (!success)
    return Exception::kUnknownError;
  ++generation;
  return Exception::kNoError;
}
void Origin::Load() {
  for (unsigned slot = 0; slot < 2; ++slot) {
    std::string path = Stem(identity) + "-" + std::to_string(slot);
    int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
      continue;
    off_t size = lseek(descriptor, 0, SEEK_END);
    if (size < 64 || size > off_t(kMaximumSnapshot) ||
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
    uint64_t count = reader.Number();
    if (count > 32)
      continue;
    std::map<std::u16string, std::shared_ptr<Data>> loaded;
    for (uint64_t database_index = 0; database_index < count && reader.valid;
         ++database_index) {
      auto data = std::make_shared<Data>();
      reader.Object<api::IDBDatabaseMetadata>(data->metadata);
      uint64_t stores = reader.Number();
      if (!reader.valid || stores > 64 ||
          stores != data->metadata.object_stores.size() ||
          data->metadata.version < 1 || data->metadata.name.size() > 4096 ||
          loaded.contains(data->metadata.name)) {
        reader.valid = false;
        break;
      }
      for (uint64_t store_index = 0; store_index < stores && reader.valid;
           ++store_index) {
        int64_t identifier = reader.Number();
        uint64_t generator = reader.Number(), records = reader.Number();
        if (records > kMaximumRecords || !generator ||
            generator > 9007199254740993ULL ||
            !data->metadata.object_stores.contains(identifier) ||
            data->stores.contains(identifier)) {
          reader.valid = false;
          break;
        }
        auto& store = data->stores[identifier];
        store.generator = generator;
        for (uint64_t position = 0; position < records && reader.valid;
             ++position) {
          auto record = std::make_shared<Record>();
          reader.Object<api::IDBKey>(record->key);
          auto bits = reader.Part();
          record->bits.assign(bits.begin(), bits.end());
          uint64_t external_count = reader.Number();
          if (!record->key.IsValid() || external_count > 256) {
            reader.valid = false;
            break;
          }
          for (auto& previous : store.records)
            if (previous->key.Equals(record->key))
              reader.valid = false;
          for (uint64_t item = 0; item < external_count && reader.valid;
               ++item) {
            auto external = std::make_shared<External>();
            auto type = reader.Part();
            if (type.size() % 2 || type.size() > 8192) {
              reader.valid = false;
              break;
            }
            external->type.resize(type.size() / 2);
            memcpy(external->type.data(), type.data(), type.size());
            uint64_t has_file = reader.Number();
            if (has_file > 1) {
              reader.valid = false;
              break;
            }
            if (has_file) {
              reader.Object<api::IDBFileInfo>(external->file);
              if (!external->file)
                reader.valid = false;
            }
            auto contents = reader.Part();
            external->bytes.assign(contents.begin(), contents.end());
            record->external.push_back(std::move(external));
          }
          uint64_t indexes = reader.Number();
          if (indexes > 64) {
            reader.valid = false;
            break;
          }
          for (uint64_t item = 0; item < indexes && reader.valid; ++item) {
            int64_t index = reader.Number();
            uint64_t keys_count = reader.Number();
            if (keys_count > kMaximumRecords ||
                record->indexes.contains(index)) {
              reader.valid = false;
              break;
            }
            auto keys = std::make_shared<std::vector<Key>>();
            for (uint64_t key_index = 0; key_index < keys_count && reader.valid;
                 ++key_index) {
              Key key;
              reader.Object<api::IDBKey>(key);
              if (!key.IsValid())
                reader.valid = false;
              keys->push_back(std::move(key));
            }
            record->indexes.emplace(index, std::move(keys));
          }
          if (!reader.valid || !ValidIndexes(*data, identifier, *record)) {
            reader.valid = false;
            break;
          }
          store.records.push_back(std::move(record));
        }
      }
      loaded.emplace(data->metadata.name, std::move(data));
    }
    if (reader.valid && reader.bytes.empty()) {
      generation = next_generation;
      databases = std::move(loaded);
    }
  }
  std::printf("IDB: loaded generation=%llu databases=%zu\n",
              static_cast<unsigned long long>(generation), databases.size());
}
class Factory final : public api::IDBFactory {
 public:
  explicit Factory(Origin* origin) : origin_(origin) {}
  void GetDatabaseInfo(GetDatabaseInfoCallback callback) override {
    std::vector<api::IDBNameAndVersionPtr> result;
    for (const auto& [name, data] : origin_->databases)
      result.push_back(
          api::IDBNameAndVersion::New(name, data->metadata.version));
    std::move(callback).Run(std::move(result), Error(Exception::kNoError, u""));
  }
  void Open(mojo::PendingAssociatedRemote<api::IDBFactoryClient> client,
            mojo::PendingAssociatedRemote<api::IDBDatabaseCallbacks> callbacks,
            const std::u16string& name,
            int64_t version,
            mojo::PendingAssociatedReceiver<api::IDBTransaction> receiver,
            int64_t identifier,
            int32_t,
            bool) override {
    auto request = std::make_shared<OpenRequest>();
    request->client.Bind(std::move(client));
    request->callbacks = std::move(callbacks);
    request->transaction = std::move(receiver);
    request->version = version;
    request->transaction_id = identifier;
    Enqueue(name, request);
  }
  void DeleteDatabase(
      mojo::PendingAssociatedRemote<api::IDBFactoryClient> client,
      const std::u16string& name,
      bool force_close) override {
    auto request = std::make_shared<OpenRequest>();
    request->client.Bind(std::move(client));
    request->deleting = true;
    if (force_close) {
      auto* slot = origin_->Get(name);
      if (slot)
        while (!slot->connections.empty()) {
          auto& connection = slot->connections.begin()->second;
          connection->callbacks_->ForcedClose();
          slot->Remove(slot->connections.begin()->first);
        }
    }
    Enqueue(name, request);
  }

 private:
  void Enqueue(const std::u16string& name,
               std::shared_ptr<OpenRequest> request) {
    auto* slot = origin_->Get(name);
    if (!slot || slot->opens.size() >= 64) {
      request->client->Error(Exception::kQuotaError,
                             u"Database request limit exceeded");
      return;
    }
    request->client.set_disconnect_handler(base::BindOnce(
        [](Slot* slot, std::weak_ptr<OpenRequest> weak) {
          auto request = weak.lock();
          if (!request || request->started)
            return;
          std::erase(slot->opens, request);
          slot->ProcessOpen();
        },
        base::Unretained(slot), std::weak_ptr<OpenRequest>(request)));
    slot->opens.push_back(std::move(request));
    slot->ProcessOpen();
  }
  Origin* origin_;
};
}  // namespace
void BindIndexedDB(blink::LocalFrameToken token,
                   mojo::PendingReceiver<api::IDBFactory> receiver) {
  auto* frame = blink::LocalFrame::FromFrameToken(token);
  if (!frame || !frame->DomWindow())
    return;
  auto key = frame->DomWindow()->GetStorageKey();
  if (key.GetSecurityOrigin()->IsOpaque())
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
  mojo::MakeSelfOwnedReceiver(std::make_unique<Factory>(found->second.get()),
                              std::move(receiver));
}
}  // namespace uefi_browser
