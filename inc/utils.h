#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class Type : uint8_t
{
    VALUE,
    TOMBSTONE
};

struct Entry
{
    Type type;
    uint64_t seq;
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
    uint64_t seq;
    Type type;
    std::string value;
};

// type(1) + sequence(8) + key size(4) + value size(4)
constexpr uint64_t kEncodedRecordHeaderSize = sizeof(uint8_t) + sizeof(uint64_t) + 2 * sizeof(uint32_t);

[[nodiscard]] inline uint64_t encodedRecordSize(const Record& record)
{
    return kEncodedRecordHeaderSize + record.key.size() + record.value.size();
}

struct Index
{
    uint32_t keySize;
    std::string key;
    uint64_t offset;
};

struct TableMeta
{
    uint64_t number;
    uint64_t size;
    std::string minKey;
    std::string maxKey;
};

struct Key
{
    std::string key;
    uint64_t seq;

    bool operator<(const Key& rhs) const
    {
        if (key != rhs.key)
            return key < rhs.key;
        return seq > rhs.seq;
    }
};

constexpr std::string_view kWalPrefix = "wal_";
constexpr std::string_view kWalSuffix = ".wal";
constexpr std::string_view kSSTablePrefix = "sst_";
constexpr std::string_view kSSTableSuffix = ".sst";

[[nodiscard]] std::optional<uint64_t> parseNumberedFile(std::string_view filename, std::string_view prefix,
                                                        std::string_view suffix);

[[nodiscard]] std::filesystem::path walPath(const std::filesystem::path& dataDirectory, uint64_t fileNumber);
[[nodiscard]] std::filesystem::path sstablePath(const std::filesystem::path& dataDirectory, uint64_t fileNumber);
void removeFile(const std::filesystem::path& path, const char* message);
void writeAll(int fd, const void* data, std::size_t size);

inline bool rangesOverlap(const TableMeta& table, std::string_view minKey, std::string_view maxKey)
{
    return minKey <= table.maxKey && table.minKey <= maxKey;
}

class FdGuard
{
  public:
    explicit FdGuard(int fd);
    ~FdGuard();

    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;

    void setFd(int fd);
    [[nodiscard]] int get() const;
    void close();

  private:
    int fd_ = -1;
};

class FileWriter
{
  public:
    explicit FileWriter(std::filesystem::path path);

    uint64_t add(const Record& record);
    void finish();
    [[nodiscard]] int getFd() const;

  private:
    std::filesystem::path path_;
    std::filesystem::path temporaryPath_;
    std::filesystem::path parentDirectory_;
    FdGuard dataFile_;
};

class Iterator
{
  public:
    virtual ~Iterator() = default;

    [[nodiscard]] virtual bool valid() const = 0;
    [[nodiscard]] virtual const Record& current() const = 0;
    virtual void advance() = 0;
};

class SnapshotIterator : public Iterator
{
  public:
    explicit SnapshotIterator(std::unique_ptr<Iterator> iterator, uint64_t readSeq);

    ~SnapshotIterator() override = default;
    [[nodiscard]] bool valid() const override;
    [[nodiscard]] const Record& current() const override;
    void advance() override;

  private:
    void skipInvisibleRecords();

    std::unique_ptr<Iterator> iterator_;
    uint64_t readSeq_;
};

std::vector<Record> mergeAll(std::vector<std::unique_ptr<Iterator>>& sources);

void latestVisiblePerKey(std::vector<Record>& records);
void retainForCompaction(std::vector<Record>& records, uint64_t oldestSnapshotSequence);
