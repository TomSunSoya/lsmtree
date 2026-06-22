#include "SSTable.h"

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

    char type = 0;
    while (ifs.read(&type, sizeof(char)))
    {
        uint32_t key_size{}, value_size{};
        if (!ifs.read(reinterpret_cast<char *>(&key_size), sizeof(key_size)))
            return Result::ABSENT;
        if (!ifs.read(reinterpret_cast<char *>(&value_size), sizeof(value_size)))
            return Result::ABSENT;

        std::string cur_key(key_size, 0);
        std::string cur_value(value_size, 0);

        if (!ifs.read(cur_key.data(), key_size))
            return Result::ABSENT;
        if (!ifs.read(cur_value.data(), value_size))
            return Result::ABSENT;

        if (cur_key == key)
        {
            if (static_cast<Type>(type) == Type::VALUE)
            {
                value = cur_value;
                return Result::VALUE;
            }
            return Result::TOMBSTONE;
        }
    }
    return Result::ABSENT;
}