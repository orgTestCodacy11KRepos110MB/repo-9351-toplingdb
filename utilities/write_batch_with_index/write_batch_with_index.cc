//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include "rocksdb/utilities/write_batch_with_index.h"

#include <memory>
#include <vector>

#include "db/column_family.h"
#include "db/db_impl.h"
#include "db/merge_context.h"
#include "db/merge_helper.h"
#include "options/db_options.h"
#include "rocksdb/comparator.h"
#include "rocksdb/iterator.h"
#include "util/arena.h"
#include "util/cast_util.h"
#include "utilities/write_batch_with_index/write_batch_with_index_internal.h"

namespace rocksdb {

// when direction == forward
// * current_at_base_ <=> base_iterator > delta_iterator
// when direction == backwards
// * current_at_base_ <=> base_iterator < delta_iterator
// always:
// * equal_keys_ <=> base_iterator == delta_iterator
class BaseDeltaIterator : public Iterator {
 public:
  BaseDeltaIterator(Iterator* base_iterator, WBWIIterator* delta_iterator,
                    const Comparator* comparator)
      : forward_(true),
        current_at_base_(true),
        equal_keys_(false),
        status_(Status::OK()),
        base_iterator_(base_iterator),
        delta_iterator_(delta_iterator),
        comparator_(comparator) {}

  virtual ~BaseDeltaIterator() {}

  bool Valid() const override {
    return current_at_base_ ? BaseValid() : DeltaValid();
  }

  void SeekToFirst() override {
    forward_ = true;
    base_iterator_->SeekToFirst();
    delta_iterator_->SeekToFirst();
    UpdateCurrent();
  }

  void SeekToLast() override {
    forward_ = false;
    base_iterator_->SeekToLast();
    delta_iterator_->SeekToLast();
    UpdateCurrent();
  }

  void Seek(const Slice& k) override {
    forward_ = true;
    base_iterator_->Seek(k);
    delta_iterator_->Seek(k);
    UpdateCurrent();
  }

  void SeekForPrev(const Slice& k) override {
    forward_ = false;
    base_iterator_->SeekForPrev(k);
    delta_iterator_->SeekForPrev(k);
    UpdateCurrent();
  }

  void Next() override {
    if (!Valid()) {
      status_ = Status::NotSupported("Next() on invalid iterator");
    }

    if (!forward_) {
      // Need to change direction
      // if our direction was backward and we're not equal, we have two states:
      // * both iterators are valid: we're already in a good state (current
      // shows to smaller)
      // * only one iterator is valid: we need to advance that iterator
      forward_ = true;
      equal_keys_ = false;
      if (!BaseValid()) {
        assert(DeltaValid());
        base_iterator_->SeekToFirst();
      } else if (!DeltaValid()) {
        delta_iterator_->SeekToFirst();
      } else if (current_at_base_) {
        // Change delta from larger than base to smaller
        AdvanceDelta();
      } else {
        // Change base from larger than delta to smaller
        AdvanceBase();
      }
      if (DeltaValid() && BaseValid()) {
        if (comparator_->Equal(delta_iterator_->Entry().key,
                               base_iterator_->key())) {
          equal_keys_ = true;
        }
      }
    }
    Advance();
  }

  void Prev() override {
    if (!Valid()) {
      status_ = Status::NotSupported("Prev() on invalid iterator");
    }

    if (forward_) {
      // Need to change direction
      // if our direction was backward and we're not equal, we have two states:
      // * both iterators are valid: we're already in a good state (current
      // shows to smaller)
      // * only one iterator is valid: we need to advance that iterator
      forward_ = false;
      equal_keys_ = false;
      if (!BaseValid()) {
        assert(DeltaValid());
        base_iterator_->SeekToLast();
      } else if (!DeltaValid()) {
        delta_iterator_->SeekToLast();
      } else if (current_at_base_) {
        // Change delta from less advanced than base to more advanced
        AdvanceDelta();
      } else {
        // Change base from less advanced than delta to more advanced
        AdvanceBase();
      }
      if (DeltaValid() && BaseValid()) {
        if (comparator_->Equal(delta_iterator_->Entry().key,
                               base_iterator_->key())) {
          equal_keys_ = true;
        }
      }
    }

    Advance();
  }

  Slice key() const override {
    return current_at_base_ ? base_iterator_->key()
                            : delta_iterator_->Entry().key;
  }

  Slice value() const override {
    return current_at_base_ ? base_iterator_->value()
                            : delta_iterator_->Entry().value;
  }

  Status status() const override {
    if (!status_.ok()) {
      return status_;
    }
    if (!base_iterator_->status().ok()) {
      return base_iterator_->status();
    }
    return delta_iterator_->status();
  }

 private:
  void AssertInvariants() {
#ifndef NDEBUG
    if (!Valid()) {
      return;
    }
    if (!BaseValid()) {
      assert(!current_at_base_ && delta_iterator_->Valid());
      return;
    }
    if (!DeltaValid()) {
      assert(current_at_base_ && base_iterator_->Valid());
      return;
    }
    // we don't support those yet
    assert(delta_iterator_->Entry().type != kMergeRecord &&
           delta_iterator_->Entry().type != kLogDataRecord);
    int compare = comparator_->Compare(delta_iterator_->Entry().key,
                                       base_iterator_->key());
    if (forward_) {
      // current_at_base -> compare < 0
      assert(!current_at_base_ || compare < 0);
      // !current_at_base -> compare <= 0
      assert(current_at_base_ && compare >= 0);
    } else {
      // current_at_base -> compare > 0
      assert(!current_at_base_ || compare > 0);
      // !current_at_base -> compare <= 0
      assert(current_at_base_ && compare <= 0);
    }
    // equal_keys_ <=> compare == 0
    assert((equal_keys_ || compare != 0) && (!equal_keys_ || compare == 0));
#endif
  }

  void Advance() {
    if (equal_keys_) {
      assert(BaseValid() && DeltaValid());
      AdvanceBase();
      AdvanceDelta();
    } else {
      if (current_at_base_) {
        assert(BaseValid());
        AdvanceBase();
      } else {
        assert(DeltaValid());
        AdvanceDelta();
      }
    }
    UpdateCurrent();
  }

  void AdvanceDelta() {
    if (forward_) {
      delta_iterator_->Next();
    } else {
      delta_iterator_->Prev();
    }
  }
  void AdvanceBase() {
    if (forward_) {
      base_iterator_->Next();
    } else {
      base_iterator_->Prev();
    }
  }
  bool BaseValid() const { return base_iterator_->Valid(); }
  bool DeltaValid() const { return delta_iterator_->Valid(); }
  void UpdateCurrent() {
// Suppress false positive clang analyzer warnings.
#ifndef __clang_analyzer__
    while (true) {
      WriteEntry delta_entry;
      if (DeltaValid()) {
        delta_entry = delta_iterator_->Entry();
      }
      equal_keys_ = false;
      if (!BaseValid()) {
        // Base has finished.
        if (!DeltaValid()) {
          // Finished
          return;
        }
        if (delta_entry.type == kDeleteRecord ||
            delta_entry.type == kSingleDeleteRecord) {
          AdvanceDelta();
        } else {
          current_at_base_ = false;
          return;
        }
      } else if (!DeltaValid()) {
        // Delta has finished.
        current_at_base_ = true;
        return;
      } else {
        int compare =
            (forward_ ? 1 : -1) *
            comparator_->Compare(delta_entry.key, base_iterator_->key());
        if (compare <= 0) {  // delta bigger or equal
          if (compare == 0) {
            equal_keys_ = true;
          }
          if (delta_entry.type != kDeleteRecord &&
              delta_entry.type != kSingleDeleteRecord) {
            current_at_base_ = false;
            return;
          }
          // Delta is less advanced and is delete.
          AdvanceDelta();
          if (equal_keys_) {
            AdvanceBase();
          }
        } else {
          current_at_base_ = true;
          return;
        }
      }
    }

    AssertInvariants();
#endif  // __clang_analyzer__
  }

  bool forward_;
  bool current_at_base_;
  bool equal_keys_;
  Status status_;
  std::unique_ptr<Iterator> base_iterator_;
  std::unique_ptr<WBWIIterator> delta_iterator_;
  const Comparator* comparator_;  // not owned
};

class WBWIIteratorImpl : public WBWIIterator {
 public:
  WBWIIteratorImpl(uint32_t column_family_id,
                   WriteBatchEntryIndex* entry_index,
                   const ReadableWriteBatch* write_batch,
                   bool ephemeral)
    : column_family_id_(column_family_id),
      write_batch_(write_batch) {
    entry_index->NewIterator(iter_, ephemeral);
  }

  virtual ~WBWIIteratorImpl() {}

  virtual bool Valid() const override {
    return iter_->Valid();
  }

  virtual void SeekToFirst() override {
    iter_->SeekToFirst();
  }

  virtual void SeekToLast() override {
    iter_->SeekToLast();
  }

  virtual void Seek(const Slice& key) override {
    WriteBatchIndexEntry search_entry(&key, column_family_id_);
    iter_->Seek(&search_entry);
  }

  virtual void SeekForPrev(const Slice& key) override {
    WriteBatchIndexEntry search_entry(&key, column_family_id_);
    iter_->SeekForPrev(&search_entry);
  }

  virtual void Next() override { iter_->Next(); }

  virtual void Prev() override { iter_->Prev(); }

  virtual WriteEntry Entry() const override {
    WriteEntry ret;
    Slice blob, xid;
    const WriteBatchIndexEntry* iter_entry = iter_->key();
    // this is guaranteed with Valid()
    assert(iter_entry != nullptr &&
           iter_entry->column_family == column_family_id_);
    auto s = write_batch_->GetEntryFromDataOffset(
        iter_entry->offset, &ret.type, &ret.key, &ret.value, &blob, &xid);
    assert(s.ok());
    assert(ret.type == kPutRecord || ret.type == kDeleteRecord ||
           ret.type == kSingleDeleteRecord || ret.type == kDeleteRangeRecord ||
           ret.type == kMergeRecord);
    return ret;
  }

  virtual Status status() const override {
    // this is in-memory data structure, so the only way status can be non-ok is
    // through memory corruption
    return Status::OK();
  }

  const WriteBatchIndexEntry* GetRawEntry() const {
    return iter_->key();
  }

 private:
  uint32_t column_family_id_;
  WBIteratorStorage<WriteBatchEntryIndex::Iterator, 24> iter_;
  const ReadableWriteBatch* write_batch_;
};

struct WriteBatchWithIndex::Rep {
  explicit Rep(const Comparator* index_comparator, size_t reserved_bytes = 0,
               size_t max_bytes = 0, bool _overwrite_key = false,
               const WriteBatchEntryIndexFactory* _index_factory = nullptr)
      : write_batch(reserved_bytes, max_bytes),
        default_comparator(index_comparator),
        index_factory(_index_factory == nullptr
                          ? WriteBatchEntryRBTreeIndexFactory()
                          : _index_factory),
        overwrite_key(_overwrite_key),
        free_entry(nullptr),
        last_entry_offset(0) {
    factory_context = index_factory->NewContext(&arena);
  }
  ~Rep() {
    for (auto& pair : entry_indices) {
      if (pair.index != nullptr) {
        pair.index->~WriteBatchEntryIndex();
      }
    }
    if (factory_context != nullptr) {
      factory_context->~WriteBatchEntryIndexContext();
    }
  }

  ReadableWriteBatch write_batch;
  const Comparator* default_comparator;
  struct ComparatorIndexPair {
    const Comparator* comparator = nullptr;
    WriteBatchEntryIndex* index = nullptr;
  };
  std::vector<ComparatorIndexPair> entry_indices;
  Arena arena;
  const WriteBatchEntryIndexFactory* index_factory;
  WriteBatchEntryIndexContext* factory_context;
  bool overwrite_key;
  WriteBatchIndexEntry* free_entry;
  size_t last_entry_offset;
  std::vector<size_t> obsolete_offsets;

  // Remember current offset of internal write batch, which is used as
  // the starting offset of the next record.
  void SetLastEntryOffset() { last_entry_offset = write_batch.GetDataSize(); }

  // Add the recent entry to the update.
  // In overwrite mode, if key already exists in the index, update it.
  void AddOrUpdateIndex(ColumnFamilyHandle* column_family);
  void AddOrUpdateIndex(uint32_t column_family_id);
  void AddOrUpdateIndex();
  void AddOrUpdateIndex(uint32_t column_family_id,
                        WriteBatchEntryIndex* entry_index);

  // Get entry_index, create if missing
  WriteBatchEntryIndex* GetEntryIndex(ColumnFamilyHandle* column_family);
  WriteBatchEntryIndex* GetEntryIndexWithCfId(uint32_t column_family_id);

  // Get comparator, nullptr if missing
  const Comparator* GetComparator(ColumnFamilyHandle* column_family);

  // Clear all updates buffered in this batch.
  void Clear();
  void ClearIndex();

  // Rebuild index by reading all records from the batch.
  // Returns non-ok status on corruption.
  Status ReBuildIndex();
};

void WriteBatchWithIndex::Rep::AddOrUpdateIndex(ColumnFamilyHandle* column_family) {
  AddOrUpdateIndex(GetColumnFamilyID(column_family), GetEntryIndex(column_family));
}

void WriteBatchWithIndex::Rep::AddOrUpdateIndex(uint32_t column_family_id) {
  AddOrUpdateIndex(column_family_id, GetEntryIndexWithCfId(column_family_id));
}

void WriteBatchWithIndex::Rep::AddOrUpdateIndex() {
  AddOrUpdateIndex(0, GetEntryIndex(nullptr));
}

void WriteBatchWithIndex::Rep::AddOrUpdateIndex(uint32_t column_family_id,
                                                WriteBatchEntryIndex* entry_index) {
  assert(entry_index == GetEntryIndexWithCfId(column_family_id));
  const std::string& wb_data = write_batch.Data();
  Slice entry_ptr = Slice(wb_data.data() + last_entry_offset,
                          wb_data.size() - last_entry_offset);
  // Extract key
  Slice key;
  bool success __attribute__((__unused__));
  success = ReadKeyFromWriteBatchEntry(&entry_ptr, &key, column_family_id != 0);
  assert(success);

  void* mem = free_entry;
  if (mem == nullptr) {
    mem = arena.Allocate(sizeof(WriteBatchIndexEntry));
  }
  auto* index_entry =
      new (mem) WriteBatchIndexEntry(last_entry_offset, column_family_id,
                                     key.data() - wb_data.data(), key.size());
  if (!entry_index->Upsert(index_entry)) {
    // overwrite key
    obsolete_offsets.push_back(index_entry->offset);
    free_entry = index_entry;
  } else {
    free_entry = nullptr;
  }
}

WriteBatchEntryIndex* WriteBatchWithIndex::Rep::GetEntryIndex(
    ColumnFamilyHandle* column_family) {
  uint32_t cf_id = GetColumnFamilyID(column_family);
  if (cf_id >= entry_indices.size()) {
    entry_indices.resize(cf_id + 1);
  }
  auto index = entry_indices[cf_id].index;
  if (index == nullptr) {
    const auto* cf_cmp = GetColumnFamilyUserComparator(column_family);
    if (cf_cmp == nullptr) {
      cf_cmp = default_comparator;
    }
    entry_indices[cf_id].comparator = cf_cmp;
    entry_indices[cf_id].index = index =
        index_factory->New(factory_context, WriteBatchKeyExtractor(&write_batch),
                           cf_cmp, &arena, overwrite_key);
  }
  return index;
}

WriteBatchEntryIndex* WriteBatchWithIndex::Rep::GetEntryIndexWithCfId(
    uint32_t column_family_id) {
  assert(column_family_id < entry_indices.size());
  auto index = entry_indices[column_family_id].index;
  if (index == nullptr) {
    const auto* cf_cmp = entry_indices[column_family_id].comparator;
    assert(cf_cmp != nullptr);
    entry_indices[column_family_id].index = index =
        index_factory->New(factory_context, WriteBatchKeyExtractor(&write_batch),
                           cf_cmp, &arena, overwrite_key);
  }
  return index;
}

const Comparator* WriteBatchWithIndex::Rep::GetComparator(
    ColumnFamilyHandle* column_family) {
  uint32_t cf_id = GetColumnFamilyID(column_family);
  if (cf_id >= entry_indices.size()) {
    return nullptr;
  }
  return entry_indices[cf_id].comparator;
}

void WriteBatchWithIndex::Rep::Clear() {
  write_batch.Clear();
  ClearIndex();
}

void WriteBatchWithIndex::Rep::ClearIndex() {
  for (auto& pair : entry_indices) {
    if (pair.index != nullptr) {
      pair.index->~WriteBatchEntryIndex();
      pair.index = nullptr;
    }

  }
  if (factory_context != nullptr) {
    factory_context->~WriteBatchEntryIndexContext();
  }
  arena.~Arena();
  new (&arena) Arena();
  factory_context = index_factory->NewContext(&arena);
  last_entry_offset = 0;
  free_entry = nullptr;
}

Status WriteBatchWithIndex::Rep::ReBuildIndex() {
  Status s;

  ClearIndex();

  if (write_batch.Count() == 0) {
    // Nothing to re-index
    return s;
  }

  size_t offset = WriteBatchInternal::GetFirstOffset(&write_batch);

  Slice input(write_batch.Data());
  input.remove_prefix(offset);

  // Loop through all entries in Rep and add each one to the index
  int found = 0;
  while (s.ok() && !input.empty()) {
    Slice key, value, blob, xid;
    uint32_t column_family_id = 0;  // default
    char tag = 0;

    // set offset of current entry for call to AddNewEntry()
    last_entry_offset = input.data() - write_batch.Data().data();

    s = ReadRecordFromWriteBatch(&input, &tag, &column_family_id, &key,
                                  &value, &blob, &xid);
    if (!s.ok()) {
      break;
    }

    switch (tag) {
      case kTypeColumnFamilyValue:
      case kTypeValue:
      case kTypeColumnFamilyDeletion:
      case kTypeDeletion:
      case kTypeColumnFamilySingleDeletion:
      case kTypeSingleDeletion:
      case kTypeColumnFamilyMerge:
      case kTypeMerge:
        found++;
        AddOrUpdateIndex(column_family_id);
        break;
      case kTypeLogData:
      case kTypeBeginPrepareXID:
      case kTypeEndPrepareXID:
      case kTypeCommitXID:
      case kTypeRollbackXID:
      case kTypeNoop:
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }

  if (s.ok() && found != write_batch.Count()) {
    s = Status::Corruption("WriteBatch has wrong count");
  }

  return s;
}

WriteBatchWithIndex::WriteBatchWithIndex(
    const Comparator* default_index_comparator, size_t reserved_bytes,
    bool overwrite_key, size_t max_bytes,
    const WriteBatchEntryIndexFactory* index_factory)
    : rep(new Rep(default_index_comparator, reserved_bytes, max_bytes,
                  overwrite_key, index_factory)) {}

WriteBatchWithIndex::~WriteBatchWithIndex() {}

WriteBatch* WriteBatchWithIndex::GetWriteBatch() { return &rep->write_batch; }

bool WriteBatchWithIndex::Collapse() {
  if (rep->obsolete_offsets.size() == 0) {
    return false;
  }
  std::sort(rep->obsolete_offsets.begin(), rep->obsolete_offsets.end());
  WriteBatch& write_batch = rep->write_batch;
  assert(write_batch.Count() != 0);
  size_t offset = WriteBatchInternal::GetFirstOffset(&write_batch);
  Slice input(write_batch.Data());
  input.remove_prefix(offset);
  std::string collapsed_buf;
  collapsed_buf.resize(WriteBatchInternal::kHeader);

  size_t count = 0;
  Status s;
  // Loop through all entries in the write batch and add keep them if they are
  // not obsolete by a newere entry.
  while (s.ok() && !input.empty()) {
    Slice key, value, blob, xid;
    uint32_t column_family_id = 0;  // default
    char tag = 0;
    // set offset of current entry for call to AddNewEntry()
    size_t last_entry_offset = input.data() - write_batch.Data().data();
    s = ReadRecordFromWriteBatch(&input, &tag, &column_family_id, &key,
                                  &value, &blob, &xid);
    if (rep->obsolete_offsets.front() == last_entry_offset) {
      rep->obsolete_offsets.erase(rep->obsolete_offsets.begin());
      continue;
    }
    switch (tag) {
      case kTypeColumnFamilyValue:
      case kTypeValue:
      case kTypeColumnFamilyDeletion:
      case kTypeDeletion:
      case kTypeColumnFamilySingleDeletion:
      case kTypeSingleDeletion:
      case kTypeColumnFamilyMerge:
      case kTypeMerge:
        count++;
        break;
      case kTypeLogData:
      case kTypeBeginPrepareXID:
      case kTypeEndPrepareXID:
      case kTypeCommitXID:
      case kTypeRollbackXID:
      case kTypeNoop:
        break;
      default:
        assert(0);
    }
    size_t entry_offset = input.data() - write_batch.Data().data();
    const std::string& wb_data = write_batch.Data();
    Slice entry_ptr = Slice(wb_data.data() + last_entry_offset,
                            entry_offset - last_entry_offset);
    collapsed_buf.append(entry_ptr.data(), entry_ptr.size());
  }
  write_batch.rep_ = std::move(collapsed_buf);
  WriteBatchInternal::SetCount(&write_batch, static_cast<int>(count));
  return true;
}

WBWIIterator* WriteBatchWithIndex::NewIterator() {
  return new WBWIIteratorImpl(0, rep->GetEntryIndex(nullptr),
                              &rep->write_batch, false);
}

WBWIIterator* WriteBatchWithIndex::NewIterator(
    ColumnFamilyHandle* column_family) {
  auto cf_id = GetColumnFamilyID(column_family);
  return new WBWIIteratorImpl(cf_id, rep->GetEntryIndex(column_family),
                              &rep->write_batch, false);
}

void WriteBatchWithIndex::NewIterator(ColumnFamilyHandle* column_family,
                                      WBWIIterator::IteratorStorage& storage,
                                      bool ephemeral) {
  static_assert(sizeof(WBWIIteratorImpl) <= sizeof storage.buffer,
                "Need larger buffer for WBWIIteratorImpl");
  storage.iter = new (storage.buffer)
      WBWIIteratorImpl(GetColumnFamilyID(column_family),
                       rep->GetEntryIndex(column_family), &rep->write_batch,
                       ephemeral);
}
Iterator* WriteBatchWithIndex::NewIteratorWithBase(
    ColumnFamilyHandle* column_family, Iterator* base_iterator) {
  if (!rep->overwrite_key) {
    assert(false);
    return nullptr;
  }
  return new BaseDeltaIterator(base_iterator, NewIterator(column_family),
                               GetColumnFamilyUserComparator(column_family));
}

Iterator* WriteBatchWithIndex::NewIteratorWithBase(Iterator* base_iterator) {
  if (rep->overwrite_key == false) {
    assert(false);
    return nullptr;
  }
  // default column family's comparator
  return new BaseDeltaIterator(base_iterator, NewIterator(),
                               rep->default_comparator);
}

Status WriteBatchWithIndex::Put(ColumnFamilyHandle* column_family,
                                const Slice& key, const Slice& value) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.Put(column_family, key, value);
  if (s.ok()) {
    rep->AddOrUpdateIndex(column_family);
  }
  return s;
}

Status WriteBatchWithIndex::Put(const Slice& key, const Slice& value) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.Put(key, value);
  if (s.ok()) {
    rep->AddOrUpdateIndex();
  }
  return s;
}

Status WriteBatchWithIndex::Delete(ColumnFamilyHandle* column_family,
                                   const Slice& key) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.Delete(column_family, key);
  if (s.ok()) {
    rep->AddOrUpdateIndex(column_family);
  }
  return s;
}

Status WriteBatchWithIndex::Delete(const Slice& key) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.Delete(key);
  if (s.ok()) {
    rep->AddOrUpdateIndex();
  }
  return s;
}

Status WriteBatchWithIndex::SingleDelete(ColumnFamilyHandle* column_family,
                                         const Slice& key) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.SingleDelete(column_family, key);
  if (s.ok()) {
    rep->AddOrUpdateIndex(column_family);
  }
  return s;
}

Status WriteBatchWithIndex::SingleDelete(const Slice& key) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.SingleDelete(key);
  if (s.ok()) {
    rep->AddOrUpdateIndex();
  }
  return s;
}

Status WriteBatchWithIndex::DeleteRange(ColumnFamilyHandle* column_family,
                                        const Slice& begin_key,
                                        const Slice& end_key) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.DeleteRange(column_family, begin_key, end_key);
  if (s.ok()) {
    rep->AddOrUpdateIndex(column_family);
  }
  return s;
}

Status WriteBatchWithIndex::DeleteRange(const Slice& begin_key,
                                        const Slice& end_key) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.DeleteRange(begin_key, end_key);
  if (s.ok()) {
    rep->AddOrUpdateIndex();
  }
  return s;
}

Status WriteBatchWithIndex::Merge(ColumnFamilyHandle* column_family,
                                  const Slice& key, const Slice& value) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.Merge(column_family, key, value);
  if (s.ok()) {
    auto size_before = rep->obsolete_offsets.size();
    rep->AddOrUpdateIndex(column_family);
    auto size_after = rep->obsolete_offsets.size();
    bool duplicate_key = size_before != size_after;
    if (!allow_dup_merge_ && duplicate_key) {
      assert(0);
      return Status::NotSupported(
          "Duplicate key with merge value is not supported yet");
    }
  }
  return s;
}

Status WriteBatchWithIndex::Merge(const Slice& key, const Slice& value) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.Merge(key, value);
  if (s.ok()) {
    auto size_before = rep->obsolete_offsets.size();
    rep->AddOrUpdateIndex();
    auto size_after = rep->obsolete_offsets.size();
    bool duplicate_key = size_before != size_after;
    if (!allow_dup_merge_ && duplicate_key) {
      assert(0);
      return Status::NotSupported(
          "Duplicate key with merge value is not supported yet");
    }
  }
  return s;
}

Status WriteBatchWithIndex::PutLogData(const Slice& blob) {
  return rep->write_batch.PutLogData(blob);
}

void WriteBatchWithIndex::Clear() { rep->Clear(); }

Status WriteBatchWithIndex::GetFromBatch(ColumnFamilyHandle* column_family,
                                         const DBOptions& options,
                                         const Slice& key, std::string* value) {
  Status s;
  MergeContext merge_context;
  const ImmutableDBOptions immuable_db_options(options);

  WriteBatchWithIndexInternal::Result result =
      WriteBatchWithIndexInternal::GetFromBatch(
          immuable_db_options, this, column_family, key, &merge_context,
          rep->GetComparator(column_family), value, rep->overwrite_key, &s);

  switch (result) {
    case WriteBatchWithIndexInternal::Result::kFound:
    case WriteBatchWithIndexInternal::Result::kError:
      // use returned status
      break;
    case WriteBatchWithIndexInternal::Result::kDeleted:
    case WriteBatchWithIndexInternal::Result::kNotFound:
      s = Status::NotFound();
      break;
    case WriteBatchWithIndexInternal::Result::kMergeInProgress:
      s = Status::MergeInProgress();
      break;
    default:
      assert(false);
  }

  return s;
}

Status WriteBatchWithIndex::GetFromBatchAndDB(DB* db,
                                              const ReadOptions& read_options,
                                              const Slice& key,
                                              std::string* value) {
  assert(value != nullptr);
  PinnableSlice pinnable_val(value);
  assert(!pinnable_val.IsPinned());
  auto s = GetFromBatchAndDB(db, read_options, db->DefaultColumnFamily(), key,
                             &pinnable_val);
  if (s.ok() && pinnable_val.IsPinned()) {
    value->assign(pinnable_val.data(), pinnable_val.size());
  }  // else value is already assigned
  return s;
}

Status WriteBatchWithIndex::GetFromBatchAndDB(DB* db,
                                              const ReadOptions& read_options,
                                              const Slice& key,
                                              PinnableSlice* pinnable_val) {
  return GetFromBatchAndDB(db, read_options, db->DefaultColumnFamily(), key,
                           pinnable_val);
}

Status WriteBatchWithIndex::GetFromBatchAndDB(DB* db,
                                              const ReadOptions& read_options,
                                              ColumnFamilyHandle* column_family,
                                              const Slice& key,
                                              std::string* value) {
  assert(value != nullptr);
  PinnableSlice pinnable_val(value);
  assert(!pinnable_val.IsPinned());
  auto s =
      GetFromBatchAndDB(db, read_options, column_family, key, &pinnable_val);
  if (s.ok() && pinnable_val.IsPinned()) {
    value->assign(pinnable_val.data(), pinnable_val.size());
  }  // else value is already assigned
  return s;
}

Status WriteBatchWithIndex::GetFromBatchAndDB(DB* db,
                                              const ReadOptions& read_options,
                                              ColumnFamilyHandle* column_family,
                                              const Slice& key,
                                              PinnableSlice* pinnable_val) {
  return GetFromBatchAndDB(db, read_options, column_family, key, pinnable_val,
                           nullptr);
}

Status WriteBatchWithIndex::GetFromBatchAndDB(
    DB* db, const ReadOptions& read_options, ColumnFamilyHandle* column_family,
    const Slice& key, PinnableSlice* pinnable_val, ReadCallback* callback) {
  Status s;
  MergeContext merge_context;
  const ImmutableDBOptions& immuable_db_options =
      static_cast_with_check<DBImpl, DB>(db->GetRootDB())
          ->immutable_db_options();

  // Since the lifetime of the WriteBatch is the same as that of the transaction
  // we cannot pin it as otherwise the returned value will not be available
  // after the transaction finishes.
  std::string& batch_value = *pinnable_val->GetSelf();
  WriteBatchWithIndexInternal::Result result =
      WriteBatchWithIndexInternal::GetFromBatch(
          immuable_db_options, this, column_family, key, &merge_context,
          rep->GetComparator(column_family), &batch_value, rep->overwrite_key, &s);

  if (result == WriteBatchWithIndexInternal::Result::kFound) {
    pinnable_val->PinSelf();
    return s;
  }
  if (result == WriteBatchWithIndexInternal::Result::kDeleted) {
    return Status::NotFound();
  }
  if (result == WriteBatchWithIndexInternal::Result::kError) {
    return s;
  }
  if (result == WriteBatchWithIndexInternal::Result::kMergeInProgress &&
      rep->overwrite_key == true) {
    // Since we've overwritten keys, we do not know what other operations are
    // in this batch for this key, so we cannot do a Merge to compute the
    // result.  Instead, we will simply return MergeInProgress.
    return Status::MergeInProgress();
  }

  assert(result == WriteBatchWithIndexInternal::Result::kMergeInProgress ||
         result == WriteBatchWithIndexInternal::Result::kNotFound);

  // Did not find key in batch OR could not resolve Merges.  Try DB.
  if (!callback) {
    s = db->Get(read_options, column_family, key, pinnable_val);
  } else {
    s = static_cast_with_check<DBImpl, DB>(db->GetRootDB())
            ->GetImpl(read_options, column_family, key, pinnable_val, nullptr,
                      callback);
  }

  if (s.ok() || s.IsNotFound()) {  // DB Get Succeeded
    if (result == WriteBatchWithIndexInternal::Result::kMergeInProgress) {
      // Merge result from DB with merges in Batch
      auto cfh = reinterpret_cast<ColumnFamilyHandleImpl*>(column_family);
      const MergeOperator* merge_operator =
          cfh->cfd()->ioptions()->merge_operator;
      Statistics* statistics = immuable_db_options.statistics.get();
      Env* env = immuable_db_options.env;
      Logger* logger = immuable_db_options.info_log.get();

      Slice* merge_data;
      if (s.ok()) {
        merge_data = pinnable_val;
      } else {  // Key not present in db (s.IsNotFound())
        merge_data = nullptr;
      }

      if (merge_operator) {
        s = MergeHelper::TimedFullMerge(
            merge_operator, key, merge_data, merge_context.GetOperands(),
            pinnable_val->GetSelf(), logger, statistics, env);
        pinnable_val->PinSelf();
      } else {
        s = Status::InvalidArgument("Options::merge_operator must be set");
      }
    }
  }

  return s;
}

void WriteBatchWithIndex::SetSavePoint() { rep->write_batch.SetSavePoint(); }

Status WriteBatchWithIndex::RollbackToSavePoint() {
  Status s = rep->write_batch.RollbackToSavePoint();

  if (s.ok()) {
    s = rep->ReBuildIndex();
    rep->obsolete_offsets.clear();
  }

  return s;
}

Status WriteBatchWithIndex::PopSavePoint() {
  return rep->write_batch.PopSavePoint();
}

void WriteBatchWithIndex::SetMaxBytes(size_t max_bytes) {
  rep->write_batch.SetMaxBytes(max_bytes);
}

}  // namespace rocksdb
#endif  // !ROCKSDB_LITE
