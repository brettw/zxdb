// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Source for the type unit fixtures. With -fdebug-types-section, GCC puts each struct in a type
// unit. The DW_AT_type of the global variable references its type unit directly with
// DW_FORM_ref_sig8. See dwarf_symbol_factory_unittest.cc.

struct TypeUnitInner {
  int a;
};

struct TypeUnitOuter {
  TypeUnitInner inner;
  TypeUnitInner* ptr;
};

TypeUnitOuter type_unit_outer;
