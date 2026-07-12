#include "MemTable.h"

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
std::string encodeWALRecord(const std::string& key, const std::string& value, const Type type)
{
    const std::string operation = type == Type::VALUE ? "P," : "D,";
    return operation + std::to_string(key.size()) + "," + key + "=" + std::to_string(value.size()) + "," + value + "\n";
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

            std::string_view operation;
            if (!readField(1, operation) || !consume(','))
                return records;
            if (operation != "D" && operation != "P")
                return records;

            std::string_view key;
            if (!readLengthPrefixedField(key, '='))
                return records;

            std::string_view value;
            if (!readLengthPrefixedField(value, '\n'))
                return records;

            const Type type = operation == "P" ? Type::VALUE : Type::TOMBSTONE;
            records.emplace_back(std::string(key), Entry{type, std::string(value)});
        }

        lastGoodOffset = position_;
        return records;
    }

  private:
    bool readLength(size_t& length)
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
        size_t length = 0;
        return readLength(length) && consume(',') && readField(length, field) && consume(terminator);
    }

    std::string_view content_;
    size_t position_ = 0;
};
} // namespace

MemTable::MemTable(const std::string_view logFilePath)
    : logPath_(logFilePath), walWriter_(logFilePath), currentSizeBytes_(0)
{
    if (!restoreFromWAL())
        throw std::runtime_error("Failed to restore from wal");
}

bool MemTable::put(const std::string& key, const std::string& value)
{
    if (!appendToWAL(key, value, Type::VALUE))
        return false;

    if (const auto existing = table_.find(key); existing != table_.end())
    {
        assert(currentSizeBytes_ >= existing->second.value.size() + existing->first.size() + sizeof(Type));
        currentSizeBytes_ -= existing->second.value.size();
        currentSizeBytes_ -= existing->first.size();
        currentSizeBytes_ -= sizeof(Type);
    }

    table_[key] = {Type::VALUE, value};
    currentSizeBytes_ += value.size();
    currentSizeBytes_ += key.size();
    currentSizeBytes_ += sizeof(Type);
    return true;
}

Result MemTable::get(const std::string_view key, std::string& value) const
{
    const auto entry = table_.find(key);
    if (entry == table_.end())
        return Result::ABSENT;
    if (entry->second.type == Type::TOMBSTONE)
        return Result::TOMBSTONE;

    value = entry->second.value;
    return Result::VALUE;
}

bool MemTable::remove(const std::string& key)
{
    if (!appendToWAL(key, "", Type::TOMBSTONE))
        return false;

    if (const auto existing = table_.find(key); existing != table_.end())
    {
        existing->second.type = Type::TOMBSTONE;
        currentSizeBytes_ -= existing->second.value.size();
        existing->second.value.clear();
    }
    else
    {
        table_[key] = {Type::TOMBSTONE, ""};
        currentSizeBytes_ += sizeof(Type);
        currentSizeBytes_ += key.size();
    }
    return true;
}

size_t MemTable::size() const { return table_.size(); }

size_t MemTable::size_bytes() const { return currentSizeBytes_; }

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

    fileDescriptor_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0664);
    if (fileDescriptor_ < 0)
        throw std::runtime_error("Failed to open file for writing");
}

MemTable::WALFileWriter::~WALFileWriter() { ::close(fileDescriptor_); }

void MemTable::WALFileWriter::write(const std::string& record)
{
    if (poisoned_)
        throw std::runtime_error("File is poisoned");

    const ssize_t bytesWritten = ::write(fileDescriptor_, record.c_str(), record.size());
    if (bytesWritten < 0 || static_cast<size_t>(bytesWritten) != record.size())
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

bool MemTable::appendToWAL(const std::string& key, const std::string& value, const Type type)
{
    try
    {
        walWriter_.write(encodeWALRecord(key, value, type));
        return true;
    }
    catch (const std::system_error& error)
    {
        std::cerr << "putToWAL failed: " << error.code().message() << std::endl;
        return false;
    }
}

bool MemTable::restoreFromWAL()
{
    std::ifstream input(logPath_);
    if (!input)
        return false;

    std::stringstream contentBuffer;
    contentBuffer << input.rdbuf();
    const std::string content = contentBuffer.str();

    size_t lastGoodOffset = 0;
    for (const auto& [key, entry] : parseWALRecords(content, lastGoodOffset))
        table_[key] = entry;

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
        currentIterator_->first,
        currentIterator_->second.type,
        currentIterator_->second.value,
    };
}
