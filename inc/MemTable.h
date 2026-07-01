#ifndef LSMTREE_MEMTABLE_H
#define LSMTREE_MEMTABLE_H

#include <map>
#include <string>
#include <string_view>
#include <filesystem>
#include <vector>
#include <fstream>
#include <unistd.h>

#include "utils.h"

class MemTable
{
public:
    explicit MemTable(std::string_view logFilePath);

    bool put(const std::string &key, const std::string &value);
    Result get(std::string_view key, std::string &value) const;
    bool remove(const std::string &key);
    [[nodiscard]] size_t size() const;

    using const_iterator = std::map<std::string, Entry, std::less<>>::const_iterator;
    [[nodiscard]] const_iterator begin() const noexcept { return table.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return table.end(); }

    [[nodiscard]] size_t size_bytes() const;

private:
    struct WALFileWriter
    {
        std::filesystem::path path;
        int fd = -1;
        bool poisoned = false;

        explicit WALFileWriter(std::string_view logPath);
        WALFileWriter(const WALFileWriter &other) = delete;
        ~WALFileWriter() {::close(fd);};

        WALFileWriter &operator=(const WALFileWriter &other) = delete;

        void write(const std::string &record);
        int truncate(size_t offset);
    };

    std::map<std::string, Entry, std::less<>> table;
    const std::filesystem::path log_path;
    WALFileWriter writer;
    size_t current_size;

    friend class MemTableIterator;

    void putToWAL(const std::string &key, const std::string &value, const Type type);
    bool restoreFromWAL();
    static std::vector<std::pair<std::string, Entry>> parseWALRecord(std::string_view content, size_t &lastGoodOffset);
};

class MemTableIterator : public Iterator
{
public:
    explicit MemTableIterator(const MemTable &mt);
    [[nodiscard]] bool valid() const override;
    [[nodiscard]] const Record& current() const override;
    void advance() override;

private:
    std::optional<Record> currentRecord;
    MemTable::const_iterator currentIt, endIt;
};


#endif //LSMTREE_MEMTABLE_H
