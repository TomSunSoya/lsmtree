module;

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils.h"

export module lsm.memtable;

export class MemTable
{
  public:
    explicit MemTable(std::string_view logFilePath);

    bool put(const std::string& key, const std::string& value);
    Result get(std::string_view key, std::string& value) const;
    bool remove(const std::string& key);

    [[nodiscard]] size_t size() const;
    [[nodiscard]] size_t size_bytes() const;

    using const_iterator = std::map<std::string, Entry, std::less<>>::const_iterator;

    [[nodiscard]] const_iterator begin() const noexcept { return table_.begin(); }

    [[nodiscard]] const_iterator end() const noexcept { return table_.end(); }

  private:
    struct WALFileWriter
    {
        explicit WALFileWriter(std::string_view logPath);
        ~WALFileWriter();

        WALFileWriter(const WALFileWriter&) = delete;
        WALFileWriter& operator=(const WALFileWriter&) = delete;

        void write(const std::string& record);
        int truncate(size_t offset);

        std::filesystem::path path_;
        int fileDescriptor_ = -1;
        bool poisoned_ = false;
    };

    friend class MemTableIterator;

    bool appendToWAL(const std::string& key, const std::string& value, Type type);
    bool restoreFromWAL();
    static std::vector<std::pair<std::string, Entry>> parseWALRecords(std::string_view content, size_t& lastGoodOffset);

    std::map<std::string, Entry, std::less<>> table_;
    const std::filesystem::path logPath_;
    WALFileWriter walWriter_;
    size_t currentSizeBytes_;
};

export class MemTableIterator : public Iterator
{
  public:
    explicit MemTableIterator(const MemTable& memTable);

    [[nodiscard]] bool valid() const override;
    [[nodiscard]] const Record& current() const override;
    void advance() override;

  private:
    void refreshCurrentRecord();

    std::optional<Record> currentRecord_;
    MemTable::const_iterator currentIterator_;
    MemTable::const_iterator endIterator_;
};
