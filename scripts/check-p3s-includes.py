#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Check the explicit, translation-unit-local P3S opt-in boundary."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
paths = subprocess.check_output(
    ["git", "ls-files", "--cached", "--others", "--exclude-standard",
     "*.c", "*.h"], cwd=root, text=True).splitlines()
include = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
errors = []
count = 0
for name in sorted(set(paths)):
    source = (root / name).read_text(errors="replace")
    if "p3s_user_pages.h" not in source:
        continue
    includes = list(include.finditer(source))
    selected = [m for m in includes if m[1] == "linux/p3s_user_pages.h"]
    if not selected:
        continue
    count += 1
    if not name.endswith(".c") or len(selected) != 1:
        errors.append(f"{name}: opt in exactly once, only from a C file")
        continue
    for other in includes:
        # These are X-macro initializer data, not helper headers.
        generated_table = re.fullmatch(r"asm/syscall_table_(32|64)\.h", other[1])
        if other.start() > selected[0].start() and not generated_table:
            errors.append(f"{name}: ordinary include after override: {other[1]}")
for error in errors:
    print(error, file=sys.stderr)
print(f"P3S include boundary: {count} C files, {len(errors)} errors")
sys.exit(bool(errors))
