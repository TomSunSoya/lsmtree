# Repository Readability and Formatting Design

## Goal

Restructure the entire C++ repository for readability and simplicity while preserving all observable behavior, then establish one reproducible formatting workflow shared by the command line, CMake, Git commits, and CLion.

## Non-negotiable invariants

- Public class and function signatures remain source-compatible unless a declaration is only private implementation detail.
- WAL, SSTable, Manifest, Bloom filter, and index binary layouts remain byte-for-byte compatible.
- Record precedence, tombstone handling, scan interval semantics, compaction input selection, output slicing, and table ordering remain unchanged.
- Existing exception types, filesystem publication order, cleanup behavior, and reopen behavior remain unchanged.
- The baseline is commit `9368aee`, where the Debug build and all 111 tests pass.
- Refactoring is complete only when formatting checks, the full Debug suite, and the full ASan suite pass.

## Refactoring approach

### Production code

All headers and implementation files may be structurally refactored without changing algorithms:

- Replace vague or inconsistent local names with names that expose intent.
- Extract cohesive private or translation-unit helpers from long functions while preserving statement order and data flow.
- Replace repeated literals with named constants where the value already has one meaning.
- Remove dead helpers, unused includes, redundant conditions, and obsolete comments.
- Make ownership and mutation visible through `const`, references, spans, and narrow scopes without changing lifetime.
- Keep disk-format reads and writes in their existing order and widths.
- Keep lookup and merge source ordering exactly as implemented at the baseline.
- Avoid introducing new abstractions that are used only once unless they shorten a genuinely long operation.

The six components remain separate: `DB`, `Manifest`, `MemTable`, `SSTable`, `BloomFilter`, and shared utilities. This task does not migrate to modules, introduce namespaces, redesign APIs, or change storage behavior.

### Tests

- Introduce `tests/test_support.h` for shared filesystem cleanup and binary file helpers.
- Replace manual start/end cleanup with an RAII workspace guard so fatal assertions do not leave artifacts.
- Keep domain-specific assertions local to each test file.
- Preserve every GTest suite and test name.
- Keep tests separated by component; do not split or rename test source files in this pass because that would inflate the formatting diff without improving behavior coverage.

### CMake

- Declare the project as C++ only and propagate C++23 through the library target.
- Use a small helper function to remove repeated test-target setup while preserving the six existing executable target names.
- Add `format` and `format-check` targets through a dedicated `cmake/Format.cmake` module.
- Keep GTest discovery behavior and timeout unchanged.

## Formatting workflow

The repository standard is `clang-format 22.1.8`.

- `.clang-format` is the canonical C++ style file and is directly recognized by CLion.
- `.clang-format-version` records the required formatter version for scripts and developer setup.
- The style is based on LLVM, with four-space indentation, Allman braces, a 120-column limit, left-aligned pointers/references, sorted includes, and no single-line control statements.
- `.editorconfig` defines UTF-8, LF endings, final newlines, and trailing-whitespace cleanup without duplicating C++ layout rules.
- `tools/clang-format.sh format` formats every tracked C/C++ file.
- `tools/clang-format.sh check` performs a dry run with errors on differences.
- CMake exposes the same operations as `format` and `format-check` targets.
- `.githooks/pre-commit` runs the format check and blocks unformatted commits.
- The local repository sets `core.hooksPath=.githooks`; no global Git setting is changed.
- The hook checks but does not silently rewrite or stage files.

## Execution order

1. Add formatting configuration, scripts, CMake targets, and the versioned Git hook.
2. Refactor test support and CMake duplication.
3. Refactor each production component, running its focused tests after each component.
4. Run the formatter across every tracked C++ source and header.
5. Verify format check, Debug build/tests, ASan build/tests, `git diff --check`, and hook behavior.
6. Review the final diff for semantic drift and commit all task-related changes once.

The unrelated untracked `docs/superpowers/plans/2026-07-11-cpp-modules-migration.md` file is explicitly outside this task and must not be staged.

## Failure handling

- If a focused test fails after a refactor, revert only that refactor locally and reapply it in smaller steps; do not alter expectations to match changed behavior.
- If `clang-format` changes behavior-sensitive macro or generated content, exclude only that specific file with a documented reason. No such exclusion is expected for the current repository.
- If CLion's bundled formatter produces a different result, configure CLion to use `/opt/homebrew/opt/llvm/bin/clang-format`; the checked-in style remains authoritative.
- If ASan exposes an existing defect, stop before the final commit and report it rather than suppressing the check.

## Acceptance criteria

- Every tracked `.h` and `.cpp` file passes `clang-format --dry-run --Werror --style=file`.
- CLion discovers the root `.clang-format` without a project-specific ignored settings file.
- `cmake --build cmake-build-debug --target format-check` succeeds.
- A deliberately unformatted tracked C++ change is rejected by the pre-commit hook, and the clean formatted tree is accepted.
- The Debug and ASan builds succeed and all registered tests pass.
- The final worktree contains no task-related unstaged or staged changes after the final commit.
