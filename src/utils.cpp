#include "utils.h"

#include <filesystem>
#include <sys/fcntl.h>

FileWriter::FileWriter(std::filesystem::path path) : path_(std::move(path)), dataFd(-1)
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

void FileWriter::add(const Record &record)
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

void FileWriter::finish()
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

int FileWriter::getFd() const
{
    return dataFd.get();
}
