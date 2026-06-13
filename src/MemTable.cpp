#include "MemTable.h"

#include <cerrno>
#include <cctype>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace
{
std::string makeWALRecord(const std::string &key, const std::string &value)
{
    return std::to_string(key.size()) + "," + key + "=" + std::to_string(value.size()) + "," + value + "\n";
}
}

MemTable::MemTable(std::string_view logFilePath) : log_path(logFilePath), writer(logFilePath)
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
        putToWAL(key, value);
    } catch (const std::system_error& e)
    {
        // WAL write failed
        std::cerr << "putToWAL failed: " << e.code().message() << std::endl;
        return false;
    }
    table[key] = value;
    return true;
}

bool MemTable::get(const std::string_view key, std::string &value) const
{
    const auto it = table.find(key);
    if (it == table.end())
        return false;
    value = it->second;
    return true;
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

    const ssize_t bytesWritten = ::write(fd, record.c_str(), record.size());
    if (bytesWritten < 0 || static_cast<size_t>(bytesWritten) != record.size())
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

void MemTable::putToWAL(const std::string& key, const std::string& value)
{
    writer.write(makeWALRecord(key, value));
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

std::vector<std::pair<std::string, std::string>> MemTable::parseWALRecord(std::string_view content, size_t &lastGoodOffset)
{
    std::vector<std::pair<std::string, std::string>> records;
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
        std::string_view key;
        if (size_t keyLen = 0; !readLength(keyLen) || !consume(',') || !readField(keyLen, key) || !consume('='))
            return records;

        std::string_view value;
        if (size_t valueLen = 0; !readLength(valueLen) || !consume(',') || !readField(valueLen, value) || !consume('\n'))
            return records;

        records.emplace_back(key, value);
    }
    lastGoodOffset = pos;
    return records;
}
