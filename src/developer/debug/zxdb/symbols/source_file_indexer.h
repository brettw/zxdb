// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_SOURCE_FILE_INDEXER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_SOURCE_FILE_INDEXER_H_

#include <stdint.h>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "llvm/DebugInfo/DWARF/DWARFDebugLine.h"
#include "src/developer/debug/zxdb/symbols/unit_index.h"

namespace llvm {
class DWARFContext;
class DWARFUnit;
struct DWARFSection;
}  // namespace llvm

namespace zxdb {

class DwarfBinary;

// Finds the source files that each unit of a binary has code for. The symbol index uses the result
// to find the units for a file name.
//
// The work for each unit is independent, so it runs on worker threads. Workers read only immutable
// section data and the LLVM line table header parser, and they don't use the LLVM unit objects.
// The DWARFContext of a binary isn't thread-safe, so the thread that owns the context collects the
// line table locations first (CollectLineTableJobs()). It can then continue to use the context
// while the workers run.
//
// Some string forms (DW_FORM_strx and similar) need the unit's string offsets table, which LLVM
// can change while the owning thread uses the unit. Workers skip the tables that use these forms.
// FinishInto() does them on the owning thread with the unit.

// The location of the line table of one unit.
struct LineTableJob {
  UnitIndex unit_index;
  const llvm::DWARFSection* section = nullptr;
  uint64_t offset = 0;
  uint8_t address_size = 0;

  // Bytes of the line table, used to decide how many threads to start.
  uint64_t size = 0;

  // Used only on the thread that owns the context, for string forms that need the unit.
  llvm::DWARFUnit* unit = nullptr;
};

// Adds a job for the unit's line table, if it has one. Call it on the thread that owns the
// context.
void AddLineTableJob(UnitIndex unit_index, llvm::DWARFUnit& unit, std::vector<LineTableJob>* jobs);

// Finds the line table of every unit of the binary. Call it on the thread that owns the binary's
// DWARFContext.
std::vector<LineTableJob> CollectLineTableJobs(DwarfBinary& binary);

// Returns true if the names in the prologue use string forms that need the unit to resolve.
bool LineNamesNeedUnit(const llvm::DWARFDebugLine::Prologue& prologue);

// Resolves file table entries to normalized file names and caches the result.
//
// In DWARF 5, the linker merges the strings of all line tables, so the same directory and file
// name strings have the same address in every unit. The cache key is the pair of string addresses,
// so each unique path is computed once. For other string forms the cache misses but the result is
// still correct.
class LineFilePathResolver {
 public:
  // Returns null if the entry has no name or an empty name. The pointer is valid for the lifetime of
  // this object.
  const std::string* Resolve(const llvm::DWARFDebugLine::Prologue& prologue, uint64_t file_index);

  // For testing.
  size_t cache_size() const { return by_key_.size(); }

 private:
  struct KeyHash {
    size_t operator()(const std::pair<const char*, const char*>& key) const {
      return std::hash<const void*>()(key.first) * 31 + std::hash<const void*>()(key.second);
    }
  };

  // The nodes of an unordered_map don't move, so the returned pointers stay valid.
  std::unordered_map<std::pair<const char*, const char*>, std::string, KeyHash> by_key_;
};

// Use each object for one set of jobs: the constructor starts the work, and FinishInto() collects
// it once.
class SourceFileIndexer {
 public:
  // Maps each file name to the units that have code for it, in unit order.
  using FileIndex = std::map<std::string, std::vector<UnitIndex>>;

  struct Options {
    // The number of worker threads. 0 runs the work in FinishInto(). By default, the number
    // depends on the amount of line table data. See the .cc file.
    std::optional<size_t> thread_count;
  };

  // Starts the work for the jobs. The context must outlive this object.
  SourceFileIndexer(const llvm::DWARFContext& context, std::vector<LineTableJob> jobs,
                    Options options = Options());

  // Waits for any workers that are still running.
  ~SourceFileIndexer();

  // Waits for the work and adds the results to the index. Call it once, on the thread that owns
  // the context. Each unit is added at most once for each file, and units are added in job order.
  void FinishInto(FileIndex* files);

  // The number of worker threads. For testing.
  size_t worker_count() const { return threads_.size(); }

 private:
  // Resolved file names for one job. The pointers point into a worker resolver in resolvers_ or
  // into the local resolver of FinishInto(). Both live until FinishInto() merges the results.
  struct JobResult {
    std::vector<const std::string*> names;

    // The names need the unit, so FinishInto() must do the job.
    bool needs_unit = false;
  };

  void RunJobs(LineFilePathResolver& resolver);
  void RunJob(size_t index, LineFilePathResolver& resolver, const llvm::DWARFUnit* unit);

  const llvm::DWARFContext& context_;
  bool finished_ = false;

  std::vector<LineTableJob> jobs_;
  std::vector<JobResult> results_;  // Indexed like jobs_.

  std::atomic<size_t> next_job_ = 0;
  // One for each worker thread. The vector is sized before the threads start and doesn't change
  // while they run.
  std::vector<LineFilePathResolver> resolvers_;
  std::vector<std::thread> threads_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_SOURCE_FILE_INDEXER_H_
