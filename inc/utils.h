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

struct TableMeta
{
    uint64_t number;
    std::string minKey;
    std::string maxKey;
};

constexpr std::string_view kWalPrefix = "wal_";
constexpr std::string_view kWalSuffix = ".wal";
constexpr std::string_view kSSTablePrefix = "sst_";
constexpr std::string_view kSSTableSuffix = ".sst";

[[nodiscard]] std::optional<uint64_t> parseNumberedFile(std::string_view filename, std::string_view prefix,
                                                        std::string_view suffix);

[[nodiscard]] std::filesystem::path walPath(const std::filesystem::path& dataDir, uint64_t fileNumber);
[[nodiscard]] std::filesystem::path sstablePath(const std::filesystem::path& dataDir, uint64_t fileNumber);
void removeFile(const std::filesystem::path& path, const char* message);
void writeAll(int fd, const void* data, std::size_t size);

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
    int fd_;
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

std::vector<Record> mergeSorted(std::vector<std::unique_ptr<Iterator>>& sources);
