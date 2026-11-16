#!/bin/sh
# The Vita app's vault core (src/vault.c, src/lvcrypt.c) on this PC, against the golden vaults.
set -e
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/lv-vita-core-test"
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -o "$out" tests/core_test.c src/vault.c src/lvcrypt.c -lm
"$out" ../src-tauri/tests/fixtures
