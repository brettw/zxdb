// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/permissive_input_location.h"

#include <set>

#include "src/developer/debug/zxdb/expr/find_name.h"
#include "src/developer/debug/zxdb/expr/found_name.h"
#include "src/developer/debug/zxdb/expr/parsed_identifier.h"
#include "src/developer/debug/zxdb/symbols/loaded_module_symbols.h"
#include "src/developer/debug/zxdb/symbols/process_symbols.h"

namespace zxdb {

std::vector<Location> ResolvePermissiveInputLocation(const LoadedModuleSymbols& module,
                                                     const ResolveOptions& options,
                                                     const InputLocation& input) {
  if (input.type != InputLocation::Type::kName ||
      (input.name.components().size() == 1 &&
       input.name.components().front().special() != SpecialIdentifier::kNone))
    return module.ResolveInputLocation(input, options);

  const ModuleSymbols* symbols = module.module_symbols();
  if (!symbols)
    return {};

  FindNameOptions opts(FindNameOptions::kNoKinds);
  opts.max_results = FindNameOptions::kAllResults;
  opts.search_mode = FindNameOptions::kAllNamespaces;
  opts.find_functions = true;
  opts.find_vars = true;

  std::vector<FoundName> found;
  FindIndexedNameInModule(opts, symbols, ParsedIdentifier(), ToParsedIdentifier(input.name), false,
                          &found);
  std::vector<LazySymbol> matches;
  for (const auto& match : found) {
    if (match.function())
      matches.emplace_back(match.function());
    else if (match.variable())
      matches.emplace_back(match.variable());
  }
  return symbols->ResolveNamedSymbols(module.symbol_context(), options, input.name,
                                      std::move(matches));
}

std::vector<Location> ResolvePermissiveInputLocations(const ProcessSymbols* process,
                                                      const ResolveOptions& options,
                                                      const std::vector<InputLocation>& input) {
  std::vector<Location> result;
  if (!process)
    return result;
  std::set<std::pair<uint64_t, LazySymbol>> named_locations;
  for (const auto& in : input) {
    if (in.type == InputLocation::Type::kName) {
      for (const auto* module : process->GetLoadedModuleSymbols()) {
        auto locations = ResolvePermissiveInputLocation(*module, options, in);
        for (auto& loc : locations) {
          // A single name is already resolved once per module. Multiple input spellings can
          // refer to the same function, for example a method and its qualified name.
          if (input.size() == 1 || named_locations.emplace(loc.address(), loc.symbol()).second)
            result.push_back(std::move(loc));
        }
      }
    } else {
      auto locations = process->ResolveInputLocation(in, options);
      result.insert(result.end(), locations.begin(), locations.end());
    }
  }
  return result;
}

}  // namespace zxdb
