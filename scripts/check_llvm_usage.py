# Copyright 2026 Brett Wilson. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from pathlib import Path
import re
import sys

LLVM_USE = re.compile(rb'^[ \t]*#[ \t]*include[ \t]*[<"]llvm/|\bllvm\s*::', re.MULTILINE)

root = Path(__file__).resolve().parents[1]
allowlist = Path("scripts/llvm_usage_allowlist.txt")
allowed = {
    line for line in (root / allowlist).read_text().splitlines() if line and not line.startswith("#")
}
actual = set()
for directory in ("src", "third_party"):
    for path in (root / directory).rglob("*"):
        if path.suffix in (".cc", ".h") and path.is_file():
            source = re.sub(rb"//.*", b"", path.read_bytes())
            if LLVM_USE.search(source):
                actual.add(path.relative_to(root).as_posix())

if new := actual - allowed:
    print("Remove the new LLVM use from these files:", *sorted(new), sep="\n  ", file=sys.stderr)
if stale := allowed - actual:
    print(f"Remove these unused paths from {allowlist}:", *sorted(stale), sep="\n  ", file=sys.stderr)
sys.exit(actual != allowed)
