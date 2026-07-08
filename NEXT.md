leveled 第四步:读路径逐层走——searchFromSSTable/scan 不再用 allTableNumbers,改为:L0 按新→旧(用 manifest 里的 min/max 先剪掉不含目标 key 的表),L1 按 minKey 二分定位到最多一个表(手感同稀疏索引);验收:多层结构下点查/扫描全对、打开的文件数下降(可用测试观察 SSTable 构造次数)。
