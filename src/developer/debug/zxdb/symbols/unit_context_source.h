// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_UNIT_CONTEXT_SOURCE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_UNIT_CONTEXT_SOURCE_H_

#include <memory>

#include "src/developer/debug/zxdb/common/err_or.h"
#include "src/developer/debug/zxdb/symbols/unit_index.h"

namespace zxdb {

class DwarfBinary;
class UnitContext;

// Identifies an explicit address table provider. Its binary must outlive a split binary that
// caches views of the provider's sections. A null binary allows lookup by DWO ID.
struct UnitContextSource {
  const DwarfBinary* binary = nullptr;
  UnitIndex index;
  bool operator==(const UnitContextSource&) const = default;
};

using UnitContextResult = ErrOr<std::shared_ptr<const UnitContext>>;

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_UNIT_CONTEXT_SOURCE_H_
