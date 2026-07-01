#pragma once
#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

enum class Type : uint8_t
{
    VALUE,
    TOMBSTONE
};

struct Entry
{
    Type type;
    std::string value;
};

enum class Result
{
    VALUE,
    TOMBSTONE,
    ABSENT
};

struct Record
{
    std::string key;
    Type type;
    std::string value;
};

struct Index
{
    uint32_t keySize;
    std::string key;
    uint64_t offset;
};

inline std::optional<uint64_t> parseNumberedFile(
    const std::string_view filename,
    const std::string_view prefix,
    const std::string_view suffix)
{
    if (filename.size() <= prefix.size() + suffix.size())
        return std::nullopt;
    if (!filename.starts_with(prefix))
        return std::nullopt;
    if (!filename.ends_with(suffix))
        return std::nullopt;

    const std::string numberStr{filename.substr(prefix.size(), filename.size() - prefix.size() - suffix.size())};
    try
    {
        size_t pos = 0;
        const auto value = std::stoull(numberStr, &pos, 10);
        if (pos != numberStr.size())
            return std::nullopt;

        return value;
    } catch (const std::exception &)
    {
        return std::nullopt;
    }
}

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

    void setFd(const int fd)
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
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

static void writeAll(const int fd, const void *data, std::size_t size)
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


class FileWriter
{
public:
    explicit FileWriter(std::filesystem::path path);
    uint64_t add(const Record &);
    void finish();
    [[nodiscard]] int getFd() const;

private:
    std::filesystem::path path_, tempPath, parentDir;
    FdGuard dataFd;
};

class Iterator
{
public:
    virtual ~Iterator() = default;
    [[nodiscard]] virtual bool valid() const = 0;
    [[nodiscard]] virtual const Record &current() const = 0;
    virtual void advance() = 0;
};

std::vector<Record> mergeSorted(std::vector<std::unique_ptr<Iterator>> sources);
