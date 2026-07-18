#include "utils.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <format>
#include <queue>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace
{
struct MergeItem
{
    std::string key;
    int sourceIndex;

    bool operator<(const MergeItem& other) const
    {
        if (key != other.key)
            return key > other.key;
        return sourceIndex > other.sourceIndex;
    }
};

void enqueueCurrent(std::priority_queue<MergeItem>& queue, const std::vector<std::unique_ptr<Iterator>>& sources,
                    int index)
{
    if (sources[index] && sources[index]->valid())
        queue.emplace(sources[index]->current().key, index);
}
} // namespace

std::optional<uint64_t> parseNumberedFile(const std::string_view filename, const std::string_view prefix,
                                          const std::string_view suffix)
{
    if (filename.size() <= prefix.size() + suffix.size())
        return std::nullopt;
    if (!filename.starts_with(prefix))
        return std::nullopt;
    if (!filename.ends_with(suffix))
        return std::nullopt;

    const std::string numberText{filename.substr(prefix.size(), filename.size() - prefix.size() - suffix.size())};
    try
    {
        size_t parsedCharacters = 0;
        const auto value = std::stoull(numberText, &parsedCharacters, 10);
        if (parsedCharacters != numberText.size())
            return std::nullopt;

        return value;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::filesystem::path walPath(const std::filesystem::path& dataDir, const uint64_t fileNumber)
{
    return dataDir / "wal" / std::format("{}{}{}", kWalPrefix, fileNumber, kWalSuffix);
}

std::filesystem::path sstablePath(const std::filesystem::path& dataDir, const uint64_t fileNumber)
{
    return dataDir / "sstable" / std::format("{}{}{}", kSSTablePrefix, fileNumber, kSSTableSuffix);
}

void removeFile(const std::filesystem::path& path, const char* message)
{
    if (::remove(path.c_str()))
    {
        const int error = errno;
        throw std::system_error(error, std::system_category(), message);
    }
}

void writeAll(const int fd, const void* data, std::size_t size)
{
    auto* position = static_cast<const char*>(data);
    while (size > 0)
    {
        const ssize_t written = ::write(fd, position, size);
        if (written < 0)
        {
            if (errno == EINTR)
                continue;

            const int error = errno;
            throw std::system_error(error, std::generic_category(), "write failed");
        }

        if (written == 0)
            throw std::runtime_error("write returned 0");

        position += written;
        size -= static_cast<std::size_t>(written);
    }
}

FdGuard::FdGuard(const int fd) : fd_(fd) {}

FdGuard::~FdGuard()
{
    if (fd_ >= 0)
        ::close(fd_);
}

void FdGuard::setFd(const int fd)
{
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = fd;
}

int FdGuard::get() const { return fd_; }

void FdGuard::close()
{
    if (fd_ < 0)
        return;

    if (::close(fd_) != 0)
    {
        const int error = errno;
        throw std::system_error(error, std::generic_category(), "close failed");
    }

    fd_ = -1;
}

FileWriter::FileWriter(std::filesystem::path path) : path_(std::move(path)), dataFile_(-1)
{
    parentDirectory_ = path_.parent_path();
    if (parentDirectory_.empty())
    {
        parentDirectory_ = ".";
    }
    else
    {
        std::filesystem::create_directories(parentDirectory_);
    }

    temporaryPath_ = path_.string() + ".tmp";
    const int fileDescriptor = ::open(temporaryPath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fileDescriptor < 0)
        throw std::runtime_error("Failed to open file for writing!");
    dataFile_.setFd(fileDescriptor);
}

uint64_t FileWriter::add(const Record& record)
{
    const uint32_t keySize = record.key.size();
    const uint32_t valueSize = record.value.size();
    const uint64_t seq = record.seq;
    const auto type = static_cast<uint8_t>(record.type);

    writeAll(dataFile_.get(), &type, sizeof(type));
    writeAll(dataFile_.get(), &seq, sizeof(seq));
    writeAll(dataFile_.get(), &keySize, sizeof(keySize));
    writeAll(dataFile_.get(), &valueSize, sizeof(valueSize));
    writeAll(dataFile_.get(), record.key.data(), keySize);
    writeAll(dataFile_.get(), record.value.data(), valueSize);
    return kEncodedRecordHeaderSize + keySize + valueSize;
}

void FileWriter::finish()
{
    if (::fsync(dataFile_.get()))
        throw std::runtime_error("Failed to fsync table!");

    dataFile_.close();
    if (::rename(temporaryPath_.c_str(), path_.c_str()))
    {
        const int error = errno;
        throw std::system_error(error, std::generic_category(), "rename failed");
    }

    const int directoryDescriptor = ::open(parentDirectory_.c_str(), O_RDONLY);
    if (directoryDescriptor < 0)
        throw std::runtime_error("Failed to open dir for reading!");

    FdGuard directory(directoryDescriptor);
    if (::fsync(directory.get()))
        throw std::runtime_error("Failed to fsync dir!");
    directory.close();
}

int FileWriter::getFd() const { return dataFile_.get(); }

std::vector<Record> mergeSorted(std::vector<std::unique_ptr<Iterator>>& sources)
{
    std::priority_queue<MergeItem> queue;
    for (int index = 0; index < sources.size(); ++index)
        enqueueCurrent(queue, sources, index);

    std::vector<Record> result;
    while (!queue.empty())
    {
        const MergeItem winner = queue.top();
        queue.pop();

        while (!queue.empty() && queue.top().key == winner.key)
        {
            const int shadowedIndex = queue.top().sourceIndex;
            queue.pop();

            sources[shadowedIndex]->advance();
            enqueueCurrent(queue, sources, shadowedIndex);
        }

        auto& winningSource = sources[winner.sourceIndex];
        result.push_back(winningSource->current());
        winningSource->advance();
        enqueueCurrent(queue, sources, winner.sourceIndex);
    }

    return result;
}
