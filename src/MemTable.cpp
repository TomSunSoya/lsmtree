#include "MemTable.h"

#include <cerrno>
#include <cassert>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace
{
std::string makeWALRecord(const std::string &key, const std::string &value, const Type valueType)
{
    return (valueType == Type::VALUE ? "P," : "D,") + std::to_string(key.size()) + "," + key + "=" + std::to_string(value.size()) + "," + value + "\n";
}
}

MemTable::MemTable(std::string_view logFilePath) : log_path(logFilePath), writer(logFilePath), current_size(0)
{
    if (!restoreFromWAL())
    {
        throw std::runtime_error("Failed to restore from wal");
    }
}

bool MemTable::put(const std::string& key, const std::string& value)
{
    try
    {
        putToWAL(key, value, Type::VALUE);
    } catch (const std::system_error& e)
    {
        // WAL write failed
        std::cerr << "putToWAL failed: " << e.code().message() << std::endl;
        return false;
    }
    if (const auto it = table.find(key); it != table.end())
    {
        assert(current_size >= it->second.value.size() + it->first.size() + sizeof(Type));
        current_size -= it->second.value.size();
        current_size -= it->first.size();
        current_size -= sizeof(Type);
    }

    table[key].value = value;
    table[key].type = Type::VALUE;
    current_size += value.size();
    current_size += key.size();
    current_size += sizeof(Type);
    return true;
}

Result MemTable::get(const std::string_view key, std::string &value) const
{
    const auto it = table.find(key);
    if (it == table.end())
        return Result::ABSENT;
    if (it->second.type == Type::TOMBSTONE)
        return Result::TOMBSTONE;
    value = it->second.value;
    return Result::VALUE;
}

bool MemTable::remove(const std::string &key)
{
    try
    {
        putToWAL(key, "", Type::TOMBSTONE);
    } catch (const std::system_error& e)
    {
        // WAL write failed
        std::cerr << "putToWAL failed: " << e.code().message() << std::endl;
        return false;
    }
    if (const auto it = table.find(key); it != table.end())
    {
        it->second.type = Type::TOMBSTONE;
        current_size -= it->second.value.size();
        it->second.value.clear();
    } else
    {
        table[key].type = Type::TOMBSTONE;
        table[key].value = "";
        current_size += sizeof(Type);
        current_size += key.size();
    }
    return true;
}

size_t MemTable::size_bytes() const
{
    return current_size;
}

MemTable::FileWriter::FileWriter(const std::string_view logPath) : path(logPath), poisoned(false)
{
    if (const auto parentPath = path.parent_path(); !parentPath.empty())
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(parentPath, ec);
        if (ec)
        {
            std::cerr << "Create Directory failed: " << ec.message() << std::endl;
            throw std::runtime_error("Failed to create directory");
        }
    }

    fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0664);
    if (fd < 0)
        throw std::runtime_error("Failed to open file for writing");
}

void MemTable::FileWriter::write(const std::string &record)
{
    if (poisoned)
        throw std::runtime_error("File is poisoned");

    if (const ssize_t bytesWritten = ::write(fd, record.c_str(), record.size()); bytesWritten < 0 || static_cast<size_t>(bytesWritten) != record.size())
    {
        poisoned = true;
        const int err = errno;
        throw std::system_error(err, std::system_category(), "write failed");
    }

    if (::fsync(fd))
    {
        poisoned = true;
        const int err = errno;
        throw std::system_error(err, std::system_category(), "fsync failed");
    }
}

int MemTable::FileWriter::truncate(const size_t offset)
{
    if (::ftruncate(fd, static_cast<off_t>(offset)) == 0)
        return 0;

    return errno;
}

void MemTable::putToWAL(const std::string& key, const std::string& value, const Type type)
{
    writer.write(makeWALRecord(key, value, type));
}

bool MemTable::restoreFromWAL()
{
    std::ifstream input(log_path);
    if (!input)
    {
        return false;
    }

    std::stringstream contentBuffer;
    contentBuffer << input.rdbuf();

    const std::string content(contentBuffer.str());
    size_t lastGoodOffset = 0;
    for (const auto walInfo = parseWALRecord(content, lastGoodOffset); const auto &[key, value] : walInfo)
        table[key] = value;

    if (lastGoodOffset == content.size())
        return true;

    if (const int err = writer.truncate(lastGoodOffset); err != 0)
    {
        std::cerr << "truncate error: " << std::generic_category().message(err) << std::endl;
        return false;
    }

    return true;
}

std::vector<std::pair<std::string, Entry>> MemTable::parseWALRecord(std::string_view content, size_t &lastGoodOffset)
{
    std::vector<std::pair<std::string, Entry>> records;
    size_t pos = 0;

    auto readLength = [&content, &pos](size_t &length)
    {
        const size_t begin = pos;
        while (pos < content.size() && std::isdigit(static_cast<unsigned char>(content[pos])))
            ++pos;

        if (begin == pos || pos == content.size())
            return false;

        try
        {
            length = std::stoull(std::string(content.substr(begin, pos - begin)));
        } catch (const std::out_of_range&)
        {
            return false;
        }
        return true;
    };

    auto consume = [&content, &pos](const char expected)
    {
        if (pos >= content.size() || content[pos] != expected)
            return false;

        ++pos;
        return true;
    };

    auto readField = [&content, &pos](const size_t length, std::string_view &field)
    {
        field = content.substr(pos, length);
        if (field.size() != length)
            return false;

        pos += length;
        return true;
    };

    while (pos < content.size())
    {
        lastGoodOffset = pos;

        std::string_view type;
        if (!readField(1, type) || !consume(','))
            return records;

        if (type != "D" && type != "P")
            return records;

        Type cur_type = type == "P" ? Type::VALUE : Type::TOMBSTONE;

        std::string_view key;
        if (size_t keyLen = 0; !readLength(keyLen) || !consume(',') || !readField(keyLen, key) || !consume('='))
            return records;

        std::string_view value;
        if (size_t valueLen = 0; !readLength(valueLen) || !consume(',') || !readField(valueLen, value) || !consume('\n'))
            return records;

        auto record = std::make_pair<std::string, Entry>(std::string(key), {cur_type, std::string(value)});

        records.emplace_back(record);
    }
    lastGoodOffset = pos;
    return records;
}
