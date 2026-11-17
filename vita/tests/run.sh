#!/bin/sh
# The Vita app's portable parts on this PC: the vault core (src/vault.c, src/lvcrypt.c) against the
# golden vaults, and the Browser's site readers (src/booru.c, src/json.c) against saved answers.
set -e
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/lv-vita-test"
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -o "$out-core" tests/core_test.c src/vault.c src/lvcrypt.c -lm
"$out-core" ../src-tauri/tests/fixtures
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -o "$out-booru" tests/booru_test.c src/booru.c src/json.c
"$out-booru"
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -o "$out-manga" tests/manga_test.c src/manga.c src/json.c
"$out-manga"
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -o "$out-discover" tests/discover_test.c src/discover.c src/json.c
"$out-discover"
