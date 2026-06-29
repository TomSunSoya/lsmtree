#include "SSTable.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr std::string_view kSSTablePrefix = "sst_";
    constexpr auto kSSTableSuffix = ".sst";
    constexpr uint64_t kRecordHeaderSize = sizeof(char) + 2 * sizeof(uint32_t);
    constexpr uint64_t kIndexMetadataSize = sizeof(uint32_t) + sizeof(uint64_t);
    constexpr size_t kFooterSize = 3 * sizeof(uint64_t);

    struct Footer
    {
        uint64_t recordsSize;
        uint64_t bloomSize;
        uint64_t indexSize;
    };

    Footer readFooter(std::ifstream &input)
    {
        input.seekg(-static_cast<std::streamoff>(kFooterSize), std::ios::end);

        std::array<std::byte, kFooterSize> buffer;
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));

        Footer footer{};
        std::memcpy(&footer.recordsSize, buffer.data(), sizeof(footer.recordsSize));
        std::memcpy(&footer.bloomSize, buffer.data() + sizeof(footer.recordsSize), sizeof(footer.bloomSize));
        std::memcpy(
            &footer.indexSize,
            buffer.data() + sizeof(footer.recordsSize) + sizeof(footer.bloomSize),
            sizeof(footer.indexSize));
        return footer;
    }

    uint64_t sstableNumberOrZero(const fs::path &path)
    {
        return parseNumberedFile(path.filename().string(), kSSTablePrefix, kSSTableSuffix).value_or(0);
    }

    uint64_t serializedRecordSize(const std::optional<Record> &record)
    {
        if (!record)
            return 0;
        return kRecordHeaderSize + record->key.size() + record->value.size();
    }

    uint64_t serializedIndexSize(const Index &index)
    {
        return kIndexMetadataSize + index.key.size();
    }
}

void SSTable::build(const MemTable &mt, const std::filesystem::path &path)
{
    if (std::filesystem::exists(path))
        throw std::runtime_error("SSTable file already exists!");

    std::vector<Record> records;
    records.reserve(mt.size());
    for (auto &[key, entry] : mt)
        records.emplace_back(key, entry.type, entry.value);

    addRecordToFile(path, records);
}

void SSTable::merge(std::vector<std::filesystem::path> inputs, const std::filesystem::path& outPath)
{
    std::ranges::sort(inputs, [] (const fs::path &a, const fs::path &b)
    {
        return sstableNumberOrZero(a) < sstableNumberOrZero(b);
    });

    std::vector<Cursor> cursors;
    cursors.reserve(inputs.size());
    for (const auto &path : inputs)
        cursors.emplace_back(path);

    struct MergeItem
    {
        std::string key;
        int index;

        bool operator<(const MergeItem &item) const
        {
            if (key != item.key)
                return key > item.key;
            return index < item.index;
        }
    };

    std::priority_queue<MergeItem> items;
    for (int i = 0; i < cursors.size(); ++i)
    {
        if (auto &cursor = cursors[i]; cursor.valid())
            items.push({cursor.current().key, i});
    }

    std::vector<Record> records;

    while (!items.empty())
    {
        const auto [key, index] = items.top();
        items.pop();
        auto &cursor = cursors[index];
        records.push_back(cursor.current());

        cursor.advance();

        while (!items.empty() && key == items.top().key)
        {
            const auto duplicateIndex = items.top().index;
            auto &duplicateCursor = cursors[duplicateIndex];
            duplicateCursor.advance();
            items.pop();

            if (duplicateCursor.valid())
                items.push({duplicateCursor.current().key, duplicateIndex});
        }

        if (cursor.valid())
            items.push({cursor.current().key, index});
    }
    addRecordToFile(outPath, records);
}

void SSTable::cleanupOrphanedTemps(const std::filesystem::path& dir)
{
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec))
        return;

    for (const auto &entry : fs::directory_iterator(dir, ec))
    {
        if (ec)
            return;

        if (!entry.is_regular_file(ec))
            continue;

        const fs::path &path = entry.path();
        if (path.extension() != ".tmp")
            continue;

        fs::remove(path, ec);
        if (ec)
            throw std::runtime_error("remove failed: " + path.string() + ", reason: " + ec.message());
    }
}

SSTable::SSTable(std::filesystem::path path) : path(std::move(path))
{
    namespace fs = std::filesystem;
    if (!fs::exists(this->path) || !fs::is_regular_file(this->path))
        throw std::runtime_error("SSTable file is not a regular file: " + this->path.string());

    std::ifstream ifs{this->path, std::ios::binary};
    if (!ifs)
        throw std::runtime_error("Could not open file: " + this->path.string());

    const auto footer = readFooter(ifs);
    recordsSize = footer.recordsSize;
    bloomSize = footer.bloomSize;
    indexSize = footer.indexSize;

    ifs.seekg(recordsSize, std::ios::beg);
    std::vector<std::byte> bloomBytes(bloomSize);
    ifs.read(reinterpret_cast<char*>(bloomBytes.data()), bloomSize);
    bloomFilter = std::make_unique<BloomFilter>(BloomFilter::fromBytes(bloomBytes));
}

Result SSTable::get(const std::string_view key, std::string &value) const
{
    if (!std::filesystem::exists(path) || !bloomFilter->mightContain(key))
        return Result::ABSENT;

    std::ifstream ifs{path, std::ios::binary};
    if (!ifs.is_open())
        return Result::ABSENT;

    const auto block = getBlock(key);
    if (!block)
        return Result::ABSENT;

    const auto &[firstIndex, blockEnd] = *block;
    ifs.seekg(firstIndex.offset, std::ios::beg);
    uint64_t currentOffset = firstIndex.offset;
    while (currentOffset < blockEnd)
    {
        auto record = readOneRecord(ifs);
        if (!record)
            break;
        currentOffset += serializedRecordSize(record);
        if (record->key == key)
        {
            if (record->type != Type::VALUE)
                return Result::TOMBSTONE;
            value = record->value;
            return Result::VALUE;
        }
    }
    return Result::ABSENT;
}

std::optional<std::pair<Index, uint64_t>> SSTable::getBlock(const std::string_view key) const
{
    std::ifstream ifs{path, std::ios::binary};
    if (!ifs)
        throw std::runtime_error("Could not open file: " + path.string());

    std::vector<Index> indices;
    ifs.seekg(recordsSize + bloomSize, std::ios::beg);
    uint64_t indexBytesRead = 0;
    while (indexBytesRead < indexSize)
    {
        const auto index = readOneIndex(ifs);
        if (!index)
            break;

        indices.push_back(*index);
        indexBytesRead += serializedIndexSize(*index);
    }

    const auto it = std::upper_bound(indices.begin(), indices.end(), key,
        [](const std::string_view value, const Index &item)
    {
        return item.key > value;
    });

    if (it == indices.begin())
        return std::nullopt;

    const uint64_t blockEnd = it == indices.end() ? recordsSize : it->offset;
    return std::make_pair(*std::prev(it), blockEnd);
}

std::optional<Record> SSTable::readOneRecord(std::ifstream& ifs)
{
    char type = 0;
    if (!ifs.read(&type, sizeof(type)))
        return std::nullopt;

    uint32_t keySize = 0;
    uint32_t valueSize = 0;
    if (!ifs.read(reinterpret_cast<char*>(&keySize), sizeof(keySize)))
        return std::nullopt;
    if (!ifs.read(reinterpret_cast<char*>(&valueSize), sizeof(valueSize)))
        return std::nullopt;

    std::string key(keySize, '\0');
    std::string value(valueSize, '\0');
    if (!ifs.read(key.data(), keySize))
        return std::nullopt;
    if (!ifs.read(value.data(), valueSize))
        return std::nullopt;

    Record record{};
    record.key = std::move(key);
    record.value = std::move(value);
    record.type = static_cast<Type>(type);
    return record;
}

std::optional<Index> SSTable::readOneIndex(std::ifstream& ifs)
{
    uint32_t keySize = 0;
    if (!ifs.read(reinterpret_cast<char*>(&keySize), sizeof(keySize)))
        return std::nullopt;

    std::string key(keySize, '\0');
    if (!ifs.read(key.data(), keySize))
        return std::nullopt;

    uint64_t offset = 0;
    if (!ifs.read(reinterpret_cast<char*>(&offset), sizeof(offset)))
        return std::nullopt;

    return Index{keySize, std::move(key), offset};
}

void SSTable::addRecordToFile(const std::filesystem::path& path, const std::vector<Record>& records)
{
    BloomFilter bloomFilter(records.size(), 0.01);
    FileWriter writer(path);

    std::vector<Index> indices;
    uint64_t recordsSize = 0;
    uint64_t currentBlockSize = 0;
    for (const auto & [key, type, value] : records)
    {
        if (recordsSize == 0 || currentBlockSize > BLOCK_SIZE)
        {
            currentBlockSize = 0;
            indices.emplace_back(key.size(), key, recordsSize);
        }

        const auto bytesWritten = writer.add({key, type, value});
        recordsSize += bytesWritten;
        currentBlockSize += bytesWritten;
        bloomFilter.add(key);
    }

    const auto bloomBytes = BloomFilter::Serialize(bloomFilter);
    writeAll(writer.getFd(), bloomBytes.data(), bloomBytes.size());

    uint64_t indicesSize = 0;
    for (const auto & [keySize, key, offset] : indices)
    {
        writeAll(writer.getFd(), &keySize, sizeof(keySize));
        writeAll(writer.getFd(), key.data(), key.size());
        writeAll(writer.getFd(), &offset, sizeof(offset));
        indicesSize += sizeof(keySize) + key.size() + sizeof(offset);
    }

    writeAll(writer.getFd(), &recordsSize, sizeof(recordsSize));

    const auto bloomSize = bloomBytes.size();
    writeAll(writer.getFd(), &bloomSize, sizeof(bloomSize));

    writeAll(writer.getFd(), &indicesSize, sizeof(indicesSize));

    writer.finish();
}

Cursor::Cursor(std::filesystem::path path) : path(std::move(path))
{
    if (!std::filesystem::exists(this->path))
        throw std::runtime_error("Invalid path");

    ifs.open(this->path, std::ios::binary);
    if (!ifs.is_open())
        throw std::runtime_error("Failed to open SSTable file!");

    const auto footer = readFooter(ifs);
    recordsSize = footer.recordsSize;
    bloomSize = footer.bloomSize;
    indexSize = footer.indexSize;

    ifs.seekg(0, std::ios::beg);

    if (currentPos < recordsSize)
    {
        currentRecord = SSTable::readOneRecord(ifs);
        currentPos += serializedRecordSize(currentRecord);
    }
}

bool Cursor::valid() const
{
    return currentRecord.has_value();
}

const Record& Cursor::current() const
{
    assert(valid());
    return *currentRecord;
}

void Cursor::advance()
{
    if (valid() && currentPos < recordsSize)
    {
        currentRecord = SSTable::readOneRecord(ifs);
        currentPos += serializedRecordSize(currentRecord);
        return;
    }

    currentRecord = std::nullopt;
}
