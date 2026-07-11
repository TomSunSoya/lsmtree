module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BloomFilter.h"
#include "MemTable.h"
#include "utils.h"

export module lsm.sstable;

export class SSTable
{
  public:
    static std::pair<std::string, std::string> build(const MemTable& memTable, const std::filesystem::path& path);
    static void cleanupOrphanedTemps(const std::filesystem::path& directory);
    static void addRecordToFile(std::span<Record> records, const std::filesystem::path& path);

    explicit SSTable(std::filesystem::path path);

    Result get(std::string_view key, std::string& value) const;

  private:
    static constexpr size_t kBlockSize = 4 * 1024;

    [[nodiscard]] std::vector<Index> readSparseIndex() const;
    [[nodiscard]] std::optional<std::pair<Index, uint64_t>> getBlock(std::string_view key) const;

    std::filesystem::path path_;
    uint64_t recordsSize_{};
    uint64_t bloomSize_{};
    uint64_t indexSize_{};
    std::unique_ptr<BloomFilter> bloomFilter_{};
};

export class SSTableIterator : public Iterator
{
  public:
    explicit SSTableIterator(std::filesystem::path path);

    [[nodiscard]] bool valid() const override;
    [[nodiscard]] const Record& current() const override;
    void advance() override;

  private:
    std::optional<Record> currentRecord_{};
    std::ifstream input_;
    uint64_t recordsSize_{};
    uint64_t currentPosition_{};
};
