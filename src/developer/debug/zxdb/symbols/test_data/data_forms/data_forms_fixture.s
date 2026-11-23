# Copyright 2026 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Hand-written DWARF 5 for dwarf_symbol_factory_unittest.cc. It has array bounds and constants in
# the forms that GCC writes (DW_AT_upper_bound and DW_FORM_data<n>), and some forms that compilers
# rarely write.
#
#   Unit A (DW_LANG_C99):
#     0  DW_TAG_base_type "unsigned long"
#     1  DW_TAG_base_type "int"
#     2  DW_TAG_base_type "unsigned char"
#     3  int[10]: DW_AT_upper_bound 9 (data1)
#     4  int[10]: DW_AT_count 10 (data1)
#     5  int[256]: DW_AT_upper_bound 0xff (data1), unsigned basis type
#     6  int[0]: DW_AT_upper_bound -1 (sdata), no basis type
#     7  int[10]: DW_AT_lower_bound 5, DW_AT_upper_bound 14
#     8  int[3][5]: DW_AT_upper_bound 2 and 4
#     9  int[]: DW_AT_upper_bound is an expression
#     10 DW_TAG_enumeration_type "Unsigned" (type "unsigned long")
#     11 DW_TAG_enumeration_type "Signed" (type "int")
#     12 DW_TAG_enumeration_type "Untyped" (no type)
#     13 DW_TAG_variable "const_char": DW_AT_const_value 0x61 (data1)
#     14 DW_TAG_variable "const_u64": DW_AT_const_value 0xffffffffffffffff (data8)
#     15 DW_TAG_variable "const_short": DW_AT_const_value 0x1234 (data2)
#     16 DW_TAG_variable "const_16": DW_AT_const_value of 16 bytes (data16)
#     17 DW_TAG_base_type "i8"
#     18 DW_TAG_base_type "i32"
#     19 DW_TAG_structure_type "Small": a Rust-style enumeration with an i8 discriminant
#     20 DW_TAG_structure_type "Wide": a Rust-style enumeration with an i32 discriminant
#   Unit B (DW_LANG_Fortran90, default lower bound 1):
#     0  DW_TAG_base_type "integer"
#     1  integer[10]: DW_AT_upper_bound 10

        .section .debug_abbrev,"",@progbits
        # 1: DW_TAG_compile_unit with children.
        .uleb128 1, 0x11
        .byte 1
        .uleb128 0x03, 0x08             # DW_AT_name, DW_FORM_string
        .uleb128 0x13, 0x05             # DW_AT_language, DW_FORM_data2
        .byte 0, 0
        # 2: DW_TAG_base_type.
        .uleb128 2, 0x24
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x3e, 0x0b             # DW_AT_encoding, DW_FORM_data1
        .uleb128 0x0b, 0x0b             # DW_AT_byte_size, DW_FORM_data1
        .byte 0, 0
        # 3: DW_TAG_array_type with children.
        .uleb128 3, 0x01
        .byte 1
        .uleb128 0x49, 0x13             # DW_AT_type, DW_FORM_ref4
        .byte 0, 0
        # 4: DW_TAG_subrange_type with a type and DW_AT_upper_bound in data1.
        .uleb128 4, 0x21
        .byte 0
        .uleb128 0x49, 0x13             # DW_AT_type
        .uleb128 0x2f, 0x0b             # DW_AT_upper_bound, DW_FORM_data1
        .byte 0, 0
        # 5: DW_TAG_subrange_type with a type and DW_AT_count in data1.
        .uleb128 5, 0x21
        .byte 0
        .uleb128 0x49, 0x13             # DW_AT_type
        .uleb128 0x37, 0x0b             # DW_AT_count, DW_FORM_data1
        .byte 0, 0
        # 6: DW_TAG_subrange_type without a type, DW_AT_upper_bound in sdata.
        .uleb128 6, 0x21
        .byte 0
        .uleb128 0x2f, 0x0d             # DW_AT_upper_bound, DW_FORM_sdata
        .byte 0, 0
        # 7: DW_TAG_subrange_type with both bounds.
        .uleb128 7, 0x21
        .byte 0
        .uleb128 0x49, 0x13             # DW_AT_type
        .uleb128 0x22, 0x0b             # DW_AT_lower_bound, DW_FORM_data1
        .uleb128 0x2f, 0x0b             # DW_AT_upper_bound, DW_FORM_data1
        .byte 0, 0
        # 8: DW_TAG_subrange_type with an expression for the upper bound.
        .uleb128 8, 0x21
        .byte 0
        .uleb128 0x49, 0x13             # DW_AT_type
        .uleb128 0x2f, 0x18             # DW_AT_upper_bound, DW_FORM_exprloc
        .byte 0, 0
        # 9: DW_TAG_enumeration_type with a type and children.
        .uleb128 9, 0x04
        .byte 1
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x49, 0x13             # DW_AT_type
        .uleb128 0x0b, 0x0b             # DW_AT_byte_size
        .byte 0, 0
        # 10: DW_TAG_enumeration_type without a type, with children.
        .uleb128 10, 0x04
        .byte 1
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x0b, 0x0b             # DW_AT_byte_size
        .byte 0, 0
        # 11: DW_TAG_enumerator in data1.
        .uleb128 11, 0x28
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x1c, 0x0b             # DW_AT_const_value, DW_FORM_data1
        .byte 0, 0
        # 12: DW_TAG_enumerator in data8.
        .uleb128 12, 0x28
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x1c, 0x07             # DW_AT_const_value, DW_FORM_data8
        .byte 0, 0
        # 13: DW_TAG_enumerator in sdata.
        .uleb128 13, 0x28
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x1c, 0x0d             # DW_AT_const_value, DW_FORM_sdata
        .byte 0, 0
        # 14: DW_TAG_variable with DW_AT_const_value in data1.
        .uleb128 14, 0x34
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x49, 0x13             # DW_AT_type
        .uleb128 0x1c, 0x0b             # DW_AT_const_value, DW_FORM_data1
        .byte 0, 0
        # 15: DW_TAG_variable with DW_AT_const_value in data8.
        .uleb128 15, 0x34
        .byte 0
        .uleb128 0x03, 0x08
        .uleb128 0x49, 0x13
        .uleb128 0x1c, 0x07             # DW_FORM_data8
        .byte 0, 0
        # 16: DW_TAG_variable with DW_AT_const_value in data2.
        .uleb128 16, 0x34
        .byte 0
        .uleb128 0x03, 0x08
        .uleb128 0x49, 0x13
        .uleb128 0x1c, 0x05             # DW_FORM_data2
        .byte 0, 0
        # 17: DW_TAG_variable with DW_AT_const_value in data16.
        .uleb128 17, 0x34
        .byte 0
        .uleb128 0x03, 0x08
        .uleb128 0x1c, 0x1e             # DW_FORM_data16
        .byte 0, 0
        # 18: DW_TAG_structure_type with children.
        .uleb128 18, 0x13
        .byte 1
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x0b, 0x0b             # DW_AT_byte_size
        .byte 0, 0
        # 19: DW_TAG_variant_part with children.
        .uleb128 19, 0x33
        .byte 1
        .uleb128 0x15, 0x13             # DW_AT_discr, DW_FORM_ref4
        .byte 0, 0
        # 20: DW_TAG_member.
        .uleb128 20, 0x0d
        .byte 0
        .uleb128 0x03, 0x08             # DW_AT_name
        .uleb128 0x49, 0x13             # DW_AT_type
        .uleb128 0x38, 0x0b             # DW_AT_data_member_location, DW_FORM_data1
        .byte 0, 0
        # 21 to 24: DW_TAG_variant with DW_AT_discr_value in sdata, data1, data2, and data8.
        .uleb128 21, 0x19
        .byte 0
        .uleb128 0x16, 0x0d             # DW_AT_discr_value, DW_FORM_sdata
        .byte 0, 0
        .uleb128 22, 0x19
        .byte 0
        .uleb128 0x16, 0x0b             # DW_FORM_data1
        .byte 0, 0
        .uleb128 23, 0x19
        .byte 0
        .uleb128 0x16, 0x05             # DW_FORM_data2
        .byte 0, 0
        .uleb128 24, 0x19
        .byte 0
        .uleb128 0x16, 0x07             # DW_FORM_data8
        .byte 0, 0
        # 25: DW_TAG_variant without a discriminant value (the default).
        .uleb128 25, 0x19
        .byte 0
        .byte 0, 0
        .byte 0

        .section .debug_info,"",@progbits
.Lunit_a:
        .long .Lunit_a_end - .Lunit_a_version
.Lunit_a_version:
        .short 5                        # Version.
        .byte 1                         # DW_UT_compile.
        .byte 8                         # Address size.
        .long 0                         # Abbreviation offset.
        .uleb128 1
        .asciz "data_forms_a.c"
        .short 0x000c                   # DW_LANG_C99.
.Lulong:
        .uleb128 2
        .asciz "unsigned long"
        .byte 0x07                      # DW_ATE_unsigned.
        .byte 8
.Lint:
        .uleb128 2
        .asciz "int"
        .byte 0x05                      # DW_ATE_signed.
        .byte 4
.Luchar:
        .uleb128 2
        .asciz "unsigned char"
        .byte 0x08                      # DW_ATE_unsigned_char.
        .byte 1
        # 3: int[10] with an upper bound.
        .uleb128 3
        .long .Lint - .Lunit_a
        .uleb128 4
        .long .Lulong - .Lunit_a
        .byte 9
        .byte 0
        # 4: int[10] with a count.
        .uleb128 3
        .long .Lint - .Lunit_a
        .uleb128 5
        .long .Lulong - .Lunit_a
        .byte 10
        .byte 0
        # 5: int[256]. The upper bound isn't sign-extended because the basis type is unsigned.
        .uleb128 3
        .long .Lint - .Lunit_a
        .uleb128 4
        .long .Lulong - .Lunit_a
        .byte 0xff
        .byte 0
        # 6: int[0].
        .uleb128 3
        .long .Lint - .Lunit_a
        .uleb128 6
        .sleb128 -1
        .byte 0
        # 7: int[10] with a lower bound.
        .uleb128 3
        .long .Lint - .Lunit_a
        .uleb128 7
        .long .Lulong - .Lunit_a
        .byte 5
        .byte 14
        .byte 0
        # 8: int[3][5].
        .uleb128 3
        .long .Lint - .Lunit_a
        .uleb128 4
        .long .Lulong - .Lunit_a
        .byte 2
        .uleb128 4
        .long .Lulong - .Lunit_a
        .byte 4
        .byte 0
        # 9: int[] with a dynamic upper bound.
        .uleb128 3
        .long .Lint - .Lunit_a
        .uleb128 8
        .long .Lulong - .Lunit_a
        .uleb128 1
        .byte 0x30                      # DW_OP_lit0.
        .byte 0
        # 10: "Unsigned" enumeration.
        .uleb128 9
        .asciz "Unsigned"
        .long .Lulong - .Lunit_a
        .byte 8
        .uleb128 11
        .asciz "kOne"
        .byte 1
        .uleb128 11
        .asciz "kHigh"
        .byte 0xff                      # 255, not -1.
        .uleb128 12
        .asciz "kMax"
        .quad 0xffffffffffffffff
        .byte 0
        # 11: "Signed" enumeration.
        .uleb128 9
        .asciz "Signed"
        .long .Lint - .Lunit_a
        .byte 4
        .uleb128 11
        .asciz "kTwo"
        .byte 2
        .uleb128 11
        .asciz "kMinusOne"
        .byte 0xff                      # -1 because the type is signed.
        .uleb128 13
        .asciz "kMinusThree"
        .sleb128 -3
        .byte 0
        # 12: "Untyped" enumeration.
        .uleb128 10
        .asciz "Untyped"
        .byte 1
        .uleb128 11
        .asciz "kA"
        .byte 0x80                      # 128.
        .uleb128 13
        .asciz "kB"
        .sleb128 -1
        .byte 0
        # 13 to 16: constants.
        .uleb128 14
        .asciz "const_char"
        .long .Luchar - .Lunit_a
        .byte 0x61
        .uleb128 15
        .asciz "const_u64"
        .long .Lulong - .Lunit_a
        .quad 0xffffffffffffffff
        .uleb128 16
        .asciz "const_short"
        .long .Lint - .Lunit_a
        .short 0x1234
        .uleb128 17
        .asciz "const_16"
        .byte 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
        .byte 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
.Li8:
        .uleb128 2
        .asciz "i8"
        .byte 0x05                      # DW_ATE_signed.
        .byte 1
.Li32:
        .uleb128 2
        .asciz "i32"
        .byte 0x05
        .byte 4
        # 19: "Small". -1 is written in sdata and in data2 the way rustc writes it.
        .uleb128 18
        .asciz "Small"
        .byte 2
        .uleb128 19
        .long .Lsmall_tag - .Lunit_a
.Lsmall_tag:
        .uleb128 20
        .asciz "tag"
        .long .Li8 - .Lunit_a
        .byte 0
        .uleb128 21
        .sleb128 -1
        .uleb128 23
        .short 0x00ff
        .uleb128 22
        .byte 2
        .uleb128 25
        .byte 0                         # End of the variant part.
        .byte 0                         # End of "Small".
        # 20: "Wide". -5 is written in sdata and in data8 the way rustc writes it.
        .uleb128 18
        .asciz "Wide"
        .byte 8
        .uleb128 19
        .long .Lwide_tag - .Lunit_a
.Lwide_tag:
        .uleb128 20
        .asciz "tag"
        .long .Li32 - .Lunit_a
        .byte 0
        .uleb128 21
        .sleb128 -5
        .uleb128 24
        .quad 0xfffffffb
        .byte 0
        .byte 0
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
        .asciz "data_forms_b.f90"
        .short 0x0008                   # DW_LANG_Fortran90.
.Linteger:
        .uleb128 2
        .asciz "integer"
        .byte 0x05                      # DW_ATE_signed.
        .byte 4
        # 1: integer[10] with the default lower bound of 1.
        .uleb128 3
        .long .Linteger - .Lunit_b
        .uleb128 4
        .long .Linteger - .Lunit_b
        .byte 10
        .byte 0
        .byte 0                         # End of unit B.
.Lunit_b_end:
