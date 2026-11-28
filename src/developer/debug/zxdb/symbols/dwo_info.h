// Copyright 2024 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWO_INFO_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWO_INFO_H_

#include <memory>

#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/symbols/dwarf_symbol_factory.h"
#include "src/developer/debug/zxdb/symbols/dwarf_unit.h"
#include "src/developer/debug/zxdb/symbols/skeleton_unit.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace zxdb {

class DwarfBinary;
class ModuleSymbols;

// Holds the per-DWO file state. Each DWO file has its own binary and associated symbol factory.
// This class glues everything together for the DWO via the DwarfSymbolFactory::Delegate interface.
//
// The ModuleSymbolsImpl class will bring all of the DwoInfo factories together with the factory
// for the main binary to give the full view of the module.
class DwoInfo final : public DwarfSymbolFactory::Delegate {
 public:
  // Must call Load() which must succeed before using.
  DwoInfo(SkeletonUnit skeleton, fxl::WeakPtr<ModuleSymbols> module_symbols);
  ~DwoInfo();

  Err Load(const std::string& name, const std::string& binary_name);

  DwarfBinary* binary() { return binary_.get(); }
  DwarfUnit GetCompileUnit() const;
  const SkeletonUnit& skeleton() const { return skeleton_; }
  const SymbolFactory* symbol_factory();

  // DwarfSymbolFactory::Delegate implementation.
  std::string GetBuildDirForSymbolFactory() override { return skeleton_.comp_dir; }
  fxl::WeakPtr<ModuleSymbols> GetModuleSymbols() override { return module_symbols_; }
  CompileUnit* GetSkeletonCompileUnit() override;

 private:
  std::unique_ptr<DwarfBinary> binary_;  // Guaranteed non-null after Load() succeeds.
  UnitIndex compile_unit_index_;
  SkeletonUnit skeleton_;  // Refers to the skeleton in the main binary corresponding to this DWO.
  fxl::WeakPtr<ModuleSymbols> module_symbols_;

  // Lazily constructed & cached unit corresponding to the skeleton.
  fxl::RefPtr<CompileUnit> skeleton_unit_;

  fxl::WeakPtrFactory<DwarfSymbolFactory::Delegate> weak_factory_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWO_INFO_H_
