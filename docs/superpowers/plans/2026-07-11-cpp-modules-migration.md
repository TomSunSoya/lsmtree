# C++ Modules Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

*(v2 — rebased onto commit `219f1b1` "Refactor repository structure and enforce formatting": clang-format pre-commit hook, `add_lsmtree_test()` CMake helper, `tests/test_support.h`, self-sufficient header include blocks, `writeAll` already de-`static`-ed, renamed DB members.)*

**Goal:** Convert lsmtree's six components from header/cpp pairs into C++20 named modules (`lsm.*`), then attempt `import std` (C++23) with an explicit stop-loss.

**Architecture:** Top-down conversion (DB → SSTable → Manifest → MemTable → BloomFilter → utils). At every commit each type is defined in exactly one place — either its old header (textually included, entities attached to the global module) or its new module purview — never both, so there are no ODR splits mid-migration. Interface units live in `modules/*.cppm`; existing `src/*.cpp` files become module implementation units (minimal diffs). Phase 2 (`import std`) is gated behind a toolchain check and is allowed to fail without rolling back Phase 1.

**Tech Stack:** CMake 4.3.4 (`FILE_SET CXX_MODULES`), Ninja, Apple clang 21 (primary) / Homebrew LLVM 22.1.7 at `/opt/homebrew/opt/llvm` (fallback + required for `import std`), GTest via `find_package(GTest CONFIG)`, clang-format enforced by `.githooks/pre-commit`.

## Global Constraints

- C++23 (via `target_compile_features(lsmtree_lib PUBLIC cxx_std_23)`); `cmake_minimum_required(VERSION 4.2)` stays.
- Generator must remain **Ninja** (CMake module scanning does not work with Makefiles).
- **Zero behavior change.** This is re-packaging only: no function body may be edited.
- Module names: `lsm.db`, `lsm.sstable`, `lsm.manifest`, `lsm.memtable`, `lsm.bloom`, `lsm.utils`. Interface units in `modules/`, implementation units stay in `src/`.
- Baseline: **111 tests, 100% pass** at HEAD `219f1b1` **plus the currently pending DB.h/DB.cpp cleanup, which must be committed before Task 1** (never start this refactor on a dirty tree). After Task 1 the expected count is **112** (smoke test added); after Task 8 it is **111** again. Every task ends with `100% tests passed`.
- **Formatting:** the pre-commit hook runs `tools/clang-format.sh check`. Run `./tools/clang-format.sh format` before every commit in this plan. Task 1 adds `*.cppm` to the script's pattern list so module files are covered too.
- **Include-list rule (in case the code drifts again before execution):** a module unit's global fragment is the union of (a) the `.cpp` file's current `#include` block and (b) its own header's `#include` block, with project headers listed only until their component becomes a module. The exact lists below are correct as of `219f1b1`; if a file changed since, recompute with the rule rather than trusting the list.
- `tests/test_support.h` contains **only std + gtest** helpers — it names no project types. It stays a plain header, untouched by the entire migration.
- `$BUILD` is the build directory that passed the Task 1 gate: `cmake-build-debug` if Apple clang works, `cmake-build-modules` if the Homebrew fallback was needed. Every `cmake --build` / `ctest` below uses `$BUILD`.
- Commits: imperative one-liners, **no Co-Authored-By trailer**.
- In test files the ordering convention is: all `#include` lines first, then all `import` lines, then code.

## File Structure (end state of Phase 1)

```
modules/
  db.cppm         — export module lsm.db;        exports: DB
  sstable.cppm    — export module lsm.sstable;   exports: SSTable, SSTableIterator
  manifest.cppm   — export module lsm.manifest;  exports: Manifest
  memtable.cppm   — export module lsm.memtable;  exports: MemTable, MemTableIterator
  bloom.cppm      — export module lsm.bloom;     exports: BloomFilter
  utils.cppm      — export module lsm.utils;     exports: Type, Entry, Result, Record, Index,
                     TableMeta, removeFile, writeAll, FdGuard, FileWriter, Iterator, mergeSorted
src/*.cpp         — unchanged filenames, converted to implementation units (`module lsm.x;`)
tests/test_support.h — unchanged plain header (std + gtest only)
inc/              — deleted (Task 7)
```

Dependency edges (import graph, arrow = imports): db → {sstable, manifest, memtable, utils}; sstable → {memtable, bloom, utils}; manifest → utils; memtable → utils; bloom → (nothing).

## Known Risks and Remedies

| Risk | Symptom | Remedy |
|---|---|---|
| Apple clang has no `clang-scan-deps` | CMake configure error mentioning `CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS` | Task 1 remedy ladder (A2 then B) |
| **Forward-declaring a module-owned class** — `inc/SSTable.h:18` has `class MemTable;` | after Task 5, a stray `class MemTable;` outside `lsm.memtable` = entity attached to the wrong module → link/ODR errors | the declaration is dropped in Task 3 (replaced by a textual include) and the include becomes `import lsm.memtable;` in Task 5 — never re-add a forward declaration for any `lsm.*`-owned type |
| GTest binary ABI vs Homebrew libc++ (only if fallback B taken) | link errors in test executables | libc++↔libc++ is normally compatible; if not, `brew install googletest` built against the same libc++, or stop and reassess |
| Incomplete type across module boundary | `error: incomplete type 'Manifest'` in a test TU | the TU must `import` the module that owns the type (every TU imports what it **names**); as a last resort change the owning `import x;` in the interface to `export import x;` |
| Macros never cross `import` | `assert`/`errno` undefined | keep `<cassert>` / `<cerrno>` / `<unistd.h>` / `<fcntl.h>` as textual includes in the global module fragment — Phase 2 explicitly preserves these |
| Pre-commit hook rejects unformatted `.cppm` | commit fails at the hook | run `./tools/clang-format.sh format` before committing (Task 1 adds `*.cppm` to its patterns) |
| `import std` detection refusal (happened before on this machine) | configure error in Task 9 | stop-loss: abandon Phase 2, delete the build dir, Phase 1 state is the shipped state |
| CLion indexing lags behind module support | red code in IDE, green build in terminal | trust the terminal; Tools → CMake → Reset Cache and Reload |

---

### Task 0 (precondition): commit the pending working-tree change

- [ ] `git status` shows `M inc/DB.h`, `M src/DB.cpp` (an include cleanup + a range-for init-statement). Commit it as its own change before anything else:

```bash
./tools/clang-format.sh format && git add inc/DB.h src/DB.cpp && git commit -m "Tidy DB scan locals and includes"
```

(The plan file itself — `docs/superpowers/plans/…` — commit it here too or leave it untracked; either is fine.)

---

### Task 1: Toolchain gate — prove modules build at all

**Files:**
- Create: `modules/smoke.cppm`
- Create: `tests/modules_smoke_tests.cpp`
- Modify: `CMakeLists.txt`, `tools/clang-format.sh`

**Interfaces:**
- Consumes: nothing.
- Produces: the `FILE_SET CXX_MODULES` wiring on `lsmtree_lib` that every later task appends to, and the `$BUILD` decision recorded at the end of this task.

- [ ] **Step 1: Create the smoke module**

`modules/smoke.cppm`:
```cpp
export module lsm.smoke;

export namespace lsm
{
constexpr int smokeAnswer()
{
    return 42;
}
}
```

- [ ] **Step 2: Create the smoke test**

`tests/modules_smoke_tests.cpp`:
```cpp
#include <gtest/gtest.h>

import lsm.smoke;

TEST(ModulesSmokeTest, ImportedFunctionIsUsable)
{
    EXPECT_EQ(42, lsm::smokeAnswer());
}
```

- [ ] **Step 3: Wire modules into CMake and the format script**

In `CMakeLists.txt`, immediately after `target_compile_features(lsmtree_lib PUBLIC cxx_std_23)`, add:

```cmake
target_sources(lsmtree_lib
        PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS modules
        FILES modules/smoke.cppm)
```

With the other `add_lsmtree_test` lines, add:

```cmake
add_lsmtree_test(modules_smoke_tests tests/modules_smoke_tests.cpp)
```

In `tools/clang-format.sh` line 58, extend the patterns array:

```bash
patterns=('*.c' '*.cc' '*.cpp' '*.cppm' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx')
```

- [ ] **Step 4: Run the gate**

Run: `cmake -S . -B cmake-build-debug && cmake --build cmake-build-debug -j`

Three possible outcomes — walk the ladder top to bottom:

- **A (best):** configure + build succeed → `BUILD=cmake-build-debug`. Go to Step 5.
- **A2:** configure fails mentioning `clang-scan-deps`. Retry pointing Apple clang at Homebrew's scanner:
  `cmake -S . -B cmake-build-debug -DCMAKE_CXX_COMPILER_CLANG_SCAN_DEPS=/opt/homebrew/opt/llvm/bin/clang-scan-deps && cmake --build cmake-build-debug -j`
  If it succeeds → `BUILD=cmake-build-debug`. Go to Step 5.
- **B:** A2 also fails (scanner/compiler version mismatch shows up as scan errors). Configure a fresh dir on Homebrew LLVM — do NOT touch cmake-build-debug further:
  ```bash
  cmake -S . -B cmake-build-modules -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
    -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
    -DCMAKE_EXE_LINKER_FLAGS="-L/opt/homebrew/opt/llvm/lib/c++ -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++"
  cmake --build cmake-build-modules -j
  ```
  If it succeeds → `BUILD=cmake-build-modules` for **all** remaining tasks. If B also fails, STOP the whole plan and report the exact error — do not improvise further.

- [ ] **Step 5: Run tests**

Run: `ctest --test-dir $BUILD`
Expected: `100% tests passed, 0 tests failed out of 112`

- [ ] **Step 6: Commit**

```bash
./tools/clang-format.sh
git add modules/smoke.cppm tests/modules_smoke_tests.cpp CMakeLists.txt tools/clang-format.sh
git commit -m "Gate the toolchain for C++ named modules"
```

Record which rung (A / A2 / B) won, as a line in the commit body.

---

### Task 2: Convert DB to `lsm.db`

**Files:**
- Create: `modules/db.cppm`
- Modify: `src/DB.cpp` (top-of-file only), `tests/db_tests.cpp` (include block only), `CMakeLists.txt`
- Delete: `inc/DB.h`

**Interfaces:**
- Consumes: `Manifest.h`, `MemTable.h`, `SSTable.h`, `utils.h` — still headers at this point, textually included.
- Produces: `export module lsm.db;` exporting `class DB` (current public API: constructor, `put`, `get`, `remove`, `flush`, `compact`, `scan`). Later tasks re-point this module's preamble at `lsm.sstable` / `lsm.manifest` / `lsm.memtable` / `lsm.utils` as those appear.

- [ ] **Step 1: Write the interface unit**

`modules/db.cppm` — the global fragment is `inc/DB.h`'s current include block; the class body is moved **verbatim** from `inc/DB.h` (member names have drifted before — `activeMemTable_`, `manifest_`, `dataDirectory_` — so always paste from the file, never retype):

```cpp
module;

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Manifest.h"
#include "MemTable.h"

export module lsm.db;

export class DB
{
    // ← paste the DB class body from inc/DB.h, unchanged
};
```

- [ ] **Step 2: Convert `src/DB.cpp` into an implementation unit**

Replace its current top (`#include "DB.h"`, the std block, `#include "SSTable.h"`) with exactly (rule: own includes ∪ own header's includes):

```cpp
module;

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "Manifest.h"
#include "MemTable.h"
#include "SSTable.h"

module lsm.db;
```

Everything below (anonymous namespace, `selectScanTables`, all member definitions) stays byte-identical.

- [ ] **Step 3: Update the test TU**

In `tests/db_tests.cpp`: delete `#include "DB.h"`; keep `#include "Manifest.h"`, `#include "SSTable.h"`, `#include "test_support.h"`; after the include block add:

```cpp
import lsm.db;
```

(`Record`/`Type` stay visible through `Manifest.h → utils.h` — no extra include needed yet.)

- [ ] **Step 4: Update CMake and delete the header**

`CMakeLists.txt` — extend the file set (`add_library` lists no headers, nothing to remove there):

```cmake
        FILES modules/smoke.cppm modules/db.cppm)
```

Then: `git rm inc/DB.h`

- [ ] **Step 5: Build and test**

Run: `cmake --build $BUILD -j` → clean.
Run: `ctest --test-dir $BUILD` → `100% tests passed, 0 tests failed out of 112`

- [ ] **Step 6: Commit**

```bash
./tools/clang-format.sh format && git add -A
git commit -m "Move DB into the lsm.db named module"
```

---

### Task 3: Convert SSTable to `lsm.sstable`

**Files:**
- Create: `modules/sstable.cppm`
- Modify: `src/SSTable.cpp` (top only), `src/DB.cpp` (top only), `tests/db_tests.cpp`, `tests/sstable_tests.cpp`, `CMakeLists.txt`
- Delete: `inc/SSTable.h`

**Interfaces:**
- Consumes: `BloomFilter.h`, `MemTable.h`, `utils.h` (still headers).
- Produces: `export module lsm.sstable;` exporting `class SSTable` (public: `build`, `cleanupOrphanedTemps`, `addRecordToFile`, constructor, `get`) and `class SSTableIterator : public Iterator`.

- [ ] **Step 1: Write the interface unit**

`modules/sstable.cppm`. **Critical detail:** `inc/SSTable.h:18` currently has a forward declaration `class MemTable;`. Do NOT carry it into the module purview (a purview declaration would attach `MemTable` to `lsm.sstable` — wrong module, ODR bomb at Task 5). Replace it with a textual include of `MemTable.h` in the global fragment:

```cpp
module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BloomFilter.h"
#include "MemTable.h"
#include "utils.h"

export module lsm.sstable;

export class SSTable
{
    // ← paste the SSTable class body from inc/SSTable.h, unchanged
    //   (kBlockSize, the friend declaration, all private statics come along;
    //    the `class MemTable;` forward declaration is DELETED, not pasted)
};

export class SSTableIterator : public Iterator
{
    // ← paste the SSTableIterator class body from inc/SSTable.h, unchanged
};
```

- [ ] **Step 2: Convert `src/SSTable.cpp`**

Replace its top with:

```cpp
module;

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BloomFilter.h"
#include "MemTable.h"
#include "utils.h"

module lsm.sstable;
```

Body unchanged.

- [ ] **Step 3: Re-point consumers**

`src/DB.cpp`: remove `#include "SSTable.h"` from the global fragment; immediately after `module lsm.db;` add:

```cpp
import lsm.sstable;
```

`tests/db_tests.cpp`: replace `#include "SSTable.h"` with `import lsm.sstable;` (grouped with the existing import).
`tests/sstable_tests.cpp`: replace `#include "SSTable.h"` with `import lsm.sstable;`; keep `#include "MemTable.h"` and `#include "test_support.h"` (`Record`/`Type` stay visible through `MemTable.h → utils.h`).

- [ ] **Step 4: CMake + delete header**

File set gains `modules/sstable.cppm`. `git rm inc/SSTable.h`

- [ ] **Step 5: Build and test**

`cmake --build $BUILD -j` → clean. `ctest --test-dir $BUILD` → `112 passed`.

- [ ] **Step 6: Commit**

```bash
./tools/clang-format.sh format && git add -A
git commit -m "Move SSTable into the lsm.sstable named module"
```

---

### Task 4: Convert Manifest to `lsm.manifest`

**Files:**
- Create: `modules/manifest.cppm`
- Modify: `src/Manifest.cpp` (top only), `modules/db.cppm`, `src/DB.cpp`, `tests/db_tests.cpp`, `tests/manifest_tests.cpp`, `CMakeLists.txt`
- Delete: `inc/Manifest.h`

**Interfaces:**
- Consumes: `utils.h` (still a header).
- Produces: `export module lsm.manifest;` exporting `class Manifest` (public: `nextNumber`, `allocateNumber`, `addTable`, `replaceTables`, `save`, `logNumber`, `setLogNumber`, `level`, `levelCount`, `allTableNumbers`, `getTableMeta`).

- [ ] **Step 1: Write the interface unit**

`modules/manifest.cppm`:

```cpp
module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include "utils.h"

export module lsm.manifest;

export class Manifest
{
    // ← paste the Manifest class body from inc/Manifest.h, unchanged
};
```

- [ ] **Step 2: Convert `src/Manifest.cpp`**

Replace its top with:

```cpp
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "utils.h"

module lsm.manifest;
```

- [ ] **Step 3: Re-point consumers**

`modules/db.cppm`: remove `#include "Manifest.h"` from the global fragment; after `export module lsm.db;` add `import lsm.manifest;`.
`src/DB.cpp`: remove `#include "Manifest.h"`; the import list after `module lsm.db;` becomes:

```cpp
import lsm.manifest;
import lsm.sstable;
```

`tests/db_tests.cpp`: replace `#include "Manifest.h"` with `import lsm.manifest;` **and add `#include "utils.h"` to the include block** — this was the test's last textual path to `Record`/`Type`/`TableMeta`.
`tests/manifest_tests.cpp`: replace `#include "Manifest.h"` with `import lsm.manifest;` and add `#include "utils.h"` (the test names `TableMeta`).

- [ ] **Step 4: CMake + delete header**

File set gains `modules/manifest.cppm`. `git rm inc/Manifest.h`

- [ ] **Step 5: Build and test**

`cmake --build $BUILD -j` → clean. `ctest --test-dir $BUILD` → `112 passed`.

- [ ] **Step 6: Commit**

```bash
./tools/clang-format.sh format && git add -A
git commit -m "Move Manifest into the lsm.manifest named module"
```

---

### Task 5: Convert MemTable to `lsm.memtable`

**Files:**
- Create: `modules/memtable.cppm`
- Modify: `src/MemTable.cpp` (top only), `modules/db.cppm`, `modules/sstable.cppm`, `src/DB.cpp`, `src/SSTable.cpp`, `tests/memtable_tests.cpp`, `tests/sstable_tests.cpp`, `CMakeLists.txt`
- Delete: `inc/MemTable.h`

**Interfaces:**
- Consumes: `utils.h` (still a header).
- Produces: `export module lsm.memtable;` exporting `class MemTable` (public: `put`, `get`, `remove`, `size`, `size_bytes`, `begin`, `end`, `const_iterator`) and `class MemTableIterator : public Iterator`.

- [ ] **Step 1: Write the interface unit**

`modules/memtable.cppm`:

```cpp
module;

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils.h"

export module lsm.memtable;

export class MemTable
{
    // ← paste the MemTable class body from inc/MemTable.h, unchanged
    //   (including the nested WALFileWriter and the friend declaration)
};

export class MemTableIterator : public Iterator
{
    // ← paste the MemTableIterator class body from inc/MemTable.h, unchanged
};
```

- [ ] **Step 2: Convert `src/MemTable.cpp`**

Replace its top with:

```cpp
module;

#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "utils.h"

module lsm.memtable;
```

- [ ] **Step 3: Re-point consumers — watch the two traps**

`modules/sstable.cppm`: remove `#include "MemTable.h"`; after `export module lsm.sstable;` add `import lsm.memtable;`. (**Never** replace it with a `class MemTable;` forward declaration — see risk table.)
`modules/db.cppm`: remove `#include "MemTable.h"`; add `import lsm.memtable;` **and add `#include "utils.h"` to its global fragment** — `MemTable.h` was this interface's last textual path to `Record` (named in `scan`'s return type).
`src/DB.cpp`: remove `#include "MemTable.h"`; add `import lsm.memtable;` **and add `#include "utils.h"` to its global fragment** (same reason: it names `Record`, `Iterator`, `mergeSorted`).
`src/SSTable.cpp`: remove `#include "MemTable.h"`; add `import lsm.memtable;` (it already includes `utils.h`).
`tests/memtable_tests.cpp`: replace `#include "MemTable.h"` with `import lsm.memtable;` and add `#include "utils.h"` (test names `Type`/`Record`).
`tests/sstable_tests.cpp`: replace `#include "MemTable.h"` with `import lsm.memtable;` and add `#include "utils.h"`.

- [ ] **Step 4: CMake + delete header**

File set gains `modules/memtable.cppm`. `git rm inc/MemTable.h`

- [ ] **Step 5: Build and test**

`cmake --build $BUILD -j` → clean. `ctest --test-dir $BUILD` → `112 passed`.

- [ ] **Step 6: Commit**

```bash
./tools/clang-format.sh format && git add -A
git commit -m "Move MemTable into the lsm.memtable named module"
```

---

### Task 6: Convert BloomFilter to `lsm.bloom`

**Files:**
- Create: `modules/bloom.cppm`
- Modify: `src/BloomFilter.cpp` (top only), `modules/sstable.cppm`, `src/SSTable.cpp`, `tests/bloom_filter_tests.cpp`, `CMakeLists.txt`
- Delete: `inc/BloomFilter.h`

**Interfaces:**
- Consumes: nothing project-local (std only).
- Produces: `export module lsm.bloom;` exporting `class BloomFilter` (`Serialize`, `fromBytes`, both constructors, `add`, `mightContain`).

- [ ] **Step 1: Write the interface unit**

`modules/bloom.cppm` (the header's include block is already complete — the refactor fixed the old leeching):

```cpp
module;

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

export module lsm.bloom;

export class BloomFilter
{
    // ← paste the BloomFilter class body from inc/BloomFilter.h, unchanged
};
```

- [ ] **Step 2: Convert `src/BloomFilter.cpp`**

Replace its top with:

```cpp
module;

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

module lsm.bloom;
```

- [ ] **Step 3: Re-point consumers**

`modules/sstable.cppm`: remove `#include "BloomFilter.h"`; add `import lsm.bloom;` to its import list.
`src/SSTable.cpp`: remove `#include "BloomFilter.h"`; add `import lsm.bloom;`.
`tests/bloom_filter_tests.cpp`: replace `#include "BloomFilter.h"` with `import lsm.bloom;` (placed after the includes).

- [ ] **Step 4: CMake + delete header**

File set gains `modules/bloom.cppm`. `git rm inc/BloomFilter.h`

- [ ] **Step 5: Build and test**

`cmake --build $BUILD -j` → clean. `ctest --test-dir $BUILD` → `112 passed`.

- [ ] **Step 6: Commit**

```bash
./tools/clang-format.sh format && git add -A
git commit -m "Move BloomFilter into the lsm.bloom named module"
```

---

### Task 7: Convert utils to `lsm.utils` (last, biggest export surface)

**Files:**
- Create: `modules/utils.cppm`
- Modify: `src/utils.cpp` (top only), `modules/db.cppm`, `modules/sstable.cppm`, `modules/manifest.cppm`, `modules/memtable.cppm`, `src/DB.cpp`, `src/SSTable.cpp`, `src/MemTable.cpp`, `src/Manifest.cpp`, `tests/utils_tests.cpp`, `tests/db_tests.cpp`, `tests/sstable_tests.cpp`, `tests/memtable_tests.cpp`, `tests/manifest_tests.cpp`, `CMakeLists.txt`
- Delete: `inc/utils.h`, then the now-empty `inc/` directory

**Interfaces:**
- Consumes: std + POSIX only.
- Produces: `export module lsm.utils;` exporting every top-level entity of the current header: `Type`, `Entry`, `Result`, `Record`, `Index`, `TableMeta`, `removeFile`, `writeAll`, `FdGuard`, `FileWriter`, `Iterator`, `mergeSorted`. (`writeAll` is already a plain declaration — the old `static` definition was removed in `219f1b1`, so no signature edit is needed; the paste is fully verbatim.)

- [ ] **Step 1: Write the interface unit**

`modules/utils.cppm`:

```cpp
module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module lsm.utils;

// Paste every entity from inc/utils.h unchanged, prefixing each top-level
// declaration (enum, struct, class, function declaration) with `export`.
// No other edit. <cstdint> is added above because the header currently
// leeches uint8_t/uint32_t/uint64_t through its other includes.
```

- [ ] **Step 2: Convert `src/utils.cpp`**

Replace its top with:

```cpp
module;

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

module lsm.utils;
```

- [ ] **Step 3: Re-point every consumer**

In each of `modules/db.cppm`, `modules/sstable.cppm`, `modules/manifest.cppm`, `modules/memtable.cppm`: remove `#include "utils.h"` from the global fragment and add `import lsm.utils;` as the **first** import after the `export module` line. (`modules/bloom.cppm` never used it.)

In each of `src/DB.cpp`, `src/SSTable.cpp`, `src/MemTable.cpp`, `src/Manifest.cpp`: remove `#include "utils.h"`; add `import lsm.utils;` after the `module lsm.x;` line.

In each of `tests/utils_tests.cpp`, `tests/db_tests.cpp`, `tests/sstable_tests.cpp`, `tests/memtable_tests.cpp`, `tests/manifest_tests.cpp`: replace `#include "utils.h"` with `import lsm.utils;` (grouped with the other imports). `#include "test_support.h"` stays in every test exactly as it is.

- [ ] **Step 4: CMake + delete header + delete inc/**

`CMakeLists.txt`: file set gains `modules/utils.cppm`; **delete** the line `target_include_directories(lsmtree_lib PUBLIC inc)` (nothing includes project headers anymore — `test_support.h` is found relative to the test files).
`git rm inc/utils.h` — `inc/` is now empty and disappears from git automatically.

- [ ] **Step 5: Verify no project header includes remain**

Run: `grep -rn '#include "' modules/ src/ tests/ | grep -v test_support.h`
Expected: **no output**.

- [ ] **Step 6: Build and test**

`cmake --build $BUILD -j` → clean. `ctest --test-dir $BUILD` → `112 passed`.

- [ ] **Step 7: Commit**

```bash
./tools/clang-format.sh format && git add -A
git commit -m "Move shared utilities into the lsm.utils named module"
```

---

### Task 8: Remove the smoke scaffolding — Phase 1 done

**Files:**
- Delete: `modules/smoke.cppm`, `tests/modules_smoke_tests.cpp`
- Modify: `CMakeLists.txt`, `AGENTS.md`

- [ ] **Step 1: Delete smoke files and their CMake entries**

`git rm modules/smoke.cppm tests/modules_smoke_tests.cpp`; in `CMakeLists.txt` remove `modules/smoke.cppm` from the file set and remove the `add_lsmtree_test(modules_smoke_tests …)` line.

- [ ] **Step 2: Document the build requirement**

In `AGENTS.md`, add one short paragraph: the project uses C++20 named modules; building requires CMake ≥ 3.28 with the Ninja generator; state which compiler the Task 1 gate selected (and the exact fallback configure command if rung B won).

- [ ] **Step 3: Build and test**

`cmake --build $BUILD -j` → clean. `ctest --test-dir $BUILD` → `100% tests passed, 0 tests failed out of 111`.

- [ ] **Step 4: Commit**

```bash
./tools/clang-format.sh format && git add -A
git commit -m "Finish the named-module migration"
```

---

### Task 9: Phase 2 gate — attempt `import std` (allowed to fail)

**Files:**
- None modified up front — this task is configure-only until the gate passes.

**Interfaces:**
- Consumes: Phase 1 end state.
- Produces: either a working `cmake-build-std` build directory with `import std` available, or a documented STOP.

**Context:** a previous attempt on this machine failed at CMake's `import std` toolchain detection even though `/opt/homebrew/opt/llvm/share/libc++/v1/std.cppm` exists. Treat this task as an experiment with a hard stop-loss: **if Step 3 fails after both configure variants, abandon Phase 2 entirely** — delete `cmake-build-std`, ship Phase 1, revisit after the next CMake/LLVM upgrade.

- [ ] **Step 1: Extract this CMake version's experimental UUID**

`import std` support is gated behind a per-version experimental UUID. Read it from the local install:

Run: `find "$(dirname "$(dirname "$(command -v cmake)")")/share" -name experimental.rst 2>/dev/null | xargs grep -A3 CxxImportStd | grep -m1 -oE '[0-9a-f-]{36}'`

Expected: one UUID string (for CMake 4.3.x it looked like `451f2fe2-…`). Call it `$UUID`.

- [ ] **Step 2: Configure a fresh build dir on Homebrew LLVM (mandatory — Apple's toolchain has no std.cppm)**

```bash
cmake -S . -B cmake-build-std -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_EXE_LINKER_FLAGS="-L/opt/homebrew/opt/llvm/lib/c++ -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++" \
  -DCMAKE_EXPERIMENTAL_CXX_IMPORT_STD="$UUID" \
  -DCMAKE_CXX_MODULE_STD=ON
```

Expected: configure completes without an error mentioning `import std` support.

- [ ] **Step 3: Gate check — build and test the untouched Phase 1 code in this dir**

Run: `cmake --build cmake-build-std -j && ctest --test-dir cmake-build-std`
Expected: `100% tests passed, 0 tests failed out of 111`.

- **PASS** → continue to Task 10; use `cmake-build-std` for all Task 10 verification.
- **FAIL at configure** (detection refusal, like last time) or **FAIL at link** (GTest ABI against Homebrew libc++, see risk table) → STOP: `rm -rf cmake-build-std`, no source commit, Phase 2 is over. Write one line in `AGENTS.md` ("import std attempted <date>, blocked by <exact error>, retry after toolchain upgrade") and commit that note as `git commit -m "Record the import std attempt outcome"`.

---

### Task 10: Swap std includes for `import std`, one module per step

**Files:**
- Modify: all six `modules/*.cppm` and all six `src/*.cpp` implementation units.

**Interfaces:**
- Consumes: the `cmake-build-std` gate from Task 9.
- Produces: module units whose global fragments contain **only** macro/POSIX headers; everything std comes from `import std;`.

**The rule for every module unit (interface and implementation alike):**
1. In the global fragment, delete every `#include <...>` of a **pure std header** (`<vector>`, `<string>`, `<filesystem>`, `<format>`, `<optional>`, `<map>`, `<fstream>`, `<queue>`, `<cmath>`, `<cstring>`, `<cstdio>`, `<functional>`, `<algorithm>`, `<ranges>`, …).
2. **Keep** these textual includes — they define macros or POSIX names that `import std` can never provide: `<cassert>` (assert), `<cerrno>` (errno), `<unistd.h>`, `<fcntl.h>`.
3. Add `import std;` as the last line of the import list (after `export module lsm.x;` / `module lsm.x;` and any `import lsm.*;` lines).
4. Test TUs (`tests/*.cpp`) and `tests/test_support.h` are **not touched** — they keep ordinary includes; GTest headers and `import std` don't need to meet.
5. If the fragment ends up with no includes at all, delete the `module;` line too — the file then starts directly at its `export module` / `module` declaration.

Apply bottom-up, one module per commit:

- [ ] **Step 1: `lsm.utils`** — interface fragment empties entirely; impl keeps `<cerrno>`, `<fcntl.h>`, `<unistd.h>`. Build + ctest (111 pass). `./tools/clang-format.sh format && git commit -am "Import std in lsm.utils"`
- [ ] **Step 2: `lsm.bloom`** — both fragments empty entirely. Build + ctest. `./tools/clang-format.sh format && git commit -am "Import std in lsm.bloom"`
- [ ] **Step 3: `lsm.memtable`** — interface fragment empties; impl keeps `<cassert>`, `<cerrno>`, `<fcntl.h>`, `<unistd.h>`. Build + ctest. `./tools/clang-format.sh format && git commit -am "Import std in lsm.memtable"`
- [ ] **Step 4: `lsm.manifest`** — both fragments empty. Build + ctest. `./tools/clang-format.sh format && git commit -am "Import std in lsm.manifest"`
- [ ] **Step 5: `lsm.sstable`** — interface fragment empties; impl keeps `<cassert>`. Build + ctest. `./tools/clang-format.sh format && git commit -am "Import std in lsm.sstable"`
- [ ] **Step 6: `lsm.db`** — both fragments empty (current DB.cpp uses no macro/POSIX headers). Build + ctest. `./tools/clang-format.sh format && git commit -am "Import std in lsm.db"`

If any single module trips a libc++ include-vs-import conflict that resists a 15-minute fix, leave **that module** on includes (revert that step only) and continue — mixed state is legal and fine.

- [ ] **Step 7: Final sweep**

Run: `grep -rn '#include <' modules/ src/ | grep -vE "cassert|cerrno|unistd|fcntl"`
Expected: no output.
Run: `ctest --test-dir cmake-build-std` → `111 passed`.
Update `AGENTS.md`: the canonical build now uses the `cmake-build-std` configure line from Task 9 Step 2 — paste it verbatim there.

```bash
./tools/clang-format.sh format && git add -A
git commit -m "Document the import std build configuration"
```

---

## Self-Review Notes

- **Spec coverage:** "先模块化我自己写的代码" → Tasks 1–8; "然后将标准库也引入模块化" → Tasks 9–10 with the stop-loss the earlier failed attempt on this machine demands. ✓
- **Rebased onto `219f1b1`:** all include lists re-derived from the post-refactor files; `add_lsmtree_test()` used for the smoke test; formatting hook accounted for in every commit step; `writeAll`'s old `static` problem is gone (the refactor already made it a declaration); DB member renames make the paste-marker approach mandatory (never retype bodies).
- **New trap found and handled:** `inc/SSTable.h:18` `class MemTable;` — dropped in Task 3, `import lsm.memtable;` in Task 5, risk-table entry added. Textual-visibility handoffs (who loses `utils.h` transitively when) were re-traced per task: `db.cppm`/`src/DB.cpp` gain an explicit `#include "utils.h"` in Task 5; `tests/db_tests.cpp` gains it in Task 4.
- **Type consistency:** module names and `$BUILD`/`$UUID` conventions are used identically throughout. ✓
- **Verbatim-move markers** ("paste the class body … unchanged") are deliberate: the code drifts (this is the second rebase of this plan); the repo file at execution time is the source of truth, and every marker names its exact source file.
