# lsmtree

[English](README.md) | [简体中文](README.zh-CN.md)

![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)
![CMake](https://img.shields.io/badge/CMake-4.2%2B-blue)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

一个用现代 C++23 从零手写的 LSM-tree 嵌入式键值存储引擎 —— 一个受 LevelDB
启发的学习项目，目标是真正搞懂存储引擎的工作原理：WAL、SSTable、布隆过滤器、
分层 compaction、快照与崩溃恢复。

> 这是一个学习项目，不是生产级数据库。它刻意保持单线程，以清晰优先于取巧。

## 功能特性

- **LSM-tree 存储引擎** —— MemTable（按 key + 序列号排序的 `std::map`）、
  WAL 背书写入、刷盘为不可变 SSTable
- **预写日志（WAL）** —— 每次变更先追加到 WAL 再应用；重开时回放日志，
  带校验和的批次帧可区分已提交数据损坏与不完整、补零或残留垃圾尾部
- **SSTable 格式** —— 4 KiB 块组织记录、内存稀疏索引、逐表布隆过滤器
  加速不存在 key 的查找，并为每条记录保存 CRC32
- **Tombstone 删除** —— 删除也是持久化记录，compaction 采用保守保留策略
- **分层 compaction** —— L0 按表数量触发，更深层按几何增长的容量预算触发；
  输出按有界大小切分为多个 SSTable
- **快照** —— MVCC 风格的一致性读，钉住某个序列号读历史视图
- **范围扫描** —— 对 MemTable 与各层 SSTable 做归并迭代
- **原子批量写** —— `WriteBatch` 将一组 put/delete 归入同一序列号整体落盘
- **崩溃安全** —— WAL/SSTable 数据校验、文件原子发布（先写临时文件再
  rename）、启动时清理孤儿临时文件、MANIFEST 持久化元数据
- **故障注入测试装置** —— 拦截 `write`/`fsync` 的测试接缝，验证 I/O 故障下
  的崩溃恢复行为

## 架构

```
写路径:  put/remove ──> WAL（追加 + fsync）──> MemTable
                                                  │ 大小 ≥ 刷盘阈值
                                                  ▼
                                         SSTable（Level 0）
                                                  │ compaction
                                                  ▼
                                         Level 1 ──> Level 2 ──> ...

读路径:  get(key) ──> MemTable ──> L0 各表（新的优先）──> L1 ──> L2 ──> ...
                          （每个 SSTable 先问自己的布隆过滤器）
```

数据库目录的磁盘布局：

```
data/
├── MANIFEST            # 层级、表元数据、日志编号、最后序列号
├── wal_000001.wal      # 当前活跃的预写日志
├── sst_000002.sst      # [带校验和的记录区][布隆过滤器][稀疏索引][footer]
└── ...
```

## 环境要求

- CMake ≥ 4.2
- 支持 C++23 的编译器（较新的 Clang 或 GCC）
- GoogleTest（macOS 上可 `brew install googletest`）

## 构建与测试

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

ASan 构建按常规方式即可：

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build-asan && ctest --test-dir build-asan
```

## 快速上手

引擎以库（`lsmtree_lib`）形式提供，链接到你自己的 target：

```cmake
add_subdirectory(lsmtree)
target_link_libraries(your_app PRIVATE lsmtree_lib)
```

```cpp
#include "DB.h"

int main()
{
    DB db("/path/to/data"); // 打开或创建数据库

    db.put("hello", "world");

    std::string value;
    if (db.get("hello", value))
    {
        // value == "world"
    }

    // 原子批量写：put k1、delete k2，归入同一序列号
    WriteBatch batch;
    batch.put("k1", "v1");
    batch.remove("k2");
    db.write(batch);
    batch.clear(); // write() 不会清空 batch

    // 快照一致性读
    auto snap = db.snapshot();
    db.get("k1", snap.seq(), value);

    // 范围扫描 [a, z)
    for (const auto& record : db.scan("a", "z"))
    {
        // record.key, record.value, record.seq, record.type
    }

    db.remove("hello");
}
```

`DB` 构造函数还提供可选的调优参数：MemTable 刷盘阈值（默认 5 MiB）、
L0 compaction 触发表数（默认 4）、compaction 切片大小（默认 4 MiB）、
以及分层 compaction 的基础容量预算（默认 10 MiB）。

## 项目结构

```
inc/     公开头文件（DB、MemTable、SSTable、Manifest、WriteBatch 等）
src/     引擎实现
tests/   GoogleTest 测试套件，含崩溃/故障注入测试
cmake/   CMake 辅助脚本（clang-format 集成）
```

## 局限

- 读写、flush 与 compaction 均为单线程，也没有后台 compaction。
- 范围扫描和 compaction 会在内存中物化完整归并输入。输出 SSTable 虽按大小
  切分，峰值内存仍随输入规模增长。
- 墓碑会被无限期保留，因为引擎尚不能证明某次 compaction 正在写入最底层。
- SSTable 元数据会缓存，但点查仍会检查文件是否存在并打开文件；缓存文件描述符
  还需要配套淘汰策略。
- 元数据变化后会原子地全量重写 MANIFEST，而不是维护增量编辑日志。
- 当前磁盘格式为 WAL v2、带校验和的 SSTable 与 MANIFEST v4；旧数据库目录
  会被拒绝打开，不会原地迁移。
- 仅用于测试的故障注入在 `inc/FaultInjection.h` —— 引擎代码调用
  `fault::write`/`fault::fsync`，未启用时每次调用只有一次空检查的开销。

## 许可证

基于 MIT 许可证发布，详见 [LICENSE](LICENSE)。
