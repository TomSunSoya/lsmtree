# CLAUDE.md — lsmtree

## 项目定位

这是一个使用 C++23 和 CMake 从零实现的单线程 LSM-tree 嵌入式 KV 存储引擎。目标是学习和解释存储引擎的核心机制，不是生产数据库。

工程由 `lsmtree_lib` 静态库和 7 个 GoogleTest target 组成，生产代码位于 `inc/` 与 `src/`：

- `DB`：协调写入、读取、flush、分层 compaction、scan 和 snapshot。
- `MemTable`：保存按 user key 升序、sequence 降序排列的多版本记录，并负责 WAL 批写与恢复。
- `SSTable`：保存不可变记录、Bloom Filter、稀疏索引和 Footer。
- `Manifest`：保存文件编号、当前 WAL、最后 sequence 和各层 SSTable 元数据。
- `WriteBatch`：为批次操作分配连续 sequence，并通过一个 WAL frame 提交。
- `Snapshot` / Iterator：实现 MVCC 可见性、范围扫描和 compaction 多路归并。
- `FaultInjection`：为测试提供 `write` / `fsync` 故障注入；当前是单线程全局策略。

## 当前阶段

功能主干和本轮持久化正确性加固已经完成，项目进入封版维护阶段。当前完整测试数为 177；普通构建和 ASan/UBSan 构建均已通过。

已经覆盖的关键边界包括：

- Manifest 无法读取时中止启动，并避免提前清理受其追踪的 SSTable。
- flush 发布失败后 DB 进入不可写状态。
- WAL 批次只在写入并同步成功后进入 MemTable。
- WAL replay 恢复 `currentSizeBytes_`，重复 `(key, sequence)` 与在线写入具有一致的覆盖语义。
- 新 WAL 同步文件和父目录。
- SSTable 对 Footer、记录、Bloom Filter 和稀疏索引边界进行校验。
- Bloom Filter 使用稳定的 FNV-1a 哈希。
- Snapshot 的注册状态可安全晚于 DB 生命周期释放。

尚未实现、也不在当前收尾范围内：并发读写、后台 compaction、乐观事务、压缩、record checksum、SQL 和网络服务。乐观事务保留到未来并发设计时一并考虑。

## 协作方式

核心功能学习阶段已经结束。对于用户明确要求的 bug 修复、测试补强、可读性重构和文档维护，可以直接实现并验证；如果要新增核心机制或改变设计边界，先解释取舍并确认范围。

工作时遵守以下约束：

- 优先保持行为不变；不要顺手改变公开 API、持久化字节布局、异常边界或 I/O 发布顺序。
- WAL、SSTable 和 Manifest 的格式尺寸必须各自只有一个真相源。
- 修改故障路径时，明确区分正常 EOF、截断尾部、corruption 和 I/O failure。
- 不把这个项目扩展成生产数据库；新想法先记入复盘或未来设计。
- 保留无关的未提交改动，不擅自回退或清理。
- 简单任务由主 agent 完成。

## 验证与提交

常规验证：

```text
cmake --build build
ctest --test-dir build --output-on-failure
```

涉及生命周期、解析或持久化的改动，还要使用 ASan/UBSan 构建运行同一套完整测试。

只有完整测试全部通过时才允许提交；除非用户明确要求，否则不要自行创建 commit。每次收工在交接信息中记录下一步。
