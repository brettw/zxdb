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

#include "src/developer/debug/zxdb/symbols/line_program.h"
#include "src/developer/debug/zxdb/symbols/unit_index.h"

namespace llvm {
class DWARFUnit;
}  // namespace llvm

namespace zxdb {

class DwarfBinary;

// Finds the source files that each unit of a binary has code for. The symbol index uses the result
// to find the units for a file name.
//
// The work for each unit is independent, so it runs on worker threads. Workers read only immutable
// section data, and they don't use LLVM objects. The DWARFContext of a binary isn't thread-safe,
// so the thread that owns the context collects the line table locations and string sections first
// (CollectLineTableJobs()). This includes the string offsets base of each unit for
// DW_FORM_strx. The thread can then continue to use the context while the workers run.

// The location of the line table of one unit.
struct LineTableJob {
  UnitIndex unit_index;
  std::string_view section;  // The line section data.
  uint64_t offset = 0;
  bool little_endian = true;

  // Bytes of the line table, used to decide how many threads to start.
  uint64_t size = 0;

  // The string sections of the unit, for the names in the line table.
  LineStringSections strings;

  // The lowest address of a code section of the binary. Sequences that start below it are for
  // removed code, and their files aren't indexed. See IsStrippedSequenceStart().
  uint64_t first_code_address = 0;
};

// Adds a job for the unit's line table, if it has one. Call it on the thread that owns the
// context. first_code_address is DwarfBinary::GetFirstCodeAddress().
void AddLineTableJob(UnitIndex unit_index, llvm::DWARFUnit& unit, uint64_t first_code_address,
                     std::vector<LineTableJob>* jobs);

// Finds the line table of every unit of the binary. Call it on the thread that owns the binary's
// DWARFContext.
std::vector<LineTableJob> CollectLineTableJobs(DwarfBinary& binary);

// Resolves file table entries to normalized file names and caches the result.
//
// In DWARF 5, the linker merges the strings of all line tables, so the same directory and file
// name strings have the same address in every unit. The cache key is the pair of string addresses,
// so each unique path is computed once. For other string forms the cache misses but the result is
// still correct.
class LineFilePathResolver {
 public:
  // Returns null if the entry has no name or an empty name. The pointer is valid for the lifetime
  // of this object.
  const std::string* Resolve(LineFileEntries files, uint64_t file_index);

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

  // Starts the work for the jobs. The section data of the jobs must outlive this object.
  explicit SourceFileIndexer(std::vector<LineTableJob> jobs, Options options = Options());

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
  using JobResult = std::vector<const std::string*>;

  void RunJobs(LineFilePathResolver& resolver);
  void RunJob(size_t index, LineFilePathResolver& resolver);

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
