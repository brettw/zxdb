// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Source for the inlined parameter order fixture. With -O2, Clang omits the formal parameter
// entries of some unused parameters from the concrete inlined instances: "unused_a" of
// InlineParamsCallee(), and "this" of Method(). See dwarf_symbol_factory_unittest.cc.

extern "C" int InlineParamsSink(int);

static inline __attribute__((always_inline)) int InlineParamsCallee(int unused_a, int b, int c) {
  int local = b * c;
  return InlineParamsSink(local);
}

int InlineParamsCaller(int x) { return InlineParamsCallee(x, x + 1, x + 2); }

struct InlineParamsClass {
  int value;
  __attribute__((always_inline)) int Method(int unused, int param) {
    return InlineParamsSink(param);
  }
};

int InlineParamsMethodCaller(InlineParamsClass* object, int x) { return object->Method(x, x); }

// ELF labels identify instructions in each function regardless of where the compiler and linker
// place them. Clang generates all DWARF for the calls and inline scopes.
static inline __attribute__((always_inline)) int NestedInlineInner(int x) {
  asm volatile(".globl NestedInlineInnerMarker\nNestedInlineInnerMarker:\n nop" : "+r"(x));
  return InlineParamsSink(x);
}

static inline __attribute__((always_inline)) int NestedInlineMiddle(int x) {
  asm volatile(".globl NestedInlineMiddleMarker\nNestedInlineMiddleMarker:\n nop" : "+r"(x));
  return NestedInlineInner(x + 1) + x;
}

int NestedInlineCaller(int x) {
  asm volatile(".globl NestedInlineCallerMarker\nNestedInlineCallerMarker:\n nop" : "+r"(x));
  return NestedInlineMiddle(x + 2) + x;
}
