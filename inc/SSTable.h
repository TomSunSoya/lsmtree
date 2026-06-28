#pragma once

#include "BloomFilter.h"
#include "MemTable.h"

class Cursor;

class SSTable
{
public:
    static void build(const MemTable &mt, const std::filesystem::path& path);
    static void merge(std::vector<std::filesystem::path> inputs, const std::filesystem::path& outPath);
    static void cleanupOrphanedTemps(const std::filesystem::path& dir);

    explicit SSTable(std::filesystem::path path);

    Result get(std::string_view key, std::string &value) const;

private:
    std::filesystem::path path;
    uint64_t recordsSize{}, bloomSize{}, indexSize{};
    std::unique_ptr<BloomFilter> bloomFilter{};
    friend class Cursor;
    static constexpr size_t BLOCK_SIZE = 4 * 1024;

    static std::optional<Record> readOneRecord(std::ifstream &ifs);
    static void addRecordToFile(const std::filesystem::path &path, const std::vector<Record> &records);
};

class Cursor
{
public:
    explicit Cursor(std::filesystem::path path);

    bool valid() const;
    const Record &current() const;
    void advance();

private:
    std::filesystem::path path;
    std::optional<Record> currentRecord{};
    std::ifstream ifs;
    uint64_t recordsSize{}, bloomSize{}, currentPos{}, indexSize{};
};
