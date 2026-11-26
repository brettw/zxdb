// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf_die.h"

namespace zxdb {

const char* DwarfDie::short_name() const { return die_.getShortName(); }

const char* DwarfDie::linkage_name() const { return die_.getLinkageName(); }

}  // namespace zxdb
