#include "utils.h"

#include "FaultInjection.h"

#include <algorithm>
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
    size_t sourceIndex;
    uint64_t seq;

    bool operator<(const MergeItem& other) const
    {
        if (key != other.key)
            return key > other.key;
        return seq < other.seq;
    }
};

void enqueueCurrent(std::priority_queue<MergeItem>& queue, const std::vector<std::unique_ptr<Iterator>>& sources,
                    const size_t index)
{
    const auto& source = sources[index];
    if (source && source->valid())
        queue.emplace(source->current().key, index, source->current().seq);
}
} // namespace

std::optional<uint64_t> parseNumberedFile(const std::string_view filename, const std::string_view prefix,
                                          const std::string_view suffix)
{
    if (filename.size() <= prefix.size() + suffix.size() || !filename.starts_with(prefix) ||
        !filename.ends_with(suffix))
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

std::filesystem::path walPath(const std::filesystem::path& dataDirectory, const uint64_t fileNumber)
{
    return dataDirectory / "wal" / std::format("{}{}{}", kWalPrefix, fileNumber, kWalSuffix);
}

std::filesystem::path sstablePath(const std::filesystem::path& dataDirectory, const uint64_t fileNumber)
{
    return dataDirectory / "sstable" / std::format("{}{}{}", kSSTablePrefix, fileNumber, kSSTableSuffix);
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
    const auto* position = static_cast<const char*>(data);
    while (size > 0)
    {
        const ssize_t written = fault::write(fd, position, size);
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
    return encodedRecordSize(record);
}

void FileWriter::finish()
{
    if (fault::fsync(dataFile_.get()))
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
    if (fault::fsync(directory.get()))
        throw std::runtime_error("Failed to fsync dir!");
    directory.close();
}

int FileWriter::getFd() const { return dataFile_.get(); }

SnapshotIterator::SnapshotIterator(std::unique_ptr<Iterator> iterator, const uint64_t readSeq)
    : iterator_(std::move(iterator)), readSeq_(readSeq)
{
    skipInvisibleRecords();
}

bool SnapshotIterator::valid() const { return iterator_->valid(); }

const Record& SnapshotIterator::current() const { return iterator_->current(); }

void SnapshotIterator::advance()
{
    iterator_->advance();
    skipInvisibleRecords();
}

void SnapshotIterator::skipInvisibleRecords()
{
    while (valid() && current().seq > readSeq_)
        iterator_->advance();
}

std::vector<Record> mergeAll(std::vector<std::unique_ptr<Iterator>>& sources)
{
    if (sources.empty())
        return {};
    std::priority_queue<MergeItem> queue;
    for (size_t index = 0; index < sources.size(); ++index)
        enqueueCurrent(queue, sources, index);

    std::vector<Record> result;
    while (!queue.empty())
    {
        const MergeItem winner = queue.top();
        queue.pop();

        auto& source = sources[winner.sourceIndex];
        result.push_back(source->current());
        source->advance();
        enqueueCurrent(queue, sources, winner.sourceIndex);
    }
    return result;
}

void latestVisiblePerKey(std::vector<Record>& records)
{
    if (records.empty())
        return;
    const auto sameKey = [](const Record& lhs, const Record& rhs) { return lhs.key == rhs.key; };
    records.erase(std::ranges::unique(records, sameKey).begin(), records.end());
    std::erase_if(records, [](const Record& record) { return record.type == Type::TOMBSTONE; });
}

void retainForCompaction(std::vector<Record>& records, const uint64_t oldestSnapshotSequence)
{
    if (records.empty())
        return;

    std::vector<Record> sourceRecords;
    sourceRecords.swap(records);

    std::string currentKey = sourceRecords.front().key;
    bool keptSnapshotBoundary = false;
    for (const Record& record : sourceRecords)
    {
        if (record.key != currentKey)
        {
            currentKey = record.key;
            keptSnapshotBoundary = false;
        }

        if (record.seq > oldestSnapshotSequence)
        {
            records.push_back(record);
            continue;
        }

        if (!keptSnapshotBoundary)
        {
            records.push_back(record);
            keptSnapshotBoundary = true;
        }
    }
}
