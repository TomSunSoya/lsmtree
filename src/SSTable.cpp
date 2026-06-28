#include "SSTable.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BloomFilter.h"

namespace
{
    namespace fs = std::filesystem;

    constexpr std::string_view kSSTablePrefix = "sst_";
    constexpr auto kSSTableSuffix = ".sst";

    std::optional<uint64_t> parseSSTableNumber(const fs::path &path)
    {
        return parseNumberedFile(path.filename().string(), kSSTablePrefix, kSSTableSuffix);
    }

    uint64_t sstableNumberOrZero(const fs::path &path)
    {
        const auto number = parseSSTableNumber(path);
        return number.value_or(0);
    }

    uint64_t getRecordSize(const std::optional<Record> &record)
    {
        if (!record)
            return 0;
        return 9 + record->key.size() + record->value.size();
    }
}

void SSTable::build(const MemTable &mt, const std::filesystem::path &path)
{
    if (std::filesystem::exists(path))
        throw std::runtime_error("SSTable file already exists!");

    std::vector<Record> records;
    for (auto &[key, entry] : mt)
    {
        records.emplace_back(key, entry.type, entry.value);
    }

    addRecordToFile(path, records);
}

void SSTable::merge(std::vector<std::filesystem::path> inputs, const std::filesystem::path& outPath)
{
    std::ranges::sort(inputs, [] (const fs::path &a, const fs::path &b)
    {
        return sstableNumberOrZero(a) < sstableNumberOrZero(b);
    });

    std::vector<Cursor> cursors;
    std::ranges::transform(inputs, std::back_inserter(cursors), [] (const fs::path &path)
    {
        return Cursor{path};
    });

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

        if (const fs::path &path = entry.path(); path.extension() == ".tmp")
        {
            fs::remove(path, ec);
            if (ec)
                throw std::runtime_error("remove failed: " + path.string() + ", reason: " + ec.message());
        }
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

    ifs.seekg(-16, std::ios::end);

    std::byte buf[16];
    ifs.read(reinterpret_cast<char*>(buf), 16);

    std::memcpy(&recordsSize, buf, sizeof(recordsSize));
    std::memcpy(&bloomSize, buf + 8, sizeof(bloomSize));

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

    uint64_t pos = 0;
    while (pos < recordsSize)
    {
        auto record = readOneRecord(ifs);
        if (!record)
            break;
        pos += getRecordSize(record);
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

std::optional<Record> SSTable::readOneRecord(std::ifstream& ifs)
{
    char type = 0;
    if (ifs.read(&type, sizeof(char)))
    {
        Record record{};
        uint32_t key_size{}, value_size{};
        if (!ifs.read(reinterpret_cast<char *>(&key_size), sizeof(key_size)))
            return std::nullopt;
        if (!ifs.read(reinterpret_cast<char *>(&value_size), sizeof(value_size)))
            return std::nullopt;

        std::string cur_key(key_size, 0);
        std::string cur_value(value_size, 0);

        if (!ifs.read(cur_key.data(), key_size))
            return std::nullopt;
        if (!ifs.read(cur_value.data(), value_size))
            return std::nullopt;

        record.key = cur_key;
        record.value = cur_value;
        record.type = static_cast<Type>(type);

        return record;
    }
    return std::nullopt;
}

void SSTable::addRecordToFile(const std::filesystem::path& path, const std::vector<Record>& records)
{
    BloomFilter bloom_filter(records.size(), 0.01);
    FileWriter writer(path);

    uint64_t recordSize = 0;
    for (const auto & [key, type, value] : records)
    {
        recordSize += writer.add({key, type, value});
        bloom_filter.add(key);
    }

    const auto &bytes = BloomFilter::Serialize(bloom_filter);
    writeAll(writer.getFd(), bytes.data(), bytes.size());
    writeAll(writer.getFd(), &recordSize, sizeof(recordSize));

    const auto bloomSize = bytes.size();
    writeAll(writer.getFd(), &bloomSize, sizeof(bloomSize));
    writer.finish();
}

Cursor::Cursor(std::filesystem::path path) : path(std::move(path))
{
    if (!std::filesystem::exists(this->path))
        throw std::runtime_error("Invalid path");

    ifs.open(this->path, std::ios::binary);
    if (!ifs.is_open())
        throw std::runtime_error("Failed to open SSTable file!");

    ifs.seekg(-16, std::ios::end);

    std::byte buf[16];
    ifs.read(reinterpret_cast<char*>(buf), 16);

    std::memcpy(&recordsSize, buf, sizeof(recordsSize));
    std::memcpy(&bloomSize, buf + 8, sizeof(bloomSize));

    ifs.seekg(0, std::ios::beg);

    if (currentPos < recordsSize)
    {
        currentRecord = SSTable::readOneRecord(ifs);
        currentPos += getRecordSize(currentRecord);
    }
}

bool Cursor::valid() const
{
    return currentRecord != std::nullopt;
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
        currentPos += getRecordSize(currentRecord);
    } else
        currentRecord = std::nullopt;
}
