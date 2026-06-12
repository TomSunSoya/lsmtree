#include "MemTable.h"

#include <cctype>
#include <filesystem>
#include <iostream>
#include <vector>

MemTable::MemTable(std::string_view logFilePath) : log_path(logFilePath)
{
    if (auto dir_path = log_path.parent_path(); !dir_path.empty())
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(dir_path, ec);
        if (ec)
        {
            std::cerr << "Create Directory failed: " << ec.message() << std::endl;
            throw std::runtime_error("Failed to create directory");
        }
    }
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.open(log_path, std::ios::out | std::ios::app);
}

bool MemTable::put(const std::string& key, const std::string& value)
{
    try
    {
        putToWAL(key, value);
    } catch (std::ios_base::failure& e)
    {
        // WAL write failed
        std::cerr << "putToWAL failed: " << e.what() << std::endl;
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

void MemTable::putToWAL(const std::string& key, const std::string& value)
{
    const std::string line = std::to_string(key.size()) + "," + key + "=" + std::to_string(value.size()) + "," +  value;
    out << line << std::endl;
}

std::vector<std::pair<std::string, std::string>> MemTable::parseWALRecord(std::string_view content)
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

        length = std::stoi(std::string(content.substr(begin, pos - begin)));
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
        size_t keyLen = 0;
        std::string_view key;
        if (!readLength(keyLen) || !consume(',') || !readField(keyLen, key) || !consume('='))
            return records;

        size_t valueLen = 0;
        std::string_view value;
        if (!readLength(valueLen) || !consume(',') || !readField(valueLen, value) || !consume('\n'))
            return records;

        records.emplace_back(key, value);
    }
    return records;
}
