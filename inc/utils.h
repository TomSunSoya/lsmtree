#pragma once
#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>

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

inline uint64_t maxFileByName(const std::filesystem::path &path)
{
    namespace fs = std::filesystem;
    if (!fs::exists(path) || !fs::is_directory(path))
        return 0;

    std::error_code ec;
    std::optional<uint64_t> currentFileNumber = std::nullopt;
    for (const auto &entry : fs::directory_iterator(path, ec))
    {
        if (ec) return 0;
        if (!entry.is_regular_file(ec))
        {
            ec.clear();
            continue;
        }

        auto number = parseNumberedFile(entry.path().filename().string(), "sst_", ".sst");
        if (!number)
            continue;
        if (!currentFileNumber || *currentFileNumber < *number)
            currentFileNumber = *number;
    }
    if (currentFileNumber)
        ++*currentFileNumber;
    return currentFileNumber ? *currentFileNumber : 0;
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
    void add(const Record &);
    void finish();
    [[nodiscard]] int getFd() const;

private:
    std::filesystem::path path_, tempPath, parentDir;
    FdGuard dataFd;
};