# Repository Readability and Formatting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the entire repository easier to read and maintain without changing observable behavior, then enforce one CLion-compatible `clang-format` workflow before every commit.

**Architecture:** Preserve the six existing storage components and all public APIs. Apply structural refactors within each component, centralize test-only infrastructure, and use a versioned shell/CMake/Git-hook formatting pipeline driven by the root `.clang-format` file.

**Tech Stack:** C++23, CMake 4.2+, Ninja, GTest, clang-format 22.1.8, POSIX shell, Git hooks.

## Global Constraints

- Baseline commit is `9368aee`; Debug build and all 111 tests pass.
- Public APIs, binary formats, source priority, range boundaries, tombstone behavior, exception types, and filesystem publication order must not change.
- Preserve all existing GTest suite/test names and six test executable target names.
- Use only structural transformations: function extraction, private renaming, dead-code/include cleanup, and duplication removal.
- Run focused tests after each component and the full Debug and ASan suites before the final commit.
- Make one final task commit after formatting and verification; do not create intermediate implementation commits.
- Do not stage `docs/superpowers/plans/2026-07-11-cpp-modules-migration.md`; it is unrelated user work.

---

### Task 1: Formatting infrastructure and commit gate

**Files:**
- Create: `.clang-format`
- Create: `.clang-format-version`
- Create: `.editorconfig`
- Create: `tools/clang-format.sh`
- Create: `cmake/Format.cmake`
- Create: `.githooks/pre-commit`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `/opt/homebrew/opt/llvm/bin/clang-format` version 22.1.8.
- Produces: `tools/clang-format.sh {format|check}`, CMake targets `format` and `format-check`, and a versioned pre-commit hook.

- [ ] **Step 1: Add the canonical style files**

Use LLVM as the base, then set `IndentWidth: 4`, `BreakBeforeBraces: Allman`, `ColumnLimit: 120`, `PointerAlignment: Left`, `ReferenceAlignment: Left`, `SortIncludes: CaseSensitive`, and disable single-line control statements. Record `22.1.8` in `.clang-format-version`. Add `.editorconfig` rules for UTF-8, LF, final newline, and trailing-whitespace removal.

- [ ] **Step 2: Add the formatter wrapper**

`tools/clang-format.sh` must:

```text
usage: tools/clang-format.sh format|check
formatter: ${CLANG_FORMAT:-/opt/homebrew/opt/llvm/bin/clang-format}, then PATH fallback
inputs: git ls-files --cached --others --exclude-standard for *.c/*.cc/*.cpp/*.cxx/*.h/*.hh/*.hpp/*.hxx
format: --style=file -i
check: --style=file --dry-run --Werror
version gate: output contains the exact contents of .clang-format-version
```

- [ ] **Step 3: Add CMake targets and the Git hook**

`cmake/Format.cmake` finds clang-format and delegates to the wrapper. `.githooks/pre-commit` runs `tools/clang-format.sh check` from the repository root and exits nonzero on any mismatch; it must never rewrite or stage files.

- [ ] **Step 4: Enable the hook locally and verify discovery**

Run:

```bash
chmod +x tools/clang-format.sh .githooks/pre-commit
git config --local core.hooksPath .githooks
git config --local --get core.hooksPath
```

Expected output: `.githooks`.

### Task 2: CMake and shared test support

**Files:**
- Modify: `CMakeLists.txt`
- Create: `tests/test_support.h`
- Modify: `tests/db_tests.cpp`
- Modify: `tests/manifest_tests.cpp`
- Modify: `tests/memtable_tests.cpp`
- Modify: `tests/sstable_tests.cpp`

**Interfaces:**
- Consumes: existing six test source files and target names.
- Produces: `test_support::ScopedPathCleanup`, `test_support::readFile`, `test_support::writeFile`, and `test_support::expectFileContent`.

- [ ] **Step 1: Simplify CMake without changing targets**

Set `project(lsmtree LANGUAGES CXX)`, use `target_compile_features(lsmtree_lib PUBLIC cxx_std_23)`, list implementation files consistently, and introduce `add_lsmtree_test(target source)` to perform `add_executable`, `target_link_libraries`, and `gtest_discover_tests` once. Keep target names `memtable_tests`, `sstable_tests`, `manifest_tests`, `db_tests`, `bloom_filter_tests`, and `utils_tests`.

- [ ] **Step 2: Add shared test support**

The header exposes these exact test-only interfaces:

```cpp
namespace test_support
{
class ScopedPathCleanup
{
public:
    explicit ScopedPathCleanup(std::filesystem::path path);
    ScopedPathCleanup(std::initializer_list<std::filesystem::path> paths);
    ~ScopedPathCleanup();
};

void readFile(const std::filesystem::path& path, std::string& content);
void writeFile(const std::filesystem::path& path, std::string_view content);
void expectFileContent(const std::filesystem::path& path, std::string_view expected);
}
```

Implement all functions inline, remove paths in both constructor and destructor, and use `std::error_code` in destructors.

- [ ] **Step 3: Replace duplicated test helpers and manual cleanup**

Use the shared helper in the four test files. Keep DB/MemTable/SSTable-specific `expectGet` and `expectMissing` local. Replace start/end `remove_all` pairs with a scoped guard declared immediately after each test path.

- [ ] **Step 4: Verify tests and target names**

Run:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --parallel 11
ctest --test-dir cmake-build-debug --output-on-failure --parallel 11
```

Expected: 111/111 tests pass and all six executable targets still build by name.

### Task 3: Headers and shared utilities

**Files:**
- Modify: `inc/BloomFilter.h`
- Modify: `inc/DB.h`
- Modify: `inc/Manifest.h`
- Modify: `inc/MemTable.h`
- Modify: `inc/SSTable.h`
- Modify: `inc/utils.h`
- Modify: `src/utils.cpp`
- Test: `tests/utils_tests.cpp`

**Interfaces:**
- Consumes: all existing public declarations.
- Produces: the same public declarations with consistent ownership, naming, and include boundaries.

- [ ] **Step 1: Normalize header structure**

Use `#pragma once` consistently, include every directly used standard header, remove transitive-only includes, order public methods before private state, and preserve every public signature and default argument.

- [ ] **Step 2: Clarify shared utility ownership**

Rename `FileWriter` private fields with trailing underscores, name the serialized record-header constant, move non-template free-function implementations out of the header, and remove redundant local aliases. Do not alter byte widths, write order, fsync, rename, or directory fsync behavior.

- [ ] **Step 3: Verify shared utility behavior**

Run:

```bash
cmake --build cmake-build-debug --target utils_tests --parallel 11
ctest --test-dir cmake-build-debug --output-on-failure -R '^MergeSortedTest\.'
```

Expected: all MergeSorted tests pass.

### Task 4: BloomFilter and Manifest structure

**Files:**
- Modify: `src/BloomFilter.cpp`
- Modify: `inc/BloomFilter.h`
- Modify: `src/Manifest.cpp`
- Modify: `inc/Manifest.h`
- Test: `tests/bloom_filter_tests.cpp`
- Test: `tests/manifest_tests.cpp`

**Interfaces:**
- Consumes: current BloomFilter serialization and Manifest binary layout.
- Produces: named constants and focused read/write helpers with identical bytes and errors.

- [ ] **Step 1: Refactor BloomFilter naming**

Rename private fields and local variables to express bit count, hash count, and word storage. Extract constants for the serialized envelope widths. Preserve hash computation, bit positions, false-positive probability, and serialized bytes.

- [ ] **Step 2: Refactor Manifest binary IO**

Extract translation-unit helpers for typed reads, length-prefixed strings, and range overlap. Rename private `levels` to `levels_` for consistency. Preserve every read/write field order, width, exception type/message, L0 order, and higher-level overlap validation.

- [ ] **Step 3: Verify component suites**

Run:

```bash
cmake --build cmake-build-debug --target bloom_filter_tests manifest_tests --parallel 11
ctest --test-dir cmake-build-debug --output-on-failure -R '^(BloomFilterTest|ManifestTest)\.'
```

Expected: all BloomFilterTest and ManifestTest cases pass.

### Task 5: MemTable and WAL parser structure

**Files:**
- Modify: `inc/MemTable.h`
- Modify: `src/MemTable.cpp`
- Test: `tests/memtable_tests.cpp`

**Interfaces:**
- Consumes: current WAL text format and MemTable API.
- Produces: the same parser and writer behavior with cohesive names and helpers.

- [ ] **Step 1: Normalize private state names**

Rename private members to `table_`, `logPath_`, `walWriter_`, and `currentSizeBytes_`; rename WAL writer fields similarly. Update all uses without changing types or lifetime.

- [ ] **Step 2: Extract WAL parsing helpers**

Move the existing `readLength`, `consume`, and `readField` lambdas into a translation-unit-local parser object that owns only `content` and `position`. Keep the same delimiter checks, overflow handling, last-good-offset behavior, tombstone/value tags, and damaged-tail truncation.

- [ ] **Step 3: Verify WAL and iterator behavior**

Run:

```bash
cmake --build cmake-build-debug --target memtable_tests --parallel 11
ctest --test-dir cmake-build-debug --output-on-failure -R '^MemTableTest\.'
```

Expected: all MemTableTest cases pass.

### Task 6: SSTable structure

**Files:**
- Modify: `inc/SSTable.h`
- Modify: `src/SSTable.cpp`
- Test: `tests/sstable_tests.cpp`

**Interfaces:**
- Consumes: current SSTable binary format, sparse index, Bloom filter envelope, and iterator API.
- Produces: the same binary data and lookup behavior with focused serialization helpers.

- [ ] **Step 1: Remove dead code and normalize state names**

Delete the now-unused `sstableNumberOrZero` helper. Rename private path/size fields with trailing underscores and update all references.

- [ ] **Step 2: Extract serialization helpers**

Name the record/index/footer byte constants; extract helpers for footer reads, index-entry writes, and footer writes. Preserve record, Bloom filter, index, and footer order exactly.

- [ ] **Step 3: Clarify lookup and iterator flow**

Use intention-revealing names for block start/end, current byte offset, and current record. Keep all comparison boundaries and iterator advancement order unchanged.

- [ ] **Step 4: Verify SSTable behavior**

Run:

```bash
cmake --build cmake-build-debug --target sstable_tests --parallel 11
ctest --test-dir cmake-build-debug --output-on-failure -R '^SSTableTest\.'
```

Expected: all SSTableTest cases pass.

### Task 7: DB read, flush, and compaction structure

**Files:**
- Modify: `inc/DB.h`
- Modify: `src/DB.cpp`
- Test: `tests/db_tests.cpp`

**Interfaces:**
- Consumes: existing DB public API and all storage component APIs.
- Produces: smaller cohesive internal helpers with the same DB behavior.

- [ ] **Step 1: Normalize private member names**

Rename private state to `activeMemTable_`, `dataDirectory_`, `walFilePath_`, `flushThresholdBytes_`, `manifest_`, `level0CompactionThreshold_`, and `compactionSliceBytes_`. Preserve declaration order and constructor initialization order.

- [ ] **Step 2: Extract predicate and path helpers**

Extract translation-unit helpers for half-open scan overlap, compaction range selection, serialized record size, and numbered SSTable paths. Each helper must be a direct expression of the existing condition.

- [ ] **Step 3: Decompose compaction without changing data flow**

Extract helpers for collecting L0/L1 input numbers, opening iterators in descending file-number order, and writing sliced outputs. Keep source order, slice threshold comparison (`>`), current-record inclusion, Manifest update timing, and old-file removal timing unchanged.

- [ ] **Step 4: Decompose scan and point lookup**

Extract file-selection and per-table lookup helpers. Preserve active MemTable priority, L0 newest-to-oldest order, higher-level order, tombstone short-circuiting, and `[start, end)` filtering.

- [ ] **Step 5: Verify all DB behavior**

Run:

```bash
cmake --build cmake-build-debug --target db_tests --parallel 11
ctest --test-dir cmake-build-debug --output-on-failure -R '^DBTest\.'
```

Expected: all DBTest cases pass.

### Task 8: Full formatting and format-gate verification

**Files:**
- Format: `inc/*.h`
- Format: `src/*.cpp`
- Format: `tests/*.h`
- Format: `tests/*.cpp`

**Interfaces:**
- Consumes: `.clang-format` and `tools/clang-format.sh`.
- Produces: a repository-wide canonical layout accepted by CLion and the hook.

- [ ] **Step 1: Format every tracked and new C++ file**

Run:

```bash
tools/clang-format.sh format
```

- [ ] **Step 2: Verify CLI and CMake checks**

Run:

```bash
tools/clang-format.sh check
cmake --build cmake-build-debug --target format-check
```

Expected: both commands exit 0 with no format diagnostics.

- [ ] **Step 3: Verify hook rejection and acceptance**

Temporarily introduce one formatting-only mismatch with `apply_patch`, run `.githooks/pre-commit`, and expect nonzero. Reverse that exact temporary patch, rerun the hook, and expect exit 0.

### Task 9: Final verification, audit, and commit

**Files:**
- Verify all task files.
- Exclude: `docs/superpowers/plans/2026-07-11-cpp-modules-migration.md`.

**Interfaces:**
- Consumes: the fully refactored and formatted repository.
- Produces: one reviewed final commit and a clean task worktree.

- [ ] **Step 1: Run Debug verification**

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --parallel 11
ctest --test-dir cmake-build-debug --output-on-failure --parallel 11
```

Expected: build succeeds and 111/111 tests pass.

- [ ] **Step 2: Run ASan verification**

```bash
cmake -S . -B cmake-build-asan
cmake --build cmake-build-asan --parallel 11
ctest --test-dir cmake-build-asan --output-on-failure --parallel 11
```

Expected: build succeeds and 111/111 tests pass with no sanitizer report.

- [ ] **Step 3: Audit scope and whitespace**

```bash
git diff --check
git status --short
git diff --stat 9368aee
```

Confirm every task requirement has direct evidence and the unrelated modules plan remains unstaged.

- [ ] **Step 4: Run formatting immediately before commit**

```bash
tools/clang-format.sh format
tools/clang-format.sh check
```

- [ ] **Step 5: Stage task files and commit**

Stage only the design, this plan, formatting/tooling files, CMake changes, and refactored C++/test files. Then run:

```bash
git commit -m "Refactor repository structure and enforce formatting"
```

The configured hook must run and pass during this commit. Do not stage the unrelated C++ modules plan.
