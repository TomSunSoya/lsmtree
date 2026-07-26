#include "Manifest.h"

#include <algorithm>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace
{
template <typename Value> void readValue(std::ifstream& input, Value& value, const char* errorMessage)
{
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input)
        throw std::ios_base::failure(errorMessage);
}

std::string readKey(std::ifstream& input, const char* sizeErrorMessage, const char* keyErrorMessage)
{
    uint32_t keySize = 0;
    readValue(input, keySize, sizeErrorMessage);

    std::string key(keySize, '\0');
    input.read(key.data(), keySize);
    if (!input)
        throw std::ios_base::failure(keyErrorMessage);
    return key;
}

TableMeta readTable(std::ifstream& input)
{
    uint64_t tableNumber = 0;
    uint64_t tableSize = 0;
    readValue(input, tableNumber, "failed to read table's number");
    readValue(input, tableSize, "failed to read table's size");
    std::string minKey = readKey(input, "failed to read minKey size", "failed to read minKey");
    std::string maxKey = readKey(input, "failed to read maxKey size", "failed to read maxKey");
    return {tableNumber, tableSize, std::move(minKey), std::move(maxKey)};
}

template <typename Value> void writeValue(const FileWriter& writer, const Value& value)
{
    writeAll(writer.getFd(), &value, sizeof(value));
}

void writeKey(const FileWriter& writer, const std::string& key)
{
    const uint32_t keySize = key.length();
    writeValue(writer, keySize);
    writeAll(writer.getFd(), key.data(), key.length());
}
} // namespace

// MANIFEST binary layout:
//   uint64_t level_count
//   uint64_t log_number
//   uint8_t  version
//   uint64_t next_table_number
//   uint64_t last_sequence
//   repeated level_count times:
//     uint64_t table_count
//     repeated table_count times:
//       uint64_t table_number
//       uint64_t table_size
//       uint32_t min_key_size
//       byte[min_key_size] min_key
//       uint32_t max_key_size
//       byte[max_key_size] max_key
// Keys are length-prefixed byte strings, so they may contain separators,
// newlines, and embedded NUL bytes.
Manifest::Manifest(std::filesystem::path path) : path_(std::move(path))
{
    if (std::error_code ec; !std::filesystem::exists(path_, ec))
    {
        if (ec)
            throw std::system_error(ec, "failed to inspect MANIFEST");
        return;
    }

    std::ifstream input(path_, std::ios::binary);
    if (!input)
        throw std::ios_base::failure("MANIFEST exists but cannot be opened!");

    uint64_t levelCount = 0;
    uint8_t formatVersion = 0;
    readValue(input, levelCount, "failed to read level number");
    readValue(input, logNumber_, "failed to read log number");
    readValue(input, formatVersion, "failed to read version");
    if (formatVersion != kManifestFormatVersion)
        throw std::runtime_error("Unsupported manifest version!");
    readValue(input, nextTableNumber_, "failed to read next");
    readValue(input, lastSeq_, "failed to read last seq");

    levels_.resize(levelCount);
    for (auto& level : levels_)
    {
        uint64_t tableCount = 0;
        readValue(input, tableCount, "failed to read table number");
        level.reserve(tableCount);
        for (uint64_t tableIndex = 0; tableIndex < tableCount; ++tableIndex)
            level.push_back(readTable(input));
    }
}

uint64_t Manifest::nextNumber() const { return nextTableNumber_; }

uint64_t Manifest::allocateNumber() { return nextTableNumber_++; }

uint64_t Manifest::lastSeq() const { return lastSeq_; }

void Manifest::setLastSeq(const uint64_t lastSeq) { lastSeq_ = lastSeq; }

void Manifest::addTable(uint64_t number, uint64_t size, std::string_view minKey, std::string_view maxKey,
                        uint32_t targetLevel)
{
    TableMeta table{number, size, std::string(minKey), std::string(maxKey)};
    if (targetLevel >= levels_.size())
        levels_.resize(targetLevel + 1);

    auto& level = levels_[targetLevel];
    if (targetLevel == 0)
    {
        const auto position =
            std::lower_bound(level.begin(), level.end(), number,
                             [](const TableMeta& candidate, const uint64_t value) { return candidate.number > value; });
        level.insert(position, table);
        return;
    }

    const auto position =
        std::lower_bound(level.begin(), level.end(), table.minKey,
                         [](const TableMeta& candidate, const std::string& key) { return candidate.minKey < key; });
    if (position != level.begin() && rangesOverlap(*std::prev(position), table.minKey, table.maxKey))
        throw std::invalid_argument("overlapping key range");
    if (position != level.end() && rangesOverlap(*position, table.minKey, table.maxKey))
        throw std::invalid_argument("overlapping key range");

    level.insert(position, std::move(table));
}

void Manifest::replaceTables(const std::vector<uint64_t>& removed, const std::vector<TableMeta>& added,
                             uint32_t targetLevel)
{
    for (auto& level : levels_)
    {
        std::erase_if(level, [&removed](const TableMeta& table)
                      { return std::ranges::find(removed, table.number) != removed.end(); });
    }

    for (const auto& [number, size, minKey, maxKey] : added)
        addTable(number, size, minKey, maxKey, targetLevel);
}

void Manifest::save() const
{
    FileWriter writer(path_);

    const uint64_t levelCount = levels_.size();
    writeValue(writer, levelCount);
    writeValue(writer, logNumber_);
    writeValue(writer, kManifestFormatVersion);
    writeValue(writer, nextTableNumber_);
    writeValue(writer, lastSeq_);

    for (const auto& level : levels_)
    {
        const uint64_t tableCount = level.size();
        writeValue(writer, tableCount);
        for (const auto& [number, size, minKey, maxKey] : level)
        {
            writeValue(writer, number);
            writeValue(writer, size);
            writeKey(writer, minKey);
            writeKey(writer, maxKey);
        }
    }
    writer.finish();
}

const std::vector<TableMeta>& Manifest::level(size_t number) const
{
    static const std::vector<TableMeta> emptyLevel;
    if (number >= levels_.size())
        return emptyLevel;
    return levels_[number];
}

size_t Manifest::levelCount() const { return levels_.size(); }

std::set<uint64_t, std::greater<>> Manifest::allTableNumbers() const
{
    std::set<uint64_t, std::greater<>> tableNumbers;
    for (const auto& level : levels_)
    {
        for (const auto& table : level)
            tableNumbers.insert(table.number);
    }
    return tableNumbers;
}

std::optional<TableMeta> Manifest::getTableMeta(uint64_t levelNumber, std::string_view key) const
{
    if (levelNumber == 0)
        throw std::invalid_argument("Cannot get ZERO level");
    if (levelNumber >= levels_.size())
        return std::nullopt;

    const auto& level = levels_[levelNumber];
    auto position =
        std::upper_bound(level.begin(), level.end(), key,
                         [](const std::string_view value, const TableMeta& table) { return table.minKey > value; });

    if (position == level.begin())
        return std::nullopt;
    --position;
    if (key >= position->minKey && key <= position->maxKey)
        return *position;
    return std::nullopt;
}
