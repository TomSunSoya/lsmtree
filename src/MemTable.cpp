#include "MemTable.h"

#include "FaultInjection.h"

#include <cassert>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace
{
constexpr std::string_view kWalMagic{"LWAL", 4};
constexpr uint8_t kWalVersion = 1;
constexpr size_t kWalHeaderSize = kWalMagic.size() + sizeof(kWalVersion);

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
            lastGoodOffset = position_;
            uint64_t recordCount = 0;
            if (!readLength(recordCount) || !consume(','))
                return records;

            std::vector<std::pair<std::string, Entry>> batchRecords;
            for (uint64_t recordIndex = 0; recordIndex < recordCount; ++recordIndex)
            {
                const auto record = readRecord();
                if (!record)
                    return records;
                batchRecords.push_back(*record);
            }
            records.insert(records.end(), batchRecords.begin(), batchRecords.end());
        }

        lastGoodOffset = position_;
        return records;
    }

  private:
    std::optional<std::pair<std::string, Entry>> readRecord()
    {
        std::string_view operation;
        if (!readField(1, operation) || !consume(','))
            return std::nullopt;
        if (operation != "D" && operation != "P")
            throw std::runtime_error("Corrupt WAL record operation");

        uint64_t sequence = 0;
        if (!readLength(sequence) || !consume(','))
            return std::nullopt;

        std::string_view key;
        if (!readLengthPrefixedField(key, '='))
            return std::nullopt;

        std::string_view value;
        if (!readLengthPrefixedField(value, '\n'))
            return std::nullopt;

        const Type type = operation == "P" ? Type::VALUE : Type::TOMBSTONE;
        return std::pair{std::string(key), Entry{type, sequence, std::string(value)}};
    }

    bool readLength(uint64_t& length)
    {
        const size_t begin = position_;
        while (position_ < content_.size() && std::isdigit(static_cast<unsigned char>(content_[position_])))
            ++position_;

        if (begin == position_)
        {
            if (position_ != content_.size())
                throw std::runtime_error("Corrupt WAL record length");
            return false;
        }
        if (position_ == content_.size())
            return false;

        try
        {
            length = std::stoull(std::string(content_.substr(begin, position_ - begin)));
        }
        catch (const std::out_of_range&)
        {
            return false;
        }
        return true;
    }

    bool consume(const char expected)
    {
        if (position_ >= content_.size())
            return false;
        if (content_[position_] != expected)
            throw std::runtime_error("Corrupt WAL record delimiter");

        ++position_;
        return true;
    }

    bool readField(const size_t length, std::string_view& field)
    {
        field = content_.substr(position_, length);
        if (field.size() != length)
            return false;

        position_ += length;
        return true;
    }

    bool readLengthPrefixedField(std::string_view& field, const char terminator)
    {
        uint64_t length = 0;
        return readLength(length) && consume(',') && readField(length, field) && consume(terminator);
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
    std::string content = std::to_string(ops.size()) + ",";
    for (const auto& record : ops)
        content += encodeWALRecord(record);

    try
    {
        walWriter_.write(content);
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
