#include "MemTable.h"

#include <cassert>
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
constexpr std::string_view MAGIC{"LWAL", 4};
constexpr uint8_t VERSION = 1;
constexpr size_t kMagicSize = 4;
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
    explicit WALParser(const std::string_view content) : content_(content) {}

    std::vector<std::pair<std::string, Entry>> parse(size_t& lastGoodOffset)
    {
        std::vector<std::pair<std::string, Entry>> records;
        while (position_ < content_.size())
        {
            lastGoodOffset = position_;
            uint64_t batchNumber = 0;
            if (!readLength(batchNumber) || !consume(','))
                return records;

            std::vector<std::pair<std::string, Entry>> batchRecords;
            for (uint64_t i = 0; i < batchNumber; ++i)
            {
                std::string_view operation;
                if (!readField(1, operation) || !consume(','))
                    return records;
                if (operation != "D" && operation != "P")
                    return records;

                uint64_t seq = 0;
                if (!readLength(seq) || !consume(','))
                    return records;

                std::string_view key;
                if (!readLengthPrefixedField(key, '='))
                    return records;

                std::string_view value;
                if (!readLengthPrefixedField(value, '\n'))
                    return records;

                const Type type = operation == "P" ? Type::VALUE : Type::TOMBSTONE;
                batchRecords.emplace_back(std::string(key), Entry{type, seq, std::string(value)});
            }
            records.insert(records.end(), batchRecords.begin(), batchRecords.end());
        }

        lastGoodOffset = position_;
        return records;
    }

  private:
    bool readLength(uint64_t& length)
    {
        const size_t begin = position_;
        while (position_ < content_.size() && std::isdigit(static_cast<unsigned char>(content_[position_])))
            ++position_;

        if (begin == position_ || position_ == content_.size())
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
        if (position_ >= content_.size() || content_[position_] != expected)
            return false;

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
    size_t position_ = kMagicSize + 1;
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
    const auto it = table_.lower_bound(currentKey);
    if (it == table_.end() || it->first.key != key)
        return Result::ABSENT;
    if (it->second.type == Type::TOMBSTONE)
        return Result::TOMBSTONE;

    value = it->second.value;
    return Result::VALUE;
}

bool MemTable::remove(const std::string& key, const uint64_t seq)
{
    return applyBatch({Record{key, seq, Type::TOMBSTONE}});
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
    catch (const std::runtime_error& e)
    {
        std::cerr << "WAL file has been poisoned: " << e.what() << std::endl;
        return false;
    }

    for (const auto& [key, seq, type, value] : ops)
    {
        table_[Key{key, seq}] = Entry{type, seq, value};
        currentSizeBytes_ += value.size();
        currentSizeBytes_ += key.size();
        currentSizeBytes_ += sizeof(Type);
        currentSizeBytes_ += sizeof(seq);
    }
    return true;
}

size_t MemTable::size() const { return table_.size(); }

size_t MemTable::size_bytes() const { return currentSizeBytes_; }

uint64_t MemTable::getMaxWALSeq() const { return currentSeq_; }

MemTable::WALFileWriter::WALFileWriter(const std::string_view logPath) : path_(logPath)
{
    if (const auto parentPath = path_.parent_path(); !parentPath.empty())
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
    }
    else
    {
        writeAll(fileDescriptor_, MAGIC.data(), MAGIC.size());
        writeAll(fileDescriptor_, &VERSION, sizeof(VERSION));
        if (::fsync(fileDescriptor_))
            throw std::system_error(errno, std::system_category(), "fsync WAL head failed");
    }
}

MemTable::WALFileWriter::~WALFileWriter() { ::close(fileDescriptor_); }

void MemTable::WALFileWriter::write(const std::string& record)
{
    if (poisoned_)
        throw std::runtime_error("File is poisoned");

    if (const ssize_t bytesWritten = ::write(fileDescriptor_, record.c_str(), record.size());
        bytesWritten < 0 || static_cast<size_t>(bytesWritten) != record.size())
    {
        poisoned_ = true;
        const int error = errno;
        throw std::system_error(error, std::system_category(), "write failed");
    }

    if (::fsync(fileDescriptor_))
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
    if (content.size() < 5)
        throw std::runtime_error("Invalid WAL file format");

    if (content.compare(0, kMagicSize, MAGIC))
        throw std::runtime_error("Invalid WAL file format");

    if (const auto version = static_cast<uint8_t>(content[kMagicSize]); version != VERSION)
        throw std::runtime_error("Invalid WAL file format");

    size_t lastGoodOffset = kMagicSize + 1;
    for (const auto& [key, entry] : parseWALRecords(content, lastGoodOffset))
    {
        table_.emplace(Key{key, entry.seq}, entry);
        currentSeq_ = std::max(entry.seq, currentSeq_);
    }

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
    return WALParser(content).parse(lastGoodOffset);
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
