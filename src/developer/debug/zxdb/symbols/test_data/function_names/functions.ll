; Copyright 2026 The Fuchsia Authors. All rights reserved.
; Use of this source code is governed by a BSD-style license that can be
; found in the LICENSE file.

; These functions model independent ways a producer can describe a function.
source_filename = "functions.cc"

; GCC -g1 can emit the short name and linkage name at compilation-unit scope.
define void @_ZN1a1b1fEi(i32 %arg) !dbg !5 {
  ret void, !dbg !8
}

; The ELF label deliberately differs from the linkage name in DWARF.
define void @second(double %arg) !dbg !6 {
  ret void, !dbg !9
}

; The whole qualified name occupies one DW_AT_name.
define void @third() !dbg !7 {
  ret void, !dbg !10
}

; A Rust v0 function and a C function with only ELF symbols.
define void @_RNvNtCsa11GDJ28RlQ_1a1b4rust() { ret void }
define void @plain() { ret void }

; Linkage spellings used by the breakpoint parser when DWARF is absent.
define void @_RINvNtCsa11GDJ28RlQ_1a1b7genericlE() { ret void }
define void @_RNvMC1rNtB2_1S3new() { ret void }
define void @_ZN3foo6taggedB5cxx11Ev() { ret void }
define void @_ZN12_GLOBAL__N_16hiddenEv() { ret void }
define void @_Z11specializedIiEvv() { ret void }
define void @_Z5clonev.cold() { ret void }
define void @_ZThn8_N1A9thunkOnlyEv() { ret void }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!11, !12}
!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !1, producer: "zxdb test", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "functions.cc", directory: "/src")
!2 = !DISubroutineType(types: !3)
!3 = !{null}
!5 = distinct !DISubprogram(name: "f", linkageName: "_ZN1a1b1fEi", scope: !1, file: !1, line: 1, type: !2, spFlags: DISPFlagDefinition, unit: !0)
!6 = distinct !DISubprogram(name: "", linkageName: "_ZN1a1b1fEd", scope: !1, file: !1, line: 2, type: !2, spFlags: DISPFlagDefinition, unit: !0)
!7 = distinct !DISubprogram(name: "a::Box<b::T>::f", scope: !1, file: !1, line: 3, type: !2, spFlags: DISPFlagDefinition, unit: !0)
!8 = !DILocation(line: 1, column: 1, scope: !5)
!9 = !DILocation(line: 2, column: 1, scope: !6)
!10 = !DILocation(line: 3, column: 1, scope: !7)
!11 = !{i32 2, !"Dwarf Version", i32 5}
!12 = !{i32 2, !"Debug Info Version", i32 3}
