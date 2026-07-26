# lsmtree

[English](README.md) | [简体中文](README.zh-CN.md)

![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)
![CMake](https://img.shields.io/badge/CMake-4.2%2B-blue)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

An LSM-tree embedded key-value storage engine written from scratch in modern C++23 —
a learning project inspired by LevelDB, built to understand how storage engines
actually work: WAL, SSTables, bloom filters, leveled compaction, snapshots, and
crash recovery.

> This is a learning project, not a production database. It is single-threaded by
> design and favors clarity over cleverness.

## Features

- **LSM-tree storage engine** — MemTable (`std::map` ordered by key + sequence),
  WAL-backed writes, flush to immutable SSTables
- **Write-ahead log** — every mutation is appended to the WAL before applying;
  checksummed batch frames distinguish committed corruption from an incomplete,
  zero-filled, or stale tail
- **SSTable format** — records in 4 KiB blocks, in-memory sparse index,
  per-table bloom filter, and a CRC32 on every record
- **Tombstone deletion** — deletes are durable records and remain conservative
  across compaction
- **Leveled compaction** — L0 triggers on table count, deeper levels on geometrically
  growing size budgets; output is split into bounded table slices
- **Snapshots** — MVCC-style consistent reads at a pinned sequence number
- **Range scans** — merged iteration over MemTable and all levels
- **Atomic batch writes** — `WriteBatch` applies a group of puts/deletes under one sequence
- **Crash safety** — checksummed WAL/SSTable data, atomic file publication (write to
  temp + rename), orphaned temp cleanup on startup, MANIFEST for durable metadata
- **Fault-injection harness** — a test seam that intercepts `write`/`fsync` to verify
  crash-recovery behavior under I/O failures

## Architecture

```
Write path:  put/remove ──> WAL (append + fsync) ──> MemTable
                                                        │ size ≥ flush threshold
                                                        ▼
                                              SSTable (Level 0)
                                                        │ compaction
                                                        ▼
                                              Level 1 ──> Level 2 ──> ...

Read path:   get(key) ──> MemTable ──> L0 tables (newest first) ──> L1 ──> L2 ──> ...
                              (each SSTable consults its bloom filter first)
```

On-disk layout of a database directory:

```
data/
├── MANIFEST            # levels, table metadata, log number, last sequence
├── wal_000001.wal      # active write-ahead log
├── sst_000002.sst      # [checksummed records][bloom filter][sparse index][footer]
└── ...
```

## Requirements

- CMake ≥ 4.2
- A C++23 compiler (recent Clang or GCC)
- GoogleTest (e.g. `brew install googletest` on macOS)

## Build & Test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

An ASan build works the usual way:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build-asan && ctest --test-dir build-asan
```

## Quick Start

The engine is a library (`lsmtree_lib`); link it into your own target:

```cmake
add_subdirectory(lsmtree)
target_link_libraries(your_app PRIVATE lsmtree_lib)
```

```cpp
#include "DB.h"

int main()
{
    DB db("/path/to/data"); // opens or creates the database

    db.put("hello", "world");

    std::string value;
    if (db.get("hello", value))
    {
        // value == "world"
    }

    // Atomic batch: put k1, delete k2, all under one sequence number
    WriteBatch batch;
    batch.put("k1", "v1");
    batch.remove("k2");
    db.write(batch);
    batch.clear(); // write() does not consume the batch

    // Consistent snapshot reads
    auto snap = db.snapshot();
    db.get("k1", snap.seq(), value);

    // Range scan over [a, z)
    for (const auto& record : db.scan("a", "z"))
    {
        // record.key, record.value, record.seq, record.type
    }

    db.remove("hello");
}
```

`DB`'s constructor also takes optional tuning knobs: memtable flush threshold
(default 5 MiB), L0 compaction trigger (default 4 tables), compaction slice size
(default 4 MiB), and the base size budget for leveled compaction (default 10 MiB).

## Project Structure

```
inc/     public headers (DB, MemTable, SSTable, Manifest, WriteBatch, ...)
src/     engine implementation
tests/   GoogleTest suites, including crash/fault-injection tests
cmake/   CMake helpers (clang-format integration)
```

## Limitations

- Reads, writes, flushes, and compaction are single-threaded; there is no
  background compaction.
- Range scans and compaction materialize their merged input in memory. Output
  tables are size-bounded, but peak memory still grows with the input set.
- Tombstones are retained indefinitely because the engine does not yet prove
  that a compaction is writing the bottommost level.
- SSTable metadata is cached, but point reads still perform a file existence
  check and open the table. File-descriptor caching would require an eviction
  policy.
- MANIFEST is atomically rewritten in full after metadata changes rather than
  maintained as an incremental edit log.
- WAL v2, checksummed SSTables, and MANIFEST v4 are the current on-disk formats;
  older database directories are rejected rather than migrated in place.
- Test-only fault injection lives in `inc/FaultInjection.h` — engine code calls
  `fault::write`/`fault::fsync`, which cost one null check when disarmed.

## License

Distributed under the MIT License. See [LICENSE](LICENSE) for details.
