// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Source for the DW_FORM_LLVM_addrx_offset fixture. With "-mllvm -minimize-addr-in-v5=Form", Clang
// writes the DW_AT_low_pc of the second and third functions as an index into the address table plus
// an offset from that address. See dwarf_symbol_factory_unittest.cc.

int AddrxOffsetFirst(int a) { return a + 1; }

int AddrxOffsetSecond(int a) { return a * 3; }

int AddrxOffsetThird(int a) { return a - 7; }
