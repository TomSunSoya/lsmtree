#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Manifest.h"
#include "MemTable.h"
#include "SSTable.h"

class DB
{
  public:
    explicit DB(const std::filesystem::path& dataDirectory, uint64_t flushThreshold = 5 * 1024 * 1024,
                uint64_t compactThreshold = 4, uint64_t sliceThreshold = 4 * 1024 * 1024);

    bool put(const std::string& key, const std::string& value);
    bool get(std::string_view key, std::string& value) const;
    bool remove(const std::string& key);

    void flush();
    void compact();
    [[nodiscard]] std::vector<Record> scan(std::string_view start, std::string_view end) const;

  private:
    bool searchSSTables(std::string_view key, std::string& value) const;
    Result searchTable(uint64_t tableNumber, std::string_view key, std::string& value) const;
    void compactLevel0();
    void maybeCompact();

    std::unique_ptr<MemTable> activeMemTable_;
    std::filesystem::path dataDirectory_;
    std::filesystem::path walFilePath_;
    uint64_t flushThresholdBytes_;
    std::unique_ptr<Manifest> manifest_;
    uint64_t level0CompactionThreshold_;
    uint64_t compactionSliceBytes_;
    mutable std::unordered_map<uint64_t, std::unique_ptr<SSTable>> tables_{};
};
