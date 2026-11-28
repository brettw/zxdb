// Copyright 2026 Brett Wilson. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

int split_value;

extern "C" int split_function(int x) {
  if (x > 0)
    return x + split_value;
  return x - split_value;
}
