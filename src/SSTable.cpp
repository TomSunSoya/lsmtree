#include "SSTable.h"

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace
{
class FdGuard
{
public:
    explicit FdGuard(const int fd) : fd_(fd) {}

    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;

    ~FdGuard()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    [[nodiscard]] int get() const
    {
        return fd_;
    }

    void close()
    {
        if (fd_ < 0)
            return;

        if (::close(fd_) != 0)
        {
            const int err = errno;
            throw std::system_error(err, std::generic_category(), "close failed");
        }

        fd_ = -1;
    }

private:
    int fd_;
};

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

void SSTable::build(const MemTable &mt, const std::filesystem::path &path)
{
    std::filesystem::path parentDir = path.parent_path();
    if (parentDir.empty())
    {
        parentDir = ".";
    }
    else
    {
        std::filesystem::create_directories(parentDir);
    }

    if (std::filesystem::exists(path))
        throw std::runtime_error("SSTable has been exist!");

    const std::filesystem::path tempPath(path.string() + ".tmp");
    const int dataFd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (dataFd < 0)
        throw std::runtime_error("Failed to open file for writing!");

    FdGuard dataFile(dataFd);
    for (auto &[key, entry] : mt)
    {
        const uint32_t key_size = key.size();
        const uint32_t value_size = entry.value.size();
        auto type = static_cast<uint8_t>(entry.type);

        writeAll(dataFile.get(), &type, sizeof(type));
        writeAll(dataFile.get(), &key_size, sizeof(key_size));
        writeAll(dataFile.get(), &value_size, sizeof(value_size));
        writeAll(dataFile.get(), key.data(), key_size);
        writeAll(dataFile.get(), entry.value.data(), value_size);
    }

    if (::fsync(dataFile.get()))
        throw std::runtime_error("Failed to fsync!");
    dataFile.close();

    if (::rename(tempPath.c_str(), path.c_str()))
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

