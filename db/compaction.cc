//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/compaction.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <vector>

#include "db/column_family.h"
#include "rocksdb/compaction_filter.h"
#include "util/string_util.h"
#include "util/sync_point.h"

namespace rocksdb {

uint64_t TotalFileSize(const std::vector<FileMetaData*>& files) {
  uint64_t sum = 0;
  for (size_t i = 0; i < files.size() && files[i]; i++) {
    sum += files[i]->fd.GetFileSize();
  }
  return sum;
}

std::pair<ptrdiff_t, ptrdiff_t> FindLevelOverlap(
    const std::vector<FileMetaData*>& files,
    const InternalKeyComparator& ic,
    const InternalKey* smallest,
    const InternalKey* largest) {
  ptrdiff_t left = 0;
  ptrdiff_t right = (ptrdiff_t)files.size() - 1;
  if (smallest != nullptr) {
    left = std::lower_bound(
               files.begin(), files.end(), *smallest,
               [&ic](const FileMetaData* l, const InternalKey& r) {
                   return ic.Compare(l->largest(), r) < 0;
               }) - files.begin();
  }
  if (largest != nullptr) {
    right = std::upper_bound(
                files.begin(), files.end(), *largest,
                [&ic](const InternalKey& l, const FileMetaData* r) {
                    return ic.Compare(l, r->smallest()) < 0;
                }) - files.begin() - 1;
  }
  return std::make_pair(left, right);
}

void Compaction::SetInputVersion(Version* _input_version) {
  input_version_ = _input_version;
  cfd_ = input_version_->cfd();

  cfd_->Ref();
  input_version_->Ref();
  edit_.SetColumnFamily(cfd_->GetID());
}

void Compaction::GetBoundaryKeys(
    VersionStorageInfo* vstorage,
    const std::vector<CompactionInputFiles>& inputs, Slice* smallest_user_key,
    Slice* largest_user_key) {
  bool initialized = false;
  const Comparator* ucmp = vstorage->InternalComparator()->user_comparator();
  for (size_t i = 0; i < inputs.size(); ++i) {
    if (inputs[i].files.empty()) {
      continue;
    }
    if (inputs[i].level == 0) {
      // we need to consider all files on level 0
      for (const auto* f : inputs[i].files) {
        const Slice& start_user_key = f->smallest().user_key();
        if (!initialized ||
            ucmp->Compare(start_user_key, *smallest_user_key) < 0) {
          *smallest_user_key = start_user_key;
        }
        const Slice& end_user_key = f->largest().user_key();
        if (!initialized ||
            ucmp->Compare(end_user_key, *largest_user_key) > 0) {
          *largest_user_key = end_user_key;
        }
        initialized = true;
      }
    } else {
      // we only need to consider the first and last file
      const Slice& start_user_key = inputs[i].files[0]->smallest().user_key();
      if (!initialized ||
          ucmp->Compare(start_user_key, *smallest_user_key) < 0) {
        *smallest_user_key = start_user_key;
      }
      const Slice& end_user_key = inputs[i].files.back()->largest().user_key();
      if (!initialized || ucmp->Compare(end_user_key, *largest_user_key) > 0) {
        *largest_user_key = end_user_key;
      }
      initialized = true;
    }
  }
}

// helper function to determine if compaction is creating files at the
// bottommost level
bool Compaction::IsBottommostLevel(
    int output_level, VersionStorageInfo* vstorage,
    const std::vector<CompactionInputFiles>& inputs) {
  int output_l0_idx;
  if (output_level == 0) {
    output_l0_idx = 0;
    for (const auto* file : vstorage->LevelFiles(0)) {
      if (inputs[0].files.back() == file) {
        break;
      }
      ++output_l0_idx;
    }
    assert(static_cast<size_t>(output_l0_idx) < vstorage->LevelFiles(0).size());
  } else {
    output_l0_idx = -1;
  }
  Slice smallest_key, largest_key;
  GetBoundaryKeys(vstorage, inputs, &smallest_key, &largest_key);
  return !vstorage->RangeMightExistAfterSortedRun(smallest_key, largest_key,
                                                  output_level, output_l0_idx);
}

// test function to validate the functionality of IsBottommostLevel()
// function -- determines if compaction with inputs and storage is bottommost
bool Compaction::TEST_IsBottommostLevel(
    int output_level, VersionStorageInfo* vstorage,
    const std::vector<CompactionInputFiles>& inputs) {
  return IsBottommostLevel(output_level, vstorage, inputs);
}

bool Compaction::IsFullCompaction(
    VersionStorageInfo* vstorage,
    const std::vector<CompactionInputFiles>& inputs) {
  size_t num_files_in_compaction = 0;
  size_t total_num_files = 0;
  for (int l = 0; l < vstorage->num_levels(); l++) {
    total_num_files += vstorage->NumLevelFiles(l);
  }
  for (size_t i = 0; i < inputs.size(); i++) {
    num_files_in_compaction += inputs[i].size();
  }
  return num_files_in_compaction == total_num_files;
}

Compaction::Compaction(VersionStorageInfo* vstorage,
                       const ImmutableCFOptions& _immutable_cf_options,
                       const MutableCFOptions& _mutable_cf_options,
                       std::vector<CompactionInputFiles> _inputs,
                       int _output_level, uint64_t _target_file_size,
                       uint64_t _max_compaction_bytes, uint32_t _output_path_id,
                       CompressionType _compression,
                       std::vector<FileMetaData*> _grandparents,
                       bool _manual_compaction, double _score,
                       bool _deletion_compaction,
                       bool _disable_subcompaction,
                       bool _enable_partial_remove,
                       const std::vector<CompactionInputFilesRange>& _input_range,
                       CompactionReason _compaction_reason)
    : input_vstorage_(vstorage),
      start_level_(_inputs[0].level),
      output_level_(_output_level),
      max_output_file_size_(_target_file_size),
      max_compaction_bytes_(_max_compaction_bytes),
      immutable_cf_options_(_immutable_cf_options),
      mutable_cf_options_(_mutable_cf_options),
      input_version_(nullptr),
      number_levels_(vstorage->num_levels()),
      cfd_(nullptr),
      output_path_id_(_output_path_id),
      output_compression_(_compression),
      deletion_compaction_(_deletion_compaction),
      disable_subcompaction_(_disable_subcompaction),
      enable_partial_remove_(_enable_partial_remove),
      input_range_(_input_range),
      inputs_(std::move(_inputs)),
      grandparents_(std::move(_grandparents)),
      score_(_score),
      bottommost_level_(IsBottommostLevel(output_level_, vstorage, inputs_)),
      is_full_compaction_(IsFullCompaction(vstorage, inputs_)),
      is_manual_compaction_(_manual_compaction),
      is_trivial_move_(false),
      is_finished_(false),
      compaction_reason_(_compaction_reason) {
  MarkFilesBeingCompacted(true);
  if (is_manual_compaction_) {
    compaction_reason_ = CompactionReason::kManualCompaction;
  }

#ifndef NDEBUG
  for (size_t i = 1; i < inputs_.size(); ++i) {
    assert(inputs_[i].level > inputs_[i - 1].level);
  }
#endif

  // setup input_levels_
  {
    input_levels_.resize(num_input_levels());
    for (size_t which = 0; which < num_input_levels(); which++) {
      DoGenerateLevelFilesBrief(&input_levels_[which], inputs_[which].files,
                                &arena_);
    }
  }

  GetBoundaryKeys(vstorage, inputs_, &smallest_user_key_, &largest_user_key_);
  // shrink to input range
  auto &ic = immutable_cf_options_.internal_comparator;
  if (!input_range_.empty()) {
    if (input_range_.front().smallest != nullptr &&
        ic.Compare(smallest_user_key_,
                   input_range_.front().smallest->user_key()) < 0) {
      smallest_user_key_ = input_range_.front().smallest->user_key();
    }
    if (input_range_.back().largest != nullptr &&
        ic.Compare(largest_user_key_,
                   input_range_.back().largest->user_key()) > 0) {
      largest_user_key_ = input_range_.back().largest->user_key();
    }
  }
}

Compaction::~Compaction() {
  if (input_version_ != nullptr) {
    input_version_->Unref();
  }
  if (cfd_ != nullptr) {
    if (cfd_->Unref()) {
      delete cfd_;
    }
  }
}

bool Compaction::InputCompressionMatchesOutput() const {
  int base_level = input_vstorage_->base_level();
  bool matches = (GetCompressionType(immutable_cf_options_, input_vstorage_,
                                     mutable_cf_options_, start_level_,
                                     base_level) == output_compression_);
  if (matches) {
    TEST_SYNC_POINT("Compaction::InputCompressionMatchesOutput:Matches");
    return true;
  }
  TEST_SYNC_POINT("Compaction::InputCompressionMatchesOutput:DidntMatch");
  return matches;
}

bool Compaction::IsTrivialMove() const {
  // Avoid a move if there is lots of overlapping grandparent data.
  // Otherwise, the move could create a parent file that will require
  // a very expensive merge later on.
  // If start_level_== output_level_, the purpose is to force compaction
  // filter to be applied to that level, and thus cannot be a trivial move.

  // kUniversalTrivialMove don't need check
  if (compaction_reason_ == CompactionReason::kUniversalTrivialMove) {
    assert(is_trivial_move_);
    return true;
  }

  // Check if start level have files with overlapping ranges
  if (start_level_ == 0 && input_vstorage_->level0_non_overlapping() == false) {
    // We cannot move files from L0 to L1 if the files are overlapping
    return false;
  }

  if (is_manual_compaction_ &&
      (immutable_cf_options_.compaction_filter != nullptr ||
       immutable_cf_options_.compaction_filter_factory != nullptr)) {
    // This is a manual compaction and we have a compaction filter that should
    // be executed, we cannot do a trivial move
    return false;
  }

  // Used in universal compaction, where trivial move can be done if the
  // input files are non overlapping
  if ((immutable_cf_options_.compaction_options_universal.allow_trivial_move) &&
      (output_level_ != 0)) {
    return is_trivial_move_;
  }

  if (!(start_level_ != output_level_ && num_input_levels() == 1 &&
          input(0, 0)->fd.GetPathId() == output_path_id() &&
          InputCompressionMatchesOutput())) {
    return false;
  }

  // assert inputs_.size() == 1

  for (const auto& file : inputs_.front().files) {
    std::vector<FileMetaData*> file_grand_parents;
    if (output_level_ + 1 >= number_levels_) {
      continue;
    }
    input_vstorage_->GetOverlappingInputs(output_level_ + 1, &file->smallest(),
                                          &file->largest(), &file_grand_parents);
    const auto compaction_size =
        file->fd.GetFileSize() + TotalFileSize(file_grand_parents);
    if (compaction_size > max_compaction_bytes_) {
      return false;
    }
  }

  return true;
}

void Compaction::AddInputDeletions(VersionEdit* out_edit) {
  for (size_t which = 0; which < num_input_levels(); which++) {
    for (size_t i = 0; i < inputs_[which].size(); i++) {
      out_edit->DeleteFile(level(which), inputs_[which][i]->fd.GetNumber());
    }
  }
}

bool Compaction::KeyNotExistsBeyondOutputLevel(
    const Slice& user_key, std::vector<size_t>* level_ptrs) const {
  assert(input_version_ != nullptr);
  assert(level_ptrs != nullptr);
  assert(level_ptrs->size() == static_cast<size_t>(number_levels_));
  if (bottommost_level_) {
    return true;
  } else if (output_level_ != 0 &&
             cfd_->ioptions()->compaction_style == kCompactionStyleLevel) {
    // Maybe use binary search to find right entry instead of linear search?
    const Comparator* user_cmp = cfd_->user_comparator();
    for (int lvl = output_level_ + 1; lvl < number_levels_; lvl++) {
      const std::vector<FileMetaData*>& files =
          input_vstorage_->LevelFiles(lvl);
      for (; level_ptrs->at(lvl) < files.size(); level_ptrs->at(lvl)++) {
        auto* f = files[level_ptrs->at(lvl)];
        if (user_cmp->Compare(user_key, f->largest().user_key()) <= 0) {
          // We've advanced far enough
          if (user_cmp->Compare(user_key, f->smallest().user_key()) >= 0) {
            // Key falls in this file's range, so definitely
            // exists beyond output level
            return false;
          }
          break;
        }
      }
    }
    return true;
  }
  return false;
}

// Mark (or clear) each file that is being compacted
void Compaction::MarkFilesBeingCompacted(bool mark_as_compacted) {
  for (size_t i = 0; i < num_input_levels(); i++) {
    for (size_t j = 0; j < inputs_[i].size(); j++) {
      assert(mark_as_compacted ? !inputs_[i][j]->being_compacted
                               : inputs_[i][j]->being_compacted);
      inputs_[i][j]->being_compacted = mark_as_compacted;
    }
  }
}

// Sample output:
// If compacting 3 L0 files, 2 L3 files and 1 L4 file, and outputting to L5,
// print: "3@0 + 2@3 + 1@4 files to L5"
const char* Compaction::InputLevelSummary(
    InputLevelSummaryBuffer* scratch) const {
  int len = 0;
  bool is_first = true;
  for (auto& input_level : inputs_) {
    if (input_level.empty()) {
      continue;
    }
    if (!is_first) {
      len +=
          snprintf(scratch->buffer + len, sizeof(scratch->buffer) - len, " + ");
    } else {
      is_first = false;
    }
    len += snprintf(scratch->buffer + len, sizeof(scratch->buffer) - len,
                    "%" ROCKSDB_PRIszt "@%d", input_level.size(),
                    input_level.level);
  }
  snprintf(scratch->buffer + len, sizeof(scratch->buffer) - len,
           " files to L%d", output_level());

  return scratch->buffer;
}

uint64_t Compaction::CalculateTotalInputSize() const {
  uint64_t size = 0;
  for (auto& input_level : inputs_) {
    for (auto f : input_level.files) {
      size += f->fd.GetFileSize();
    }
  }
  return size;
}

void Compaction::ReleaseCompactionFiles(Status status) {
  MarkFilesBeingCompacted(false);
  cfd_->compaction_picker()->ReleaseCompactionFiles(this, status);
}

void Compaction::ResetNextCompactionIndex() {
  assert(input_version_ != nullptr);
  input_vstorage_->ResetNextCompactionIndex(start_level_);
}

namespace {
int InputSummary(const std::vector<FileMetaData*>& files, char* output,
                 int len) {
  *output = '\0';
  int write = 0;
  for (size_t i = 0; i < files.size(); i++) {
    int sz = len - write;
    int ret;
    char sztxt[16];
    FileMetaData* f = files[i];
    AppendHumanBytes(f->fd.GetFileSize(), sztxt, 16);
    if (!f->partial_removed) {
      ret = snprintf(output + write, sz, "%" PRIu64 "(%s) ",
                     f->fd.GetNumber(), sztxt);
    } else if (f->range_set.size() == 2) {
      ret = snprintf(output + write, sz, "%" PRIu64 "(%s,%d%%) ",
                     f->fd.GetNumber(), sztxt, 100 - f->partial_removed);
    } else {
      ret = snprintf(output + write, sz, "%" PRIu64 "(%s,%d%%,%zdFrags) ",
                     f->fd.GetNumber(), sztxt, 100 - f->partial_removed,
                     f->range_set.size() / 2);
    }
    if (ret < 0 || ret >= sz) break;
    write += ret;
  }
  // if files.size() is non-zero, overwrite the last space
  return write - !!files.size();
}
}  // namespace

void Compaction::Summary(char* output, int len) {
  int write =
      snprintf(output, len, "Base version %" PRIu64 " Base level %d, inputs: [",
               input_version_->GetVersionNumber(), start_level_);
  if (write < 0 || write >= len) {
    return;
  }

  for (size_t level_iter = 0; level_iter < num_input_levels(); ++level_iter) {
    if (level_iter > 0) {
      write += snprintf(output + write, len - write, "], [");
      if (write < 0 || write >= len) {
        return;
      }
    }
    write +=
        InputSummary(inputs_[level_iter].files, output + write, len - write);
    if (write < 0 || write >= len) {
      return;
    }
  }

  snprintf(output + write, len - write, "]");
}

uint64_t Compaction::OutputFilePreallocationSize() const {
  uint64_t preallocation_size = 0;

  if (max_output_file_size_ != port::kMaxUint64 &&
      (cfd_->ioptions()->compaction_style == kCompactionStyleLevel ||
       output_level() > 0)) {
    preallocation_size = max_output_file_size_;
  } else {
    for (const auto& level_files : inputs_) {
      for (const auto& file : level_files.files) {
        preallocation_size += file->fd.GetFileSize();
      }
    }
  }
  // Over-estimate slightly so we don't end up just barely crossing
  // the threshold
  return preallocation_size + (preallocation_size / 10);
}

std::unique_ptr<CompactionFilter> Compaction::CreateCompactionFilter() const {
  if (!cfd_->ioptions()->compaction_filter_factory) {
    return nullptr;
  }

  CompactionFilter::Context context;
  context.is_full_compaction = is_full_compaction_;
  context.is_manual_compaction = is_manual_compaction_;
  context.column_family_id = cfd_->GetID();
  return cfd_->ioptions()->compaction_filter_factory->CreateCompactionFilter(
      context);
}

bool Compaction::IsOutputLevelEmpty() const {
  return inputs_.back().level != output_level_ || inputs_.back().empty();
}

bool Compaction::ShouldFormSubcompactions() const {
  if (immutable_cf_options_.max_subcompactions <= 1 || cfd_ == nullptr ||
      disable_subcompaction_) {
    return false;
  }
  if (cfd_->ioptions()->compaction_style == kCompactionStyleLevel) {
    return start_level_ == 0 && output_level_ > 0 && !IsOutputLevelEmpty();
  } else if (cfd_->ioptions()->compaction_style == kCompactionStyleUniversal) {
    return number_levels_ > 1 && output_level_ > 0;
  } else {
    return false;
  }
}

uint64_t Compaction::MaxInputFileCreationTime() const {
  uint64_t max_creation_time = 0;
  for (const auto& file : inputs_[0].files) {
    if (file->fd.table_reader != nullptr &&
        file->fd.table_reader->GetTableProperties() != nullptr) {
      uint64_t creation_time =
          file->fd.table_reader->GetTableProperties()->creation_time;
      max_creation_time = std::max(max_creation_time, creation_time);
    }
  }
  return max_creation_time;
}

}  // namespace rocksdb
