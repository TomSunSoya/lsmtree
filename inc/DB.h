#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Manifest.h"
#include "MemTable.h"
#include "SSTable.h"

class Snapshot;

class DB
{
  public:
    explicit DB(const std::filesystem::path& dataDirectory, uint64_t flushThreshold = 5 * 1024 * 1024,
                uint64_t compactThreshold = 4, uint64_t sliceThreshold = 4 * 1024 * 1024,
                uint64_t compactBaseThresholdBytes = 10 * 1024 * 1024);

    bool put(const std::string& key, const std::string& value);
    bool get(std::string_view key, std::string& value) const;
    bool get(std::string_view key, uint64_t readSeq, std::string& value) const;
    bool remove(const std::string& key);

    void flush();
    void compact();
    [[nodiscard]] std::vector<Record> scan(std::string_view start, std::string_view end,
                                           uint64_t readSeq = std::numeric_limits<uint64_t>::max()) const;

    Snapshot snapshot();

    [[nodiscard]] size_t activeSnapshotCount() const;

  private:
    bool searchSSTables(std::string_view key, uint64_t readSeq, std::string& value) const;
    Result searchTable(uint64_t tableNumber, std::string_view key, uint64_t readSeq, std::string& value) const;
    void maybeCompact();
    void compactLevel(uint64_t n);
    void compactRange(const std::vector<uint64_t>& removedTables, uint64_t targetLevel);
    uint64_t levelBytes(uint64_t level) const;
    uint64_t budgetFor(uint64_t n) const;
    std::optional<uint64_t> getFirstOverLevel() const;
    void getNextCrossTable(std::vector<TableMeta>& tables, uint64_t nextLevel) const;
    uint64_t smallestActiveSnapShot() const;
    [[nodiscard]] uint64_t getSnapshot() const;
    void releaseSnapshot(uint64_t seq);

    std::unique_ptr<MemTable> activeMemTable_;
    std::filesystem::path dataDirectory_;
    std::filesystem::path walFilePath_;
    uint64_t flushThresholdBytes_;
    std::unique_ptr<Manifest> manifest_;
    uint64_t level0CompactionThreshold_;
    uint64_t compactionSliceBytes_;
    uint64_t compactBaseThresholdBytes_;
    std::unordered_map<uint64_t, std::string> cursors_;
    mutable std::unordered_map<uint64_t, std::unique_ptr<SSTable>> tables_{};
    uint64_t nextSeq_{};
    mutable std::multiset<uint64_t> compactSeqs_{};

    friend class Snapshot;
};

class Snapshot
{
  public:
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&& other) noexcept;
    Snapshot& operator=(Snapshot&& other) noexcept;
    ~Snapshot();

    [[nodiscard]] uint64_t seq() const;

  private:
    friend class DB;
    Snapshot(DB* db, uint64_t seq);

    DB* db_;
    uint64_t seq_;
};