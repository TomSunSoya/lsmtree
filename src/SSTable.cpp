#include "SSTable.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "MemTable.h"

namespace
{
constexpr uint64_t kIndexMetadataSize = sizeof(uint32_t) + sizeof(uint64_t);
constexpr size_t kFooterSize = 3 * sizeof(uint64_t);
constexpr double kBloomFalsePositiveProbability = 0.01;

struct Footer
{
    uint64_t recordsSize;
    uint64_t bloomSize;
    uint64_t indexSize;
};

Footer readFooter(std::ifstream& input)
{
    input.seekg(-static_cast<std::streamoff>(kFooterSize), std::ios::end);
    if (!input)
        throw std::runtime_error("Corrupt SSTable footer");

    std::array<std::byte, kFooterSize> buffer{};
    input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
    if (!input)
        throw std::runtime_error("Corrupt SSTable footer");

    Footer footer{};
    std::memcpy(&footer.recordsSize, buffer.data(), sizeof(footer.recordsSize));
    std::memcpy(&footer.bloomSize, buffer.data() + sizeof(footer.recordsSize), sizeof(footer.bloomSize));
    std::memcpy(&footer.indexSize, buffer.data() + sizeof(footer.recordsSize) + sizeof(footer.bloomSize),
                sizeof(footer.indexSize));
    return footer;
}

void validateFooter(const Footer& footer, const uint64_t fileSize)
{
    if (fileSize < kFooterSize)
        throw std::runtime_error("Corrupt SSTable size");

    uint64_t remaining = fileSize - kFooterSize;
    if (footer.recordsSize == 0 || footer.recordsSize > remaining)
        throw std::runtime_error("Corrupt SSTable records size");
    remaining -= footer.recordsSize;
    if (footer.bloomSize > remaining)
        throw std::runtime_error("Corrupt SSTable Bloom filter size");
    remaining -= footer.bloomSize;
    if (footer.indexSize != remaining)
        throw std::runtime_error("Corrupt SSTable index size");
}

Record readRecord(std::ifstream& input, const uint64_t availableBytes)
{
    if (availableBytes < kEncodedRecordHeaderSize + kEncodedRecordChecksumSize)
        throw std::runtime_error("Corrupt SSTable record header");

    std::array<char, kEncodedRecordHeaderSize> header{};
    if (!input.read(header.data(), header.size()))
        throw std::runtime_error("Corrupt SSTable record header");

    size_t headerOffset = 0;
    const auto readHeaderValue = [&header, &headerOffset](auto& value)
    {
        std::memcpy(&value, header.data() + headerOffset, sizeof(value));
        headerOffset += sizeof(value);
    };

    uint8_t type = 0;
    uint64_t sequence = 0;
    uint32_t keySize = 0;
    uint32_t valueSize = 0;
    readHeaderValue(type);
    readHeaderValue(sequence);
    readHeaderValue(keySize);
    readHeaderValue(valueSize);

    if (type > static_cast<uint8_t>(Type::TOMBSTONE))
        throw std::runtime_error("Corrupt SSTable record type");

    const uint64_t payloadBytes = availableBytes - kEncodedRecordHeaderSize - kEncodedRecordChecksumSize;
    if (keySize > payloadBytes || valueSize > payloadBytes - keySize)
        throw std::runtime_error("Corrupt SSTable record length");

    std::string key(keySize, '\0');
    std::string value(valueSize, '\0');
    if (!input.read(key.data(), keySize))
        throw std::runtime_error("Corrupt SSTable record key");
    if (!input.read(value.data(), valueSize))
        throw std::runtime_error("Corrupt SSTable record value");

    uint32_t storedChecksum = 0;
    if (!input.read(reinterpret_cast<char*>(&storedChecksum), sizeof(storedChecksum)))
        throw std::runtime_error("Corrupt SSTable record checksum");

    std::string encoded(header.data(), header.size());
    encoded.append(key);
    encoded.append(value);
    if (crc32(encoded) != storedChecksum)
        throw std::runtime_error("Corrupt SSTable record checksum");

    return Record{std::move(key), sequence, static_cast<Type>(type), std::move(value)};
}

Index readIndex(std::ifstream& input, const uint64_t availableBytes)
{
    if (availableBytes < kIndexMetadataSize)
        throw std::runtime_error("Corrupt SSTable index header");

    uint32_t keySize = 0;
    if (!input.read(reinterpret_cast<char*>(&keySize), sizeof(keySize)))
        throw std::runtime_error("Corrupt SSTable index key size");
    if (keySize > availableBytes - kIndexMetadataSize)
        throw std::runtime_error("Corrupt SSTable index length");

    std::string key(keySize, '\0');
    if (!input.read(key.data(), keySize))
        throw std::runtime_error("Corrupt SSTable index key");

    uint64_t offset = 0;
    if (!input.read(reinterpret_cast<char*>(&offset), sizeof(offset)))
        throw std::runtime_error("Corrupt SSTable index offset");

    return Index{keySize, std::move(key), offset};
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
    validateFooter(footer, std::filesystem::file_size(path_));
    recordsSize_ = footer.recordsSize;
    bloomSize_ = footer.bloomSize;
    indexSize_ = footer.indexSize;

    input.seekg(recordsSize_, std::ios::beg);
    if (!input)
        throw std::runtime_error("Corrupt SSTable Bloom filter offset");
    std::vector<std::byte> bloomBytes(bloomSize_);
    if (!input.read(reinterpret_cast<char*>(bloomBytes.data()), static_cast<std::streamsize>(bloomSize_)))
        throw std::runtime_error("Corrupt SSTable Bloom filter");
    bloomFilter_ = std::make_unique<BloomFilter>(BloomFilter::fromBytes(bloomBytes));
    indices_ = readSparseIndex();
    if (indices_.empty())
        throw std::runtime_error("Corrupt SSTable sparse index");
}

Result SSTable::get(const std::string_view key, uint64_t readSeq, std::string& value) const
{
    if (!std::filesystem::exists(path_))
        throw std::runtime_error("SSTable file disappeared: " + path_.string());
    if (!bloomFilter_->mightContain(key))
        return Result::ABSENT;

    std::ifstream input(path_, std::ios::binary);
    if (!input.is_open())
        throw std::runtime_error("Could not open file: " + path_.string());

    const auto block = getBlock(key);
    if (!block)
        return Result::ABSENT;

    const auto& [firstIndex, blockEnd] = *block;
    input.seekg(firstIndex.offset, std::ios::beg);
    uint64_t currentOffset = firstIndex.offset;
    while (currentOffset < blockEnd)
    {
        const Record record = readRecord(input, blockEnd - currentOffset);
        currentOffset += encodedRecordSize(record);
        if (record.key > key)
            break;
        if (record.key < key || record.seq > readSeq)
            continue;

        if (record.type != Type::VALUE)
            return Result::TOMBSTONE;
        value = record.value;
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
        Index index = readIndex(input, indexSize_ - bytesRead);
        if (index.offset >= recordsSize_)
            throw std::runtime_error("Corrupt SSTable index offset");
        if (!indices.empty() && (index.offset <= indices.back().offset || index.key < indices.back().key))
            throw std::runtime_error("Corrupt SSTable index order");

        bytesRead += serializedIndexSize(index);
        indices.push_back(std::move(index));
    }
    if (!indices.empty() && indices.front().offset != 0)
        throw std::runtime_error("Corrupt SSTable first index offset");
    return indices;
}

std::optional<std::pair<Index, uint64_t>> SSTable::getBlock(const std::string_view key) const
{
    if (indices_.empty())
        return std::nullopt;

    auto position = std::lower_bound(indices_.begin(), indices_.end(), key,
                                     [](const Index& index, std::string_view value) { return index.key < value; });

    if (position == indices_.begin() && position->key > key)
        return std::nullopt;

    if (position == indices_.end() || (position != indices_.begin() && position->key >= key))
        position = std::prev(position);
    auto nextBlock = position;
    while (nextBlock != indices_.end() && nextBlock->key <= key)
        ++nextBlock;

    const uint64_t blockEnd = nextBlock == indices_.end() ? recordsSize_ : nextBlock->offset;
    return std::make_pair(*position, blockEnd);
}

uint64_t SSTable::addRecordToFile(const std::span<Record> records, const std::filesystem::path& path)
{
    if (records.empty())
        throw std::runtime_error("Records cannot be empty");

    uint64_t fileSize = 0;

    BloomFilter bloomFilter = BloomFilter::forEntries(records.size(), kBloomFalsePositiveProbability);
    FileWriter writer(path);

    std::vector<Index> indices;
    uint64_t recordsSize = 0;
    uint64_t currentBlockSize = 0;
    for (const Record& record : records)
    {
        if (recordsSize == 0 || currentBlockSize > kBlockSize)
        {
            currentBlockSize = 0;
            indices.emplace_back(record.key.size(), record.key, recordsSize);
        }

        const uint64_t bytesWritten = writer.add(record);
        recordsSize += bytesWritten;
        currentBlockSize += bytesWritten;
        bloomFilter.add(record.key);
        fileSize += bytesWritten;
    }

    const std::vector<std::byte> bloomBytes = BloomFilter::Serialize(bloomFilter);
    writeAll(writer.getFd(), bloomBytes.data(), bloomBytes.size());
    fileSize += bloomBytes.size();

    uint64_t indexSize = 0;
    for (const Index& index : indices)
        indexSize += writeIndex(writer, index);
    fileSize += indexSize;

    writeFooter(writer, recordsSize, bloomBytes.size(), indexSize);
    fileSize += kFooterSize;
    writer.finish();
    return fileSize;
}

SSTableIterator::SSTableIterator(std::filesystem::path path)
{
    if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path))
        throw std::runtime_error("Invalid path");

    input_.open(path, std::ios::binary);
    if (!input_.is_open())
        throw std::runtime_error("Failed to open SSTable file!");

    const Footer footer = readFooter(input_);
    validateFooter(footer, std::filesystem::file_size(path));
    recordsSize_ = footer.recordsSize;
    input_.seekg(0, std::ios::beg);
    if (!input_)
        throw std::runtime_error("Corrupt SSTable records offset");

    loadNextRecord();
}

bool SSTableIterator::valid() const { return currentRecord_.has_value(); }

const Record& SSTableIterator::current() const
{
    assert(valid());
    return *currentRecord_;
}

void SSTableIterator::advance() { loadNextRecord(); }

void SSTableIterator::loadNextRecord()
{
    if (currentPosition_ >= recordsSize_)
    {
        currentRecord_.reset();
        return;
    }

    currentRecord_ = readRecord(input_, recordsSize_ - currentPosition_);
    currentPosition_ += encodedRecordSize(*currentRecord_);
}
