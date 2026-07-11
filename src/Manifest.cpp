#include "Manifest.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "utils.h"

namespace
{
    bool rangesOverlap(const TableMeta &table, std::string_view minKey, std::string_view maxKey)
    {
        return minKey <= table.maxKey && table.minKey <= maxKey;
    }
}

// MANIFEST binary layout:
//   uint64_t level_count
//   uint64_t log_number
//   uint8_t  version
//   uint64_t next_table_number
//   repeated level_count times:
//     uint64_t table_count
//     repeated table_count times:
//       uint64_t table_number
//       uint32_t min_key_size
//       byte[min_key_size] min_key
//       uint32_t max_key_size
//       byte[max_key_size] max_key
// Keys are length-prefixed byte strings, so they may contain separators,
// newlines, and embedded NUL bytes.
Manifest::Manifest(std::filesystem::path path) : path_(std::move(path))
{
    if (std::ifstream ifs(path_, std::ios::binary); ifs)
    {
        uint64_t levelNumber = 0;
        ifs.read(reinterpret_cast<char*>(&levelNumber), sizeof(levelNumber));
        if (!ifs)
            throw std::ios_base::failure("failed to read level number");
        ifs.read(reinterpret_cast<char*>(&logNumber_), sizeof(logNumber_));
        if (!ifs)
            throw std::ios_base::failure("failed to read log number");
        ifs.read(reinterpret_cast<char*>(&version_), sizeof(version_));
        if (!ifs)
            throw std::ios_base::failure("failed to read version");
        ifs.read(reinterpret_cast<char*>(&next_), sizeof(next_));
        if (!ifs)
            throw std::ios_base::failure("failed to read next");

        levels.resize(levelNumber);
        for (uint64_t i = 0; i < levelNumber; ++i)
        {
            uint64_t tableNumber;
            ifs.read(reinterpret_cast<char*>(&tableNumber), sizeof(tableNumber));
            if (!ifs)
                throw std::ios_base::failure("failed to read table number");
            if (tableNumber)
            {
                levels[i].resize(tableNumber);
                for (uint64_t j = 0; j < tableNumber; ++j)
                {
                    uint64_t number = 0;
                    ifs.read(reinterpret_cast<char*>(&number), sizeof(number));
                    if (!ifs)
                        throw std::ios_base::failure("failed to read table's number");
                    uint32_t minKeySize = 0, maxKeySize = 0;
                    ifs.read(reinterpret_cast<char*>(&minKeySize), sizeof(minKeySize));
                    if (!ifs)
                        throw std::ios_base::failure("failed to read minKey size");
                    std::string minKey(minKeySize, 0);
                    ifs.read(minKey.data(), minKeySize);
                    if (!ifs)
                        throw std::ios_base::failure("failed to read minKey");
                    ifs.read(reinterpret_cast<char*>(&maxKeySize), sizeof(maxKeySize));
                    if (!ifs)
                        throw std::ios_base::failure("failed to read maxKey size");
                    std::string maxKey(maxKeySize, 0);
                    ifs.read(maxKey.data(), maxKeySize);
                    if (!ifs)
                        throw std::ios_base::failure("failed to read maxKey");
                    levels[i][j] = {number, minKey, maxKey};
                }
            }
        }
    }
}

uint64_t Manifest::nextNumber() const
{
    return next_;
}

uint64_t Manifest::allocateNumber()
{
    return next_++;
}

void Manifest::addTable(const uint64_t n, std::string_view minKey, std::string_view maxKey, uint32_t targetLevel)
{
    TableMeta table{n, std::string(minKey), std::string(maxKey)};
    if (targetLevel >= levels.size())
        levels.resize(targetLevel + 1);

    auto &level = levels[targetLevel];
    if (targetLevel == 0)
    {
        const auto pos = std::lower_bound(level.begin(), level.end(), n, [](const TableMeta &table_meta, const uint64_t value)
        {
            return table_meta.number > value;
        });
        level.insert(pos, std::move(table));
        return;
    }

    const auto pos = std::lower_bound(level.begin(), level.end(), table.minKey, [](const TableMeta &table_meta, const std::string &key)
    {
        return table_meta.minKey < key;
    });
    if (pos != level.begin() && rangesOverlap(*std::prev(pos), table.minKey, table.maxKey))
        throw std::invalid_argument("overlapping key range");
    if (pos != level.end() && rangesOverlap(*pos, table.minKey, table.maxKey))
        throw std::invalid_argument("overlapping key range");
    level.insert(pos, std::move(table));
}

void Manifest::replaceTables(const std::vector<uint64_t>& removed, const std::vector<TableMeta>& added, const uint32_t targetLevel)
{
    if (!levels.empty())
    {
        for (auto &level : levels)
        {
            std::erase_if(level, [&](const TableMeta &table_meta)
            {
                return std::ranges::find_if(removed, [&table_meta](const uint64_t remove)
                {
                    return table_meta.number == remove;
                }) != removed.end();
            });
        }
    }
    for (const auto &[number, minKey, maxKey] : added)
        addTable(number, minKey, maxKey, targetLevel);
}

void Manifest::save() const
{
    FileWriter writer(path_);

    const uint64_t levelNumber = levelCount();
    writeAll(writer.getFd(), &levelNumber, sizeof(levelNumber));
    writeAll(writer.getFd(), &logNumber_, sizeof(logNumber_));
    writeAll(writer.getFd(), &version_, sizeof(version_));
    writeAll(writer.getFd(), &next_, sizeof(next_));

    for (const auto &level : levels)
    {
        const uint64_t tableNumber = level.size();
        writeAll(writer.getFd(), &tableNumber, sizeof(tableNumber));
        for (const auto &[number, minKey, maxKey] : level)
        {
            writeAll(writer.getFd(), &number, sizeof(number));
            uint32_t minKeySize = minKey.length(), maxKeySize = maxKey.length();
            writeAll(writer.getFd(), &minKeySize, sizeof(minKeySize));
            writeAll(writer.getFd(), minKey.data(), minKey.length());
            writeAll(writer.getFd(), &maxKeySize, sizeof(maxKeySize));
            writeAll(writer.getFd(), maxKey.data(), maxKey.length());
        }
    }
    writer.finish();
}

const std::vector<TableMeta>& Manifest::level(size_t n) const
{
    static constexpr std::vector<TableMeta> emptyLevels;
    if (n >= levels.size())
        return emptyLevels;
    return levels[n];
}

size_t Manifest::levelCount() const
{
    return levels.size();
}

std::set<uint64_t, std::greater<>> Manifest::allTableNumbers() const
{
    std::set<uint64_t, std::greater<>> tableNumbers;
    for (const auto &level : levels)
        for (const auto &table : level)
            tableNumbers.insert(table.number);
    return tableNumbers;
}

std::optional<TableMeta> Manifest::getTableMeta(const uint64_t n_level, std::string_view key) const
{
    if (n_level == 0)
        throw std::invalid_argument("Cannot get ZERO level");

    if (n_level >= levels.size())
        return std::nullopt;

    const auto &level = levels[n_level];
    auto it = std::upper_bound(level.begin(), level.end(), key, [](const std::string_view value, const TableMeta &table)
    {
       return table.minKey > value;
    });

    if (it == level.begin())
        return std::nullopt;
    it = std::prev(it);
    if (key >= it->minKey && key <= it->maxKey)
        return *it;
    return std::nullopt;
}
