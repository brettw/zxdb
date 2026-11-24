// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/permissive_input_location.h"

#include <set>

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/expr/expr_parser.h"
#include "src/developer/debug/zxdb/expr/template_type_extractor.h"
#include "src/developer/debug/zxdb/symbols/elf_symbol.h"
#include "src/developer/debug/zxdb/symbols/function.h"
#include "src/developer/debug/zxdb/symbols/index_test_support.h"
#include "src/developer/debug/zxdb/symbols/mock_module_symbols.h"
#include "src/developer/debug/zxdb/symbols/namespace.h"
#include "src/developer/debug/zxdb/symbols/process_symbols_test_setup.h"
#include "src/developer/debug/zxdb/symbols/symbol_test_parent_setter.h"
#include "src/developer/debug/zxdb/symbols/test_symbol_module.h"

namespace zxdb {

// The complexities of the FindName backend are all tested in its own unit tests. Here we're just
// concerned that the integration between FindName and the symbol system is working.
TEST(PermissiveInputLocation, ResolveAcrossNamespaces) {
  ProcessSymbolsTestSetup setup;
  MockModuleSymbols* module_symbols = setup.InjectMockModule();

  auto& index_root = module_symbols->index().root();
  SymbolContext symbol_context(ProcessSymbolsTestSetup::kDefaultLoadAddress);

  // Index these functions:
  // - ::Foo()
  // - ::std::Foo()

  // ::Foo().
  const char kFooName[] = "Foo";
  auto foo = fxl::MakeRefCounted<Function>(DwarfTag::kSubprogram);
  foo->set_assigned_name(kFooName);
  TestIndexedSymbol foo_indexed(module_symbols, &index_root, kFooName, foo);

  Location foo_location(ProcessSymbolsTestSetup::kDefaultLoadAddress + 0x1234,
                        FileLine("file.cc", 34), 3, symbol_context, foo);
  module_symbols->AddSymbolLocations(
      Identifier(IdentifierQualification::kGlobal, IdentifierComponent("Foo")), {foo_location});

  // "std" namespace.
  const char kStdName[] = "std";
  auto std_ns_symbol = fxl::MakeRefCounted<Namespace>(kStdName);
  auto std_ns = index_root.AddChild(IndexNode::Kind::kNamespace, kStdName);

  // ::std::Foo().
  auto std_foo = fxl::MakeRefCounted<Function>(DwarfTag::kSubprogram);
  SymbolTestParentSetter std_foo_parent(std_foo, std_ns_symbol);
  std_foo->set_assigned_name(kFooName);
  TestIndexedSymbol std_foo_indexed(module_symbols, std_ns, kFooName, std_foo);

  Location std_foo_location(ProcessSymbolsTestSetup::kDefaultLoadAddress + 0x5678,
                            FileLine("other.cc", 12), 4, symbol_context, std_foo);
  Identifier std_foo_name(IdentifierQualification::kGlobal, IdentifierComponent("std"));
  std_foo_name.AppendComponent(IdentifierComponent("Foo"));
  module_symbols->AddSymbolLocations(std_foo_name, {std_foo_location});

  InputLocation foo_input(Identifier(IdentifierComponent("Foo")));

  // Additionally gives an address-based input.
  InputLocation addr_input(ProcessSymbolsTestSetup::kDefaultLoadAddress + 0x9999);
  Location addr_location(addr_input.address, FileLine("addr.cc", 99), 1, symbol_context);
  module_symbols->AddSymbolLocations(addr_input.address, {addr_location});

  // Test ResolvePermissiveInputLocations().
  auto resolved =
      ResolvePermissiveInputLocations(&setup.process(), ResolveOptions(), {addr_input, foo_input});
  EXPECT_EQ(3u, resolved.size());
  EXPECT_TRUE(addr_location.EqualsIgnoringSymbol(resolved[0]))
      << "got " << resolved[0].GetDebugString() << " expected " << addr_location.GetDebugString();
  EXPECT_TRUE(foo_location.EqualsIgnoringSymbol(resolved[1]))
      << "got " << resolved[1].GetDebugString() << " expected " << foo_location.GetDebugString();
  EXPECT_TRUE(std_foo_location.EqualsIgnoringSymbol(resolved[2]))
      << "got " << resolved[2].GetDebugString() << " expected "
      << std_foo_location.GetDebugString();
}

TEST(PermissiveInputLocation, OmitNamespacesButKeepClassScopes) {
  TestSymbolModule file(TestSymbolModule::kCheckedIn);
  ASSERT_TRUE(file.Init("", true, ParseQualifiedFunctionName).ok());
  ProcessSymbolsTestSetup setup;
  setup.InjectModule(TestSymbolModule::GetCheckedInTestFileName(), "test",
                     ProcessSymbolsTestSetup::kDefaultLoadAddress, RefPtrTo(file.symbols()));
  auto resolve = [&](const char* name) {
    return ResolvePermissiveInputLocations(&setup.process(), {},
                                           {InputLocation(TestSymbolModule::SplitName(name))});
  };
  EXPECT_FALSE(resolve("NamespaceFunction").empty());
  EXPECT_FALSE(resolve("MyClass::MyMemberOne").empty());
  EXPECT_TRUE(resolve("MyMemberOne").empty());
  EXPECT_TRUE(resolve("Inner::MyMemberTwo").empty());
}

TEST(PermissiveInputLocation, ParseOnlyMatchingElfBaseNames) {
  Index index([](const std::string& name) {
    EXPECT_EQ("ns::match", name);
    return ParseQualifiedFunctionName(name);
  });
  ElfSymbolRecord match(ElfSymbolType::kNormal, 1, 1, "_ZN2ns5matchEv", true);
  ElfSymbolRecord other(ElfSymbolType::kNormal, 2, 1, "_ZN2ns5otherEv", true);
  auto& names = index.function_names();
  names.AddLinkageName(match.linkage_name.c_str(), &match);
  names.AddLinkageName(other.linkage_name.c_str(), &other);
  size_t matches = 0;
  names.Visit(Identifier("match"), false, [&](const Identifier&, const FunctionNameIndex::Ref& ref) {
    EXPECT_EQ(&match, std::get<const ElfSymbolRecord*>(ref));
    ++matches;
    return true;
  });
  EXPECT_EQ(1u, matches);
}

TEST(PermissiveInputLocation, AlternateFunctionNames) {
  for (std::string accelerator : {"Dwarf", "Disable"}) {
    SCOPED_TRACE(accelerator);
    std::string path = TestSymbolModule::GetTestDataDir() + "function_names/" + accelerator + ".so";
    TestSymbolModule file(path, path);
    ASSERT_TRUE(file.Init("", true, ParseQualifiedFunctionName).ok());
    ASSERT_EQ(accelerator == "Dwarf", file.symbols()->HasDebugNames());

    ProcessSymbolsTestSetup setup;
    constexpr uint64_t base = ProcessSymbolsTestSetup::kDefaultLoadAddress;
    setup.InjectModule(path, accelerator, base, RefPtrTo(file.symbols()));
    auto resolve = [&](std::string query) {
      Identifier id;
      EXPECT_FALSE(ExprParser::ParseIdentifier(query, &id).has_error());
      return ResolvePermissiveInputLocations(&setup.process(), {}, {InputLocation(id)});
    };

    // Both overloads have DWARF, and only the first also has a matching ELF name.
    auto overloads = resolve("a::b::f");
    ASSERT_EQ(2u, overloads.size());
    EXPECT_NE(overloads[0].address(), overloads[1].address());
    std::set<int> lines;
    for (const auto& location : overloads) {
      EXPECT_GT(location.address(), base);
      EXPECT_TRUE(location.symbol().Get()->As<Function>());
      lines.insert(location.file_line().line());
    }
    EXPECT_EQ((std::set<int>{1, 2}), lines);
    EXPECT_EQ(2u, resolve("b::f").size());
    EXPECT_TRUE(resolve("::b::f").empty());
    EXPECT_EQ(3u, resolve("f").size());
    EXPECT_EQ(1u, resolve("a::Box<b::T>::f").size());
    EXPECT_TRUE(resolve("b::T::f").empty());
    EXPECT_EQ(1u, resolve("$(a::Box<b::T>::f)").size());

    auto rust = resolve("b::rust");
    ASSERT_EQ(1u, rust.size());
    EXPECT_TRUE(rust.front().symbol().Get()->As<ElfSymbol>());
    EXPECT_EQ(1u, resolve("plain").size());
    EXPECT_EQ(1u, resolve("a::b::generic").size());
    EXPECT_EQ(1u, resolve("S::$(new)").size());
    EXPECT_EQ(1u, resolve("::r::S::$(new)").size());
    EXPECT_EQ(1u, resolve("foo::tagged").size());
    EXPECT_EQ(1u, resolve("$anon::hidden").size());
    EXPECT_EQ(1u, resolve("specialized<int>").size());
    EXPECT_TRUE(resolve("specialized<double>").empty());
    EXPECT_TRUE(resolve("clone").empty());
    EXPECT_TRUE(resolve("thunkOnly").empty());
    EXPECT_EQ(1u, resolve("$elf(_ZN1a1b1fEi)").size());

    // Exact module lookup uses either .debug_names or the ordinary DWARF index.
    auto exact = file.symbols()->ResolveInputLocation(
        SymbolContext(base), InputLocation(TestSymbolModule::SplitName("a::b::f")));
    ASSERT_EQ(2u, exact.size());
  }
}

}  // namespace zxdb
