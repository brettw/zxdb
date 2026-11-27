# Copyright 2026 Brett Wilson. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Hand-written DWARF 5 for function_address_map_unittest.cc. The second subprogram overlaps
# the outer subprogram and crosses several previously described inline ranges.

        .text
        .space 0x90, 0xc3

        .section .debug_abbrev,"",@progbits
        .uleb128 1, 0x11                 # DW_TAG_compile_unit
        .byte 1
        .byte 0, 0
        .uleb128 2, 0x2e                 # DW_TAG_subprogram with children
        .byte 1
        .uleb128 0x03, 0x08              # DW_AT_name, DW_FORM_string
        .uleb128 0x11, 0x01              # DW_AT_low_pc, DW_FORM_addr
        .uleb128 0x12, 0x06              # DW_AT_high_pc, DW_FORM_data4
        .byte 0, 0
        .uleb128 3, 0x1d                 # DW_TAG_inlined_subroutine with children
        .byte 1
        .uleb128 0x03, 0x08
        .uleb128 0x55, 0x17              # DW_AT_ranges, DW_FORM_sec_offset
        .byte 0, 0
        .uleb128 4, 0x1d                 # DW_TAG_inlined_subroutine
        .byte 0
        .uleb128 0x03, 0x08
        .uleb128 0x11, 0x01
        .uleb128 0x12, 0x06
        .byte 0, 0
        .uleb128 5, 0x2e                 # DW_TAG_subprogram with an absolute high PC
        .byte 0
        .uleb128 0x03, 0x08
        .uleb128 0x11, 0x01
        .uleb128 0x12, 0x01
        .byte 0, 0
        .uleb128 6, 0x2e                 # DW_TAG_subprogram declaration
        .byte 0
        .uleb128 0x03, 0x08
        .uleb128 0x11, 0x01
        .uleb128 0x12, 0x01
        .uleb128 0x3c, 0x19              # DW_AT_declaration, DW_FORM_flag_present
        .byte 0, 0
        .byte 0

        .section .debug_info,"",@progbits
        .long .Lunit_end - .Lunit_header
.Lunit_header:
        .short 5                         # DWARF version
        .byte 1, 8                       # DW_UT_compile, address size
        .long 0                          # Abbreviation table offset
        .uleb128 1
        .uleb128 2
        .asciz "outer"
        .quad 0x1000
        .long 0x50
        .uleb128 3
        .asciz "inlined"
        .long .Linline_ranges - .Lrange_section
        .uleb128 4
        .asciz "innermost"
        .quad 0x1010
        .long 0x05
        .byte 0, 0                       # End inline and outer children.
        .uleb128 5
        .asciz "crossing"
        .quad 0x1017, 0x1029
        .uleb128 5
        .asciz "removed"
        .quad 0, 0x2000
        .uleb128 5
        .asciz "reversed"
        .quad 0x1060, 0x1050
        .uleb128 6
        .asciz "declared"
        .quad 0x1060, 0x1070
        .uleb128 5
        .asciz "disjoint"
        .quad 0x1080, 0x1084
        .byte 0
.Lunit_end:

        .section .debug_rnglists,"",@progbits
.Lrange_section:
        .long .Lrange_end - .Lrange_header
.Lrange_header:
        .short 5
        .byte 8, 0                       # Address and segment selector sizes
        .long 0                          # No indexed offsets.
.Linline_ranges:
        .byte 0x06                       # DW_RLE_start_end
        .quad 0x1010, 0x1020
        .byte 0x06
        .quad 0x1028, 0x1030
        .byte 0                          # DW_RLE_end_of_list
.Lrange_end:

        .section .note.GNU-stack,"",@progbits
