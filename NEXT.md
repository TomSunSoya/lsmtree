# MVCC 第三刀:快照读——DB 暴露 getSnapshot()(返回当前 seq 的一个凭据),get/scan 接受可选快照参数;读路径把 readSeq 从 memtable 一路通到 SSTable 侧(SSTable 的 get/块扫描也要按 seq ≤ 快照过滤、同 key 取可见的最大 seq);测试:写 → 取快照 → 再写/删 → 用快照读到旧世界、不带快照读到新世界。
