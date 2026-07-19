#include "SSTable.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "MemTable.h"

namespace
{
constexpr uint64_t kIndexMetadataSize = sizeof(uint32_t) + sizeof(uint64_t);
constexpr size_t kFooterSize = 3 * sizeof(uint64_t);

struct Footer
{
    uint64_t recordsSize;
    uint64_t bloomSize;
    uint64_t indexSize;
};

Footer readFooter(std::ifstream& input)
{
    input.seekg(-static_cast<std::streamoff>(kFooterSize), std::ios::end);

    std::array<std::byte, kFooterSize> buffer{};
    input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());

    Footer footer{};
    std::memcpy(&footer.recordsSize, buffer.data(), sizeof(footer.recordsSize));
    std::memcpy(&footer.bloomSize, buffer.data() + sizeof(footer.recordsSize), sizeof(footer.bloomSize));
    std::memcpy(&footer.indexSize, buffer.data() + sizeof(footer.recordsSize) + sizeof(footer.bloomSize),
                sizeof(footer.indexSize));
    return footer;
}

std::optional<Record> readRecord(std::ifstream& input)
{
    char type = 0;
    if (!input.read(&type, sizeof(type)))
        return std::nullopt;

    uint64_t seq = 0;
    if (!input.read(reinterpret_cast<char*>(&seq), sizeof(seq)))
        return std::nullopt;

    uint32_t keySize = 0;
    uint32_t valueSize = 0;
    if (!input.read(reinterpret_cast<char*>(&keySize), sizeof(keySize)))
        return std::nullopt;
    if (!input.read(reinterpret_cast<char*>(&valueSize), sizeof(valueSize)))
        return std::nullopt;

    std::string key(keySize, '\0');
    std::string value(valueSize, '\0');
    if (!input.read(key.data(), keySize))
        return std::nullopt;
    if (!input.read(value.data(), valueSize))
        return std::nullopt;

    return Record{std::move(key), seq, static_cast<Type>(type), std::move(value)};
}

std::optional<Index> readIndex(std::ifstream& input)
{
    uint32_t keySize = 0;
    if (!input.read(reinterpret_cast<char*>(&keySize), sizeof(keySize)))
        return std::nullopt;

    std::string key(keySize, '\0');
    if (!input.read(key.data(), keySize))
        return std::nullopt;

    uint64_t offset = 0;
    if (!input.read(reinterpret_cast<char*>(&offset), sizeof(offset)))
        return std::nullopt;

    return Index{keySize, std::move(key), offset};
}

uint64_t serializedRecordSize(const std::optional<Record>& record)
{
    if (!record)
        return 0;
    return kEncodedRecordHeaderSize + record->key.size() + record->value.size();
}

uint64_t serializedIndexSize(const Index& index) { return kIndexMetadataSize + index.key.size(); }

uint64_t writeIndex(FileWriter& writer, const Index& index)
{
    writeAll(writer.getFd(), &index.keySize, sizeof(index.keySize));
    writeAll(writer.getFd(), index.key.data(), index.key.size());
    writeAll(writer.getFd(), &index.offset, sizeof(index.offset));
    return serializedIndexSize(index);
}

void writeFooter(FileWriter& writer, const uint64_t recordsSize, const uint64_t bloomSize, const uint64_t indexSize)
{
    writeAll(writer.getFd(), &recordsSize, sizeof(recordsSize));
    writeAll(writer.getFd(), &bloomSize, sizeof(bloomSize));
    writeAll(writer.getFd(), &indexSize, sizeof(indexSize));
}
} // namespace

std::pair<std::string, std::string> SSTable::build(const MemTable& memTable, const std::filesystem::path& path)
{
    if (std::filesystem::exists(path))
        throw std::runtime_error("SSTable file already exists!");

    std::vector<Record> records;
    records.reserve(memTable.size());
    for (const auto& [key, entry] : memTable)
        records.emplace_back(key.key, key.seq, entry.type, entry.value);

    addRecordToFile(records, path);
    return {records.front().key, records.back().key};
}

void SSTable::cleanupOrphanedTemps(const std::filesystem::path& directory)
{
    std::error_code error;
    if (!std::filesystem::exists(directory, error) || !std::filesystem::is_directory(directory, error))
        return;

    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (error)
            return;
        if (!entry.is_regular_file(error))
            continue;

        const std::filesystem::path& path = entry.path();
        if (path.extension() != ".tmp")
            continue;

        std::filesystem::remove(path, error);
        if (error)
            throw std::runtime_error("remove failed: " + path.string() + ", reason: " + error.message());
    }
}

SSTable::SSTable(std::filesystem::path path) : path_(std::move(path))
{
    if (!std::filesystem::exists(path_) || !std::filesystem::is_regular_file(path_))
        throw std::runtime_error("SSTable file is not a regular file: " + path_.string());

    std::ifstream input(path_, std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not open file: " + path_.string());

    const Footer footer = readFooter(input);
    recordsSize_ = footer.recordsSize;
    bloomSize_ = footer.bloomSize;
    indexSize_ = footer.indexSize;

    input.seekg(recordsSize_, std::ios::beg);
    std::vector<std::byte> bloomBytes(bloomSize_);
    input.read(reinterpret_cast<char*>(bloomBytes.data()), bloomSize_);
    bloomFilter_ = std::make_unique<BloomFilter>(BloomFilter::fromBytes(bloomBytes));
    indices_ = readSparseIndex();
}

Result SSTable::get(const std::string_view key, uint64_t readSeq, std::string& value) const
{
    if (!std::filesystem::exists(path_) || !bloomFilter_->mightContain(key))
        return Result::ABSENT;

    std::ifstream input(path_, std::ios::binary);
    if (!input.is_open())
        return Result::ABSENT;

    const auto block = getBlock(key);
    if (!block)
        return Result::ABSENT;

    const auto& [firstIndex, blockEnd] = *block;
    input.seekg(firstIndex.offset, std::ios::beg);
    uint64_t currentOffset = firstIndex.offset;
    while (currentOffset < blockEnd)
    {
        const auto record = readRecord(input);
        if (!record)
            break;

        currentOffset += serializedRecordSize(record);
        if (record->key > key)
            break;
        if (record->key < key || record->seq > readSeq)
            continue;

        if (record->type != Type::VALUE)
            return Result::TOMBSTONE;
        value = record->value;
        return Result::VALUE;
    }
    return Result::ABSENT;
}

std::vector<Index> SSTable::readSparseIndex() const
{
    std::ifstream input(path_, std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not open file: " + path_.string());

    input.seekg(recordsSize_ + bloomSize_, std::ios::beg);
    std::vector<Index> indices;
    uint64_t bytesRead = 0;
    while (bytesRead < indexSize_)
    {
        const auto index = readIndex(input);
        if (!index)
            break;

        indices.push_back(*index);
        bytesRead += serializedIndexSize(*index);
    }
    return indices;
}

std::optional<std::pair<Index, uint64_t>> SSTable::getBlock(const std::string_view key) const
{
    const auto position =
        std::upper_bound(indices_.begin(), indices_.end(), key,
                         [](const std::string_view value, const Index& index) { return index.key > value; });

    if (position == indices_.begin())
        return std::nullopt;

    const uint64_t blockEnd = position == indices_.end() ? recordsSize_ : position->offset;
    return std::make_pair(*std::prev(position), blockEnd);
}

uint64_t SSTable::addRecordToFile(const std::span<Record> records, const std::filesystem::path& path)
{
    if (records.empty())
        throw std::runtime_error("Records cannot be empty");

    uint64_t size = 0;

    BloomFilter bloomFilter(records.size(), 0.01);
    FileWriter writer(path);

    std::vector<Index> indices;
    uint64_t recordsSize = 0;
    uint64_t currentBlockSize = 0;
    for (const auto& [key, seq, type, value] : records)
    {
        if (recordsSize == 0 || currentBlockSize > kBlockSize)
        {
            currentBlockSize = 0;
            indices.emplace_back(key.size(), key, recordsSize);
        }

        const uint64_t bytesWritten = writer.add({key, seq, type, value});
        recordsSize += bytesWritten;
        currentBlockSize += bytesWritten;
        bloomFilter.add(key);
        size += bytesWritten;
    }

    const std::vector<std::byte> bloomBytes = BloomFilter::Serialize(bloomFilter);
    writeAll(writer.getFd(), bloomBytes.data(), bloomBytes.size());
    size += bloomBytes.size();

    uint64_t indexSize = 0;
    for (const Index& index : indices)
        indexSize += writeIndex(writer, index);
    size += indexSize;

    writeFooter(writer, recordsSize, bloomBytes.size(), indexSize);
    size += sizeof(recordsSize) + sizeof(size_t) + sizeof(indexSize);
    writer.finish();
    return size;
}

SSTableIterator::SSTableIterator(std::filesystem::path path)
{
    if (!std::filesystem::exists(path))
        throw std::runtime_error("Invalid path");

    input_.open(path, std::ios::binary);
    if (!input_.is_open())
        throw std::runtime_error("Failed to open SSTable file!");

    recordsSize_ = readFooter(input_).recordsSize;
    input_.seekg(0, std::ios::beg);

    if (currentPosition_ < recordsSize_)
    {
        currentRecord_ = readRecord(input_);
        currentPosition_ += serializedRecordSize(currentRecord_);
    }
}

bool SSTableIterator::valid() const { return currentRecord_.has_value(); }

const Record& SSTableIterator::current() const
{
    assert(valid());
    return *currentRecord_;
}

void SSTableIterator::advance()
{
    if (valid() && currentPosition_ < recordsSize_)
    {
        currentRecord_ = readRecord(input_);
        currentPosition_ += serializedRecordSize(currentRecord_);
        return;
    }

    currentRecord_.reset();
}
