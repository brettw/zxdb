// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_FILTER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_FILTER_H_

#include <zircon/types.h>

#include <optional>
#include <string>

#include "src/developer/debug/ipc/records.h"
#include "src/developer/debug/zxdb/client/client_object.h"
#include "src/developer/debug/zxdb/client/setting_store.h"

namespace zxdb {

class SettingSchema;

class Filter : public ClientObject {
 public:
  explicit Filter(Session* session);
  explicit Filter(Session* session, std::optional<debug_ipc::Filter> filter);

  bool is_valid() const {
    return filter_.type != debug_ipc::Filter::Type::kUnset &&
           (filter_.job_koid != 0 || !filter_.pattern.empty());
  }

  void SetType(debug_ipc::Filter::Type type);
  debug_ipc::Filter::Type type() const { return filter_.type; }

  // Sets the pattern and the type that debug_ipc::ResolveFilterType gives for the session
  // platform. The resolution can trim the pattern. Before a connection, the platform isn't known.
  // Then the filter keeps the original pattern and resolves it again when the session connects.
  void SetPatternAndResolveType(const std::string& pattern, bool exact);

  // Resolves the pattern from SetPatternAndResolveType again if the platform wasn't known when it
  // was set. Returns true if the filter changed. The caller must sync the filters.
  bool ResolvePendingPattern();

  void SetPattern(const std::string& pattern);
  const std::string& pattern() const { return filter_.pattern; }

  void SetJobKoid(zx_koid_t job_koid);
  zx_koid_t job_koid() const { return filter_.job_koid; }

  void SetWeak(bool weak);
  bool weak() const { return filter_.config.weak; }

  void SetRecursive(bool recursive);
  bool recursive() const { return filter_.config.recursive; }

  void SetJobOnly(bool job_only);
  bool job_only() const { return filter_.config.job_only; }
  bool never_attach() const { return filter_.config.never_attach; }

  // Accessing the underlying filter storage.
  void SetFilter(const debug_ipc::Filter& filter);
  const debug_ipc::Filter& filter() const { return filter_; }
  SettingStore& settings() { return settings_; }

  static fxl::RefPtr<SettingSchema> GetSchema();

 private:
  // A pattern that waits for the session platform. See SetPatternAndResolveType.
  struct UnresolvedPattern {
    std::string pattern;
    bool exact = false;
  };

  // Sync the filter to the debug_agent. Must be called when the filter changes.
  void Sync();

  // Sets the type and pattern from the pattern for the current session platform.
  void ResolvePattern(const std::string& pattern, bool exact);

  // Implements the SettingStore interface for the Filter (uses composition instead of inheritance
  // to keep the Filter API simpler).
  class Settings : public SettingStore {
   public:
    explicit Settings(Filter* filter);

   protected:
    SettingValue GetStorageValue(const std::string& key) const override;
    Err SetStorageValue(const std::string& key, SettingValue value) override;

   private:
    Filter* filter_;  // Object that owns us.
  };

  Settings settings_;

  // The real filter.
  debug_ipc::Filter filter_;

  std::optional<UnresolvedPattern> unresolved_pattern_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_FILTER_H_
