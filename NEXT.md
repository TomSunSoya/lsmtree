DB::flush() —— 把 active MemTable 构建成一个唯一命名的 SSTable，换上空的新 MemTable；
DB::get 在 active MemTable 未命中时回落查已 flush 的 SSTable。先做显式 flush()，暂不接 put 里的阈值自动触发。