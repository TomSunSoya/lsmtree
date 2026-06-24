#include "SSTable.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <format>
#include <queue>
#include <unistd.h>

namespace
{
void writeAll(const int fd, const void *data, std::size_t size)
{
    auto p = static_cast<const char *>(data);
    while (size > 0)
    {
        const ssize_t n = ::write(fd, p, size);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;

            const int err = errno;
            throw std::system_error(err, std::generic_category(), "write failed");
        }

        if (n == 0)
            throw std::runtime_error("write returned 0");

        p += n;
        size -= static_cast<std::size_t>(n);
    }
}
}

SSTableWriter::SSTableWriter(std::filesystem::path path) : path_(std::move(path)), dataFd(-1)
{
    if (parentDir = path_.parent_path(); parentDir.empty())
    {
        parentDir = ".";
    }
    else
    {
        std::filesystem::create_directories(parentDir);
    }

    tempPath = path_.string() + ".tmp";
    int fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0)
        throw std::runtime_error("Failed to open file for writing!");
    dataFd.setFd(fd);
}

void SSTableWriter::add(const Record &record)
{
    auto &key = record.key;
    auto &value = record.value;
    const uint32_t key_size = key.size();
    const uint32_t value_size = value.size();
    const auto type = static_cast<uint8_t>(record.type);

    writeAll(dataFd.get(), &type, sizeof(type));
    writeAll(dataFd.get(), &key_size, sizeof(key_size));
    writeAll(dataFd.get(), &value_size, sizeof(value_size));
    writeAll(dataFd.get(), key.data(), key_size);
    writeAll(dataFd.get(), value.data(), value_size);
}

void SSTableWriter::finish()
{
    if (::fsync(dataFd.get()))
    {
        throw std::runtime_error("Failed to fsync table!");
    }

    dataFd.close();
    if (::rename(tempPath.c_str(), path_.c_str()))
    {
        const int err = errno;
        throw std::system_error(err, std::generic_category(), "rename failed");
    }

    const int dirFd = ::open(parentDir.c_str(), O_RDONLY);
    if (dirFd < 0)
        throw std::runtime_error("Failed to open dir for reading!");

    FdGuard dir(dirFd);
    if (::fsync(dir.get()))
        throw std::runtime_error("Failed to fsync dir!");
    dir.close();
}

void SSTable::build(const MemTable &mt, const std::filesystem::path &path)
{
    if (std::filesystem::exists(path))
        throw std::runtime_error("SSTable file already exists!");

    SSTableWriter writer(path);
    for (auto &[key, entry] : mt)
    {
        writer.add({key, entry.type, entry.value});
    }

    writer.finish();
}

void SSTable::merge(const std::filesystem::path& dir)
{
    namespace fs = std::filesystem;

    if (!fs::exists(dir) || !fs::is_directory(dir))
        throw std::runtime_error("SSTable directory must exist!");

    std::vector<fs::path> sortedPaths;

    for (std::error_code ec; const auto &entry : fs::directory_iterator(dir, ec))
    {
        if (ec)
            throw std::runtime_error(entry.path().string() + ": " + ec.message());
        if (!entry.is_regular_file(ec))
            continue;

        if (const fs::path &path = entry.path(); path.extension() == ".sst")
        {
            sortedPaths.push_back(path);
        }
    }

    const auto ParseSSTableNumber = [] (const fs::path &file) -> uint64_t
    {
        const auto number = parseNumberedFile(file.filename().string(), "sst_", ".sst");
        if (!number)
            return 0;
        return *number;
    };

    std::ranges::sort(sortedPaths, [&] (const fs::path &a, const fs::path &b) -> bool
    {
        return ParseSSTableNumber(a.filename().string()) < ParseSSTableNumber(b.filename().string());
    });

    std::vector<Cursor> cursors;
    std::ranges::transform(sortedPaths, std::back_inserter(cursors), [] (const fs::path &path)
    {
        return Cursor{path};
    });


    struct Item
    {
        std::string key;
        int index;

        bool operator<(const Item &item) const
        {
            if (key != item.key)
                return key > item.key;
            return index < item.index;
        }
    };

    std::priority_queue<Item> items;
    for (int i = 0; i < cursors.size(); ++i)
    {
        if (auto &cursor = cursors[i]; cursor.valid())
        {
            auto &record = cursor.current();
            Item item{record.key, i};
            items.push(item);
        }
    }

    auto number = maxFileByName(dir);
    fs::path finalPath = dir / std::format("sst_{}.sst", number);
    SSTableWriter writer(finalPath);

    while (!items.empty())
    {
        const auto [key, index] = items.top();
        items.pop();
        auto &cursor = cursors[index];
        writer.add(cursor.current());

        // item.key == cursor.key
        cursor.advance();

        while (!items.empty() && key == items.top().key)
        {
            auto cur_index = items.top().index;
            auto &cur_cursor = cursors[cur_index];
            cur_cursor.advance();
            items.pop();

            if (cur_cursor.valid())
                items.push({cur_cursor.current().key, cur_index});
        }

        if (cursor.valid())
            items.push({cursor.current().key, index});
    }
    writer.finish();
}

void SSTable::cleanupOrphanedTemps(const std::filesystem::path& dir)
{
    std::error_code ec;
    namespace fs = std::filesystem;
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
}

Result SSTable::get(const std::string_view key, std::string &value) const
{
    if (!std::filesystem::exists(path))
        return Result::ABSENT;

    std::ifstream ifs{path, std::ios::binary};
    if (!ifs.is_open())
        return Result::ABSENT;

    while (const auto curRecord = readOneRecord(ifs))
    {
        if (curRecord->key == key)
        {
            if (curRecord->type != Type::VALUE)
                return Result::TOMBSTONE;
            value = curRecord->value;
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

Cursor::Cursor(std::filesystem::path  path) : path(std::move(path))
{
    if (!std::filesystem::exists(this->path))
        throw std::runtime_error("Invalid path");

    ifs.open(this->path, std::ios::binary);
    if (!ifs.is_open())
        throw std::runtime_error("Failed to open SSTable file!");

    currentRecord = SSTable::readOneRecord(ifs);
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
    if (valid())
        currentRecord = SSTable::readOneRecord(ifs);
}

