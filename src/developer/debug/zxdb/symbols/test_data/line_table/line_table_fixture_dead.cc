// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Nothing references this function and it isn't exported. Builds that use --gc-sections remove
// its code, so the only line table rows for this file are in a stripped sequence.
__attribute__((visibility("hidden"))) int LineTableFixtureDead(int x) {
  return x * x + 7;
}
