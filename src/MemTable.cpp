#include "MemTable.h"

#include "FaultInjection.h"

#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace
{
constexpr std::string_view kWalMagic{"LWAL", 4};
constexpr uint8_t kWalVersion = 2;
constexpr size_t kWalHeaderSize = kWalMagic.size() + sizeof(kWalVersion);

// WAL v2 repeats [frame magic][payload size][batch payload][CRC32][commit magic]
// after the file header. The trailing marker distinguishes a committed corrupt
// frame from bytes left by an incomplete append.
constexpr std::string_view kWalFrameMagic{"WFRM", 4};
constexpr std::string_view kWalCommitMagic{"WCMT", 4};
constexpr size_t kWalFrameMetadataSize =
    kWalFrameMagic.size() + sizeof(uint64_t) + sizeof(uint32_t) + kWalCommitMagic.size();

size_t entrySize(const std::string_view key, const Entry& entry)
{
    return key.size() + entry.value.size() + sizeof(entry.type) + sizeof(entry.seq);
}

std::string encodeWALRecord(const std::string& key, const uint64_t seq, const std::string& value, const Type type)
{
    const std::string operation = type == Type::VALUE ? "P," : "D,";
    return operation + std::to_string(seq) + "," + std::to_string(key.size()) + "," + key + "=" +
           std::to_string(value.size()) + "," + value + "\n";
}

std::string encodeWALRecord(const Record& record)
{
    return encodeWALRecord(record.key, record.seq, record.value, record.type);
}

template <typename Value> void appendValue(std::string& output, const Value& value)
{
    output.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename Value> bool readValue(const std::string_view content, const size_t offset, Value& value)
{
    if (offset > content.size() || sizeof(value) > content.size() - offset)
        return false;

    std::memcpy(&value, content.data() + offset, sizeof(value));
    return true;
}

std::string encodeWALFrame(const std::string_view payload)
{
    std::string frame;
    frame.reserve(kWalFrameMetadataSize + payload.size());
    frame.append(kWalFrameMagic);
    appendValue(frame, static_cast<uint64_t>(payload.size()));
    frame.append(payload);
    appendValue(frame, crc32(payload));
    frame.append(kWalCommitMagic);
    return frame;
}

bool validFrameAt(const std::string_view content, const size_t frameStart)
{
    if (frameStart > content.size() || content.size() - frameStart < kWalFrameMetadataSize ||
        !content.substr(frameStart).starts_with(kWalFrameMagic))
        return false;

    uint64_t payloadSize = 0;
    const size_t payloadStart = frameStart + kWalFrameMagic.size() + sizeof(payloadSize);
    if (!readValue(content, frameStart + kWalFrameMagic.size(), payloadSize) ||
        payloadSize > content.size() - frameStart - kWalFrameMetadataSize)
        return false;

    const size_t payloadEnd = payloadStart + static_cast<size_t>(payloadSize);
    uint32_t storedChecksum = 0;
    return readValue(content, payloadEnd, storedChecksum) &&
           content.substr(payloadEnd + sizeof(storedChecksum)).starts_with(kWalCommitMagic) &&
           crc32(content.substr(payloadStart, payloadSize)) == storedChecksum;
}

bool hasValidFrameAfter(const std::string_view content, size_t searchPosition)
{
    while ((searchPosition = content.find(kWalFrameMagic, searchPosition)) != std::string_view::npos)
    {
        if (validFrameAt(content, searchPosition))
            return true;
        ++searchPosition;
    }
    return false;
}

class WALBatchParser
{
  public:
    explicit WALBatchParser(const std::string_view content) : content_(content) {}

    std::vector<std::pair<std::string, Entry>> parse()
    {
        std::vector<std::pair<std::string, Entry>> records;
        const uint64_t recordCount = readLength();
        consume(',');
        for (uint64_t recordIndex = 0; recordIndex < recordCount; ++recordIndex)
            records.push_back(readRecord());

        if (position_ != content_.size())
            throw std::runtime_error("Corrupt WAL batch length");
        return records;
    }

  private:
    std::pair<std::string, Entry> readRecord()
    {
        const std::string_view operation = readField(1);
        consume(',');
        if (operation != "D" && operation != "P")
            throw std::runtime_error("Corrupt WAL record operation");

        const uint64_t sequence = readLength();
        consume(',');
        const std::string_view key = readLengthPrefixedField('=');
        const std::string_view value = readLengthPrefixedField('\n');

        const Type type = operation == "P" ? Type::VALUE : Type::TOMBSTONE;
        return std::pair{std::string(key), Entry{type, sequence, std::string(value)}};
    }

    uint64_t readLength()
    {
        const size_t begin = position_;
        while (position_ < content_.size() && std::isdigit(static_cast<unsigned char>(content_[position_])))
            ++position_;

        if (begin == position_)
            throw std::runtime_error("Corrupt WAL record length");
        if (position_ == content_.size())
            throw std::runtime_error("Corrupt WAL record length");

        try
        {
            return std::stoull(std::string(content_.substr(begin, position_ - begin)));
        }
        catch (const std::exception&)
        {
            throw std::runtime_error("Corrupt WAL record length");
        }
    }

    void consume(const char expected)
    {
        if (position_ >= content_.size() || content_[position_] != expected)
            throw std::runtime_error("Corrupt WAL record delimiter");

        ++position_;
    }

    std::string_view readField(const uint64_t length)
    {
        if (length > content_.size() - position_)
            throw std::runtime_error("Corrupt WAL record length");

        const std::string_view field = content_.substr(position_, static_cast<size_t>(length));
        position_ += static_cast<size_t>(length);
        return field;
    }

    std::string_view readLengthPrefixedField(const char terminator)
    {
        const uint64_t length = readLength();
        consume(',');
        const std::string_view field = readField(length);
        consume(terminator);
        return field;
    }

    std::string_view content_;
    size_t position_ = 0;
};

class WALParser
{
  public:
    WALParser(const std::string_view content, const size_t startPosition) : content_(content), position_(startPosition)
    {
    }

    std::vector<std::pair<std::string, Entry>> parse(size_t& lastGoodOffset)
    {
        std::vector<std::pair<std::string, Entry>> records;
        while (position_ < content_.size())
        {
            const size_t frameStart = position_;
            if (content_.size() - frameStart < kWalFrameMetadataSize ||
                !content_.substr(frameStart).starts_with(kWalFrameMagic))
                return incompleteTailOrCorruption(frameStart, std::move(records));

            uint64_t payloadSize = 0;
            if (!readValue(content_, frameStart + kWalFrameMagic.size(), payloadSize) ||
                payloadSize > content_.size() - frameStart - kWalFrameMetadataSize)
                return incompleteTailOrCorruption(frameStart, std::move(records));

            const size_t payloadStart = frameStart + kWalFrameMagic.size() + sizeof(payloadSize);
            const size_t payloadEnd = payloadStart + static_cast<size_t>(payloadSize);
            uint32_t storedChecksum = 0;
            if (!readValue(content_, payloadEnd, storedChecksum) ||
                !content_.substr(payloadEnd + sizeof(storedChecksum)).starts_with(kWalCommitMagic))
                return incompleteTailOrCorruption(frameStart, std::move(records));

            const std::string_view payload = content_.substr(payloadStart, static_cast<size_t>(payloadSize));
            if (crc32(payload) != storedChecksum)
                throw std::runtime_error("Corrupt WAL frame checksum");

            auto batchRecords = WALBatchParser(payload).parse();
            records.insert(records.end(), std::make_move_iterator(batchRecords.begin()),
                           std::make_move_iterator(batchRecords.end()));

            position_ = payloadEnd + sizeof(storedChecksum) + kWalCommitMagic.size();
            lastGoodOffset = position_;
        }
        return records;
    }

  private:
    std::vector<std::pair<std::string, Entry>>
    incompleteTailOrCorruption(const size_t frameStart, std::vector<std::pair<std::string, Entry>> records) const
    {
        const bool hasCommitMarker =
            content_.size() - frameStart >= kWalFrameMetadataSize && content_.ends_with(kWalCommitMagic);
        if (hasCommitMarker || hasValidFrameAfter(content_, frameStart + 1))
            throw std::runtime_error("Corrupt WAL frame before a committed batch");
        return records;
    }

    std::string_view content_;
    size_t position_;
};
} // namespace

MemTable::MemTable(const std::string_view logFilePath)
    : logPath_(logFilePath), walWriter_(logFilePath), currentSizeBytes_(0), currentSeq_(0)
{
    if (!restoreFromWAL())
        throw std::runtime_error("Failed to restore from wal");
}

bool MemTable::put(const std::string& key, const uint64_t seq, const std::string& value)
{
    return applyBatch({Record{key, seq, Type::VALUE, value}});
}

Result MemTable::get(const std::string_view key, const uint64_t readSeq, std::string& value) const
{
    const Key currentKey{std::string(key), readSeq};
    const auto entry = table_.lower_bound(currentKey);
    if (entry == table_.end() || entry->first.key != key)
        return Result::ABSENT;
    if (entry->second.type == Type::TOMBSTONE)
        return Result::TOMBSTONE;

    value = entry->second.value;
    return Result::VALUE;
}

bool MemTable::remove(const std::string& key, const uint64_t seq)
{
    return applyBatch({Record{key, seq, Type::TOMBSTONE, {}}});
}

bool MemTable::applyBatch(const std::vector<Record>& ops)
{
    if (ops.empty())
        return true;
    std::string payload = std::to_string(ops.size()) + ",";
    for (const auto& record : ops)
        payload += encodeWALRecord(record);

    try
    {
        walWriter_.write(encodeWALFrame(payload));
    }
    catch (const std::system_error& error)
    {
        std::cerr << "Write batch failed: " << error.code().message() << std::endl;
        return false;
    }
    catch (const std::runtime_error& error)
    {
        std::cerr << "WAL file has been poisoned: " << error.what() << std::endl;
        return false;
    }

    for (const auto& [key, seq, type, value] : ops)
        applyToMemory(key, Entry{type, seq, value});
    return true;
}

void MemTable::applyToMemory(const std::string& key, const Entry& entry)
{
    Key mapKey{key, entry.seq};
    if (const auto existing = table_.find(mapKey); existing != table_.end())
        currentSizeBytes_ -= entrySize(key, existing->second);

    currentSizeBytes_ += entrySize(key, entry);
    table_.insert_or_assign(std::move(mapKey), entry);
    currentSeq_ = std::max(entry.seq, currentSeq_);
}

size_t MemTable::size() const { return table_.size(); }

size_t MemTable::size_bytes() const { return currentSizeBytes_; }

uint64_t MemTable::getMaxWALSeq() const { return currentSeq_; }

MemTable::WALFileWriter::WALFileWriter(const std::string_view logPath) : path_(logPath)
{
    std::filesystem::path parentPath = path_.parent_path();
    if (parentPath.empty())
        parentPath = ".";
    else
    {
        std::error_code error;
        std::filesystem::create_directories(parentPath, error);
        if (error)
        {
            std::cerr << "Create Directory failed: " << error.message() << std::endl;
            throw std::runtime_error("Failed to create directory");
        }
    }
    fileDescriptor_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0644);
    if (fileDescriptor_ < 0)
    {
        if (errno != EEXIST)
            throw std::system_error(errno, std::system_category(), "open failed");
        fileDescriptor_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0664);
        if (fileDescriptor_ < 0)
            throw std::system_error(errno, std::system_category(), "open existing WAL failed");
    }
    else
    {
        try
        {
            writeAll(fileDescriptor_, kWalMagic.data(), kWalMagic.size());
            writeAll(fileDescriptor_, &kWalVersion, sizeof(kWalVersion));
            if (fault::fsync(fileDescriptor_))
                throw std::system_error(errno, std::system_category(), "fsync WAL head failed");

            const int directoryDescriptor = ::open(parentPath.c_str(), O_RDONLY);
            if (directoryDescriptor < 0)
                throw std::system_error(errno, std::system_category(), "open WAL directory failed");

            FdGuard directory(directoryDescriptor);
            if (fault::fsync(directory.get()))
                throw std::system_error(errno, std::system_category(), "fsync WAL directory failed");
        }
        catch (...)
        {
            ::close(fileDescriptor_);
            fileDescriptor_ = -1;
            throw;
        }
    }
}

MemTable::WALFileWriter::~WALFileWriter()
{
    if (fileDescriptor_ >= 0)
        ::close(fileDescriptor_);
}

void MemTable::WALFileWriter::write(const std::string& record)
{
    if (poisoned_)
        throw std::runtime_error("File is poisoned");

    if (const ssize_t bytesWritten = fault::write(fileDescriptor_, record.c_str(), record.size());
        bytesWritten < 0 || static_cast<size_t>(bytesWritten) != record.size())
    {
        poisoned_ = true;
        const int error = errno;
        throw std::system_error(error, std::system_category(), "write failed");
    }

    if (fault::fsync(fileDescriptor_))
    {
        poisoned_ = true;
        const int error = errno;
        throw std::system_error(error, std::system_category(), "fsync failed");
    }
}

int MemTable::WALFileWriter::truncate(const size_t offset)
{
    if (::ftruncate(fileDescriptor_, static_cast<off_t>(offset)) == 0)
        return 0;
    return errno;
}

bool MemTable::restoreFromWAL()
{
    std::ifstream input(logPath_, std::ios::binary);
    if (!input)
        return false;

    std::stringstream contentBuffer;
    contentBuffer << input.rdbuf();
    const std::string content = contentBuffer.str();
    if (content.size() < kWalHeaderSize)
        throw std::runtime_error("Invalid WAL file format");

    if (content.compare(0, kWalMagic.size(), kWalMagic))
        throw std::runtime_error("Invalid WAL file format");

    if (const auto version = static_cast<uint8_t>(content[kWalMagic.size()]); version != kWalVersion)
        throw std::runtime_error("Invalid WAL file format");

    size_t lastGoodOffset = kWalHeaderSize;
    for (const auto& [key, entry] : parseWALRecords(content, lastGoodOffset))
        applyToMemory(key, entry);

    if (lastGoodOffset == content.size())
        return true;

    if (const int error = walWriter_.truncate(lastGoodOffset); error != 0)
    {
        std::cerr << "truncate error: " << std::generic_category().message(error) << std::endl;
        return false;
    }
    return true;
}

std::vector<std::pair<std::string, Entry>> MemTable::parseWALRecords(const std::string_view content,
                                                                     size_t& lastGoodOffset)
{
    return WALParser(content, kWalHeaderSize).parse(lastGoodOffset);
}

MemTableIterator::MemTableIterator(const MemTable& memTable)
    : currentIterator_(memTable.begin()), endIterator_(memTable.end())
{
    refreshCurrentRecord();
}

bool MemTableIterator::valid() const { return currentRecord_.has_value(); }

const Record& MemTableIterator::current() const
{
    assert(valid());
    return *currentRecord_;
}

void MemTableIterator::advance()
{
    ++currentIterator_;
    refreshCurrentRecord();
}

void MemTableIterator::refreshCurrentRecord()
{
    if (currentIterator_ == endIterator_)
    {
        currentRecord_.reset();
        return;
    }

    currentRecord_ = Record{
        currentIterator_->first.key,
        currentIterator_->second.seq,
        currentIterator_->second.type,
        currentIterator_->second.value,
    };
}
