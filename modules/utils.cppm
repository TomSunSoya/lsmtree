export module lsm.utils;

import std;
import std.compat;

export enum class Type : uint8_t { VALUE, TOMBSTONE };

export struct Entry
{
    Type type;
    std::string value;
};

export enum class Result { VALUE, TOMBSTONE, ABSENT };

export struct Record
{
    std::string key;
    Type type;
    std::string value;
};

export struct Index
{
    uint32_t keySize;
    std::string key;
    uint64_t offset;
};

export struct TableMeta
{
    uint64_t number;
    std::string minKey;
    std::string maxKey;
};

export constexpr std::string_view kWalPrefix = "wal_";
export constexpr std::string_view kWalSuffix = ".wal";
export constexpr std::string_view kSSTablePrefix = "sst_";
export constexpr std::string_view kSSTableSuffix = ".sst";

export [[nodiscard]] std::optional<uint64_t> parseNumberedFile(std::string_view filename, std::string_view prefix,
                                                               std::string_view suffix);

export [[nodiscard]] std::filesystem::path walPath(const std::filesystem::path& dataDir, uint64_t fileNumber);
export [[nodiscard]] std::filesystem::path sstablePath(const std::filesystem::path& dataDir, uint64_t fileNumber);
export void removeFile(const std::filesystem::path& path, const char* message);
export void writeAll(int fd, const void* data, std::size_t size);

export class FdGuard
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

export class FileWriter
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

export class Iterator
{
  public:
    virtual ~Iterator() = default;

    [[nodiscard]] virtual bool valid() const = 0;
    [[nodiscard]] virtual const Record& current() const = 0;
    virtual void advance() = 0;
};

export std::vector<Record> mergeSorted(std::vector<std::unique_ptr<Iterator>>& sources);
