// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The asm statements keep y on the x87 stack, so the debug info gives y a register location.
__attribute__((noinline)) long double Scale(long double x) {
  long double y = x * 3.0L;
  __asm__ volatile("nop" : "+t"(y));
  __asm__ volatile("nop" : "+t"(y));  // Breakpoint line.
  return y + 1.0L;
}

// The exit code is 8 unless the debugger changes y.
int main(void) { return (int)Scale(2.5L); }
