// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_FUNCTION_ADDRESS_MAP_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_FUNCTION_ADDRESS_MAP_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "src/developer/debug/zxdb/common/address_range.h"

namespace zxdb {

class DwarfBinary;
class DwarfDie;
struct UnitContextSource;

// Maps code addresses in one unit to offsets of subprogram or inlined subroutine DIEs. Later DIEs
// take precedence over earlier ones in overlapping ranges, so inline instances cover their callers.
class FunctionAddressMap {
 public:
  FunctionAddressMap(DwarfDie die, const DwarfBinary& binary, UnitContextSource skeleton);

  std::optional<uint64_t> Lookup(uint64_t relative_address) const;

 private:
  struct Entry {
    AddressRange range;
    uint64_t offset;
  };

  void ResolveOverlaps(std::vector<Entry> input);

  std::vector<Entry> entries_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_FUNCTION_ADDRESS_MAP_H_
