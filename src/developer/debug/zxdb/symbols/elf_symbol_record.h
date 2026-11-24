// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_ELF_SYMBOL_RECORD_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_ELF_SYMBOL_RECORD_H_

#include <stdint.h>

#include <optional>
#include <string>

namespace zxdb {

enum class ElfSymbolType {
  // Normal ELF symbol.
  kNormal,

  // "PLT" addresses are the thunk used for jumping to imported functions in other modules.
  kPlt,
};

// Represents a symbol read from the ELF file. This holds the mangled and unmangled names for
// convenience for the index. Normal external users will use the ElfSymbol class which is
// constructed from this structure.
struct ElfSymbolRecord {
  ElfSymbolRecord() = default;

  // Stores the linkage name. Demangling is deferred until the name is requested.
  explicit ElfSymbolRecord(ElfSymbolType type, uint64_t relative_address, uint64_t size,
                           const std::string& linkage_name, bool is_function = false);

  ElfSymbolType type = ElfSymbolType::kNormal;

  // Defined ELF functions also participate in ordinary function-name lookup.
  bool is_function = false;

  // Address relative to the beginning of the associated module of this symbol.
  uint64_t relative_address = 0;

  // Size of this symbol.
  uint64_t size = 0;

  // The name from the ELF file. For C++ programs this will be the mangled name.
  std::string linkage_name;

  // Full unmangled name. Will be the same as the linkage_name if unmangling fails.
  //
  // Symbols for function names will include parens. This means it will NOT parse as an Identifier.
  // TODO(bug 41928) make Identifier support function parameters.
  const std::string& GetUnmangledName() const;

 private:
  mutable std::optional<std::string> unmangled_name_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_ELF_SYMBOL_RECORD_H_
