# Copyright 2026 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Hand-written DWARF 5 for dwarf_symbol_factory_unittest.cc. It has DIE chains that compilers
# don't always write: definitions with DW_AT_specification, abstract origins in another unit, and
# attributes split between a DIE and its abstract origin.
#
#   Unit A (DW_AT_addr_base 0x8):
#     DW_TAG_structure_type "Outer"
#       DW_TAG_structure_type "Inner" (declaration)
#       DW_TAG_subprogram "Method" (declaration)
#       DW_TAG_variable "kStatic" (declaration)
#     DW_TAG_base_type "int"
#     DW_TAG_structure_type (definition, specification is "Inner")
#       DW_TAG_member "x"
#     DW_TAG_subprogram (abstract instance, specification is "Method")
#       DW_TAG_variable "local_static" (with DW_AT_location)
#     DW_TAG_subprogram (concrete instance of the abstract instance)
#     DW_TAG_variable (definition, specification is "kStatic")
#     DW_TAG_subprogram "PairOnly" (DW_AT_low_pc, abstract origin below)
#     DW_TAG_subprogram "PairOnly" (abstract instance with DW_AT_high_pc)
#   Unit B (DW_AT_addr_base 0x18):
#     DW_TAG_subprogram (concrete instance of the abstract instance in unit A)
#       DW_TAG_variable (abstract origin is "local_static" in unit A)
#       DW_TAG_variable "local_b" (with DW_AT_location)

        .text
.Lfunc_a:
        .byte 0xc3, 0xc3, 0xc3, 0xc3
.Lfunc_a_end:
.Lfunc_pair:
        .byte 0xc3, 0xc3, 0xc3, 0xc3
.Lfunc_b:
        .byte 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3
.Lfunc_b_end:

        .section .debug_abbrev,"",@progbits
        # 1: DW_TAG_compile_unit with children.
        .uleb128 1, 0x11
        .byte 1
        .uleb128 0x03, 0x08             # DW_AT_name, DW_FORM_string
        .uleb128 0x13, 0x05             # DW_AT_language, DW_FORM_data2
        .uleb128 0x73, 0x17             # DW_AT_addr_base, DW_FORM_sec_offset
        .byte 0, 0
        # 2: DW_TAG_structure_type with children.
        .uleb128 2, 0x13
        .byte 1
        .uleb128 0x03, 0x08             # DW_AT_name
        .byte 0, 0
        # 3: DW_TAG_structure_type declaration.
        .uleb128 3, 0x13
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x3c, 0x19             # DW_AT_declaration, DW_FORM_flag_present
        .byte 0, 0
        # 4: DW_TAG_subprogram declaration.
        .uleb128 4, 0x2e
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x3c, 0x19             # DW_AT_declaration
        .uleb128 0x3f, 0x19             # DW_AT_external, DW_FORM_flag_present
        .uleb128 0x3b, 0x0b             # DW_AT_decl_line, DW_FORM_data1
        .byte 0, 0
        # 5: DW_TAG_variable declaration.
        .uleb128 5, 0x34
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x3c, 0x19             # DW_AT_declaration
        .uleb128 0x3f, 0x19             # DW_AT_external
        .uleb128 0x49, 0x13             # DW_AT_type, DW_FORM_ref4
        .byte 0, 0
        # 6: DW_TAG_base_type.
        .uleb128 6, 0x24
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x3e, 0x0b             # DW_AT_encoding, DW_FORM_data1
        .uleb128 0x0b, 0x0b             # DW_AT_byte_size, DW_FORM_data1
        .byte 0, 0
        # 7: DW_TAG_structure_type definition with children.
        .uleb128 7, 0x13
        .byte 1
        .uleb128 0x47, 0x13             # DW_AT_specification, DW_FORM_ref4
        .uleb128 0x0b, 0x0b             # DW_AT_byte_size
        .byte 0, 0
        # 8: DW_TAG_member.
        .uleb128 8, 0x0d
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x49, 0x13             # DW_AT_type
        .uleb128 0x38, 0x0b             # DW_AT_data_member_location, DW_FORM_data1
        .byte 0, 0
        # 9: DW_TAG_subprogram abstract instance with children.
        .uleb128 9, 0x2e
        .byte 1
        .uleb128 0x47, 0x13             # DW_AT_specification
        .uleb128 0x20, 0x0b             # DW_AT_inline, DW_FORM_data1
        .uleb128 0x3b, 0x0b             # DW_AT_decl_line
        .byte 0, 0
        # 10: DW_TAG_variable with a location.
        .uleb128 10, 0x34
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x49, 0x13             # DW_AT_type
        .uleb128 0x02, 0x18             # DW_AT_location, DW_FORM_exprloc
        .byte 0, 0
        # 11: DW_TAG_subprogram concrete instance.
        .uleb128 11, 0x2e
        .byte 0
        .uleb128 0x31, 0x13             # DW_AT_abstract_origin, DW_FORM_ref4
        .uleb128 0x11, 0x01             # DW_AT_low_pc, DW_FORM_addr
        .uleb128 0x12, 0x06             # DW_AT_high_pc, DW_FORM_data4
        .byte 0, 0
        # 12: DW_TAG_variable definition.
        .uleb128 12, 0x34
        .byte 0
        .uleb128 0x47, 0x13             # DW_AT_specification
        .uleb128 0x02, 0x18             # DW_AT_location
        .byte 0, 0
        # 13: DW_TAG_subprogram with only DW_AT_low_pc.
        .uleb128 13, 0x2e
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x31, 0x13             # DW_AT_abstract_origin
        .uleb128 0x11, 0x01             # DW_AT_low_pc
        .byte 0, 0
        # 14: DW_TAG_subprogram abstract instance with DW_AT_high_pc.
        .uleb128 14, 0x2e
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x20, 0x0b             # DW_AT_inline
        .uleb128 0x12, 0x06             # DW_AT_high_pc
        .byte 0, 0
        # 15: DW_TAG_subprogram concrete instance in another unit, with children.
        .uleb128 15, 0x2e
        .byte 1
        .uleb128 0x31, 0x10             # DW_AT_abstract_origin, DW_FORM_ref_addr
        .uleb128 0x11, 0x01             # DW_AT_low_pc
        .uleb128 0x12, 0x06             # DW_AT_high_pc
        .byte 0, 0
        # 16: DW_TAG_variable concrete instance in another unit.
        .uleb128 16, 0x34
        .byte 0
        .uleb128 0x31, 0x10             # DW_AT_abstract_origin, DW_FORM_ref_addr
        .byte 0, 0
        # 17: DW_TAG_variable with a location and no type.
        .uleb128 17, 0x34
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x02, 0x18             # DW_AT_location
        .byte 0, 0
        .byte 0

        .section .debug_info,"",@progbits
.Ldebug_info_start:

.Lunit_a:
        .long .Lunit_a_end - .Lunit_a_version
.Lunit_a_version:
        .short 5                        # Version.
        .byte 1                         # DW_UT_compile.
        .byte 8                         # Address size.
        .long 0                         # Abbreviation offset.
        .uleb128 1
        .asciz "specification_a.cc"
        .short 0x0004                   # DW_LANG_C_plus_plus.
        .long 0x8                       # DW_AT_addr_base.
.Louter:
        .uleb128 2
        .asciz "Outer"
.Ldecl_inner:
        .uleb128 3
        .asciz "Inner"
.Ldecl_method:
        .uleb128 4
        .asciz "Method"
        .byte 30                        # DW_AT_decl_line.
.Ldecl_static:
        .uleb128 5
        .asciz "kStatic"
        .long .Lint - .Lunit_a
        .byte 0                         # End of Outer.
.Lint:
        .uleb128 6
        .asciz "int"
        .byte 5                         # DW_ATE_signed.
        .byte 4
.Ldef_inner:
        .uleb128 7
        .long .Ldecl_inner - .Lunit_a
        .byte 4
        .uleb128 8
        .asciz "x"
        .long .Lint - .Lunit_a
        .byte 0
        .byte 0                         # End of the Inner definition.
.Labstract_method:
        .uleb128 9
        .long .Ldecl_method - .Lunit_a
        .byte 1                         # DW_INL_inlined.
        .byte 31                        # DW_AT_decl_line.
.Labstract_local:
        .uleb128 10
        .asciz "local_static"
        .long .Lint - .Lunit_a
        .uleb128 2                      # Expression length.
        .byte 0xa1, 0x00                # DW_OP_addrx 0.
        .byte 0                         # End of the abstract instance.
.Lconcrete_method_a:
        .uleb128 11
        .long .Labstract_method - .Lunit_a
        .quad .Lfunc_a
        .long .Lfunc_a_end - .Lfunc_a
.Ldef_static:
        .uleb128 12
        .long .Ldecl_static - .Lunit_a
        .uleb128 9                      # Expression length.
        .byte 0x03                      # DW_OP_addr.
        .quad 0x2000
.Lpair_concrete:
        .uleb128 13
        .asciz "PairOnly"
        .long .Labstract_pair - .Lunit_a
        .quad .Lfunc_pair
.Labstract_pair:
        .uleb128 14
        .asciz "PairOnly"
        .byte 1                         # DW_INL_inlined.
        .long 4                         # DW_AT_high_pc.
        .byte 0                         # End of unit A.
.Lunit_a_end:

.Lunit_b:
        .long .Lunit_b_end - .Lunit_b_version
.Lunit_b_version:
        .short 5
        .byte 1
        .byte 8
        .long 0
        .uleb128 1
        .asciz "specification_b.cc"
        .short 0x0004
        .long 0x18                      # DW_AT_addr_base.
.Lconcrete_method_b:
        .uleb128 15
        .long .Labstract_method - .Ldebug_info_start
        .quad .Lfunc_b
        .long .Lfunc_b_end - .Lfunc_b
        .uleb128 16
        .long .Labstract_local - .Ldebug_info_start
        .uleb128 17
        .asciz "local_b"
        .uleb128 2                      # Expression length.
        .byte 0xa1, 0x01                # DW_OP_addrx 1.
        .byte 0                         # End of the concrete instance.
        .byte 0                         # End of unit B.
.Lunit_b_end:
