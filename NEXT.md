# 修复 WAL 恢复的偏移语义：从 magic + version 之后开始解析 records，并确保损坏尾部的截断位置仍是相对整个文件的绝对偏移。
