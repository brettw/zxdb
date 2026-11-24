// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_EXPR_PERMISSIVE_INPUT_LOCATION_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_EXPR_PERMISSIVE_INPUT_LOCATION_H_

#include "src/developer/debug/zxdb/symbols/input_location.h"

namespace zxdb {

class Location;
class LoadedModuleSymbols;
class ProcessSymbols;
struct ResolveOptions;

// Resolve relative source names across namespaces and alternate function spellings.
std::vector<Location> ResolvePermissiveInputLocation(const LoadedModuleSymbols& module,
                                                     const ResolveOptions& options,
                                                     const InputLocation& input);
std::vector<Location> ResolvePermissiveInputLocations(const ProcessSymbols* process,
                                                      const ResolveOptions& options,
                                                      const std::vector<InputLocation>& input);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_EXPR_PERMISSIVE_INPUT_LOCATION_H_
