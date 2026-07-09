#ifndef LSMTREE_MANIFEST_H
#define LSMTREE_MANIFEST_H

#include <filesystem>
#include <cstdint>
#include <functional>
#include <set>
#include <vector>

struct TableMeta
{
    uint64_t number;
    std::string minKey, maxKey;
};

class Manifest
{
public:
    explicit Manifest(std::filesystem::path  path);

    // search
    [[nodiscard]] uint64_t nextNumber() const;

    // memory modify
    uint64_t allocateNumber();
    void addTable(uint64_t n, std::string_view minKey, std::string_view maxKey, uint32_t targetLevel);
    void replaceTables(const std::vector<uint64_t>& removed, uint64_t added, std::string_view minKey,
                       std::string_view maxKey, uint32_t targetLevel);

    void save() const;

    [[nodiscard]] uint64_t logNumber() const
    {
        return logNumber_;
    }

    void setLogNumber(const uint64_t log_number)
    {
        logNumber_ = log_number;
    }

    [[nodiscard]] const std::vector<TableMeta> &level(size_t n) const;

    [[nodiscard]] size_t levelCount() const;

    [[nodiscard]] std::set<uint64_t, std::greater<>> allTableNumbers() const;

    [[nodiscard]] std::optional<TableMeta> getTableMeta(uint64_t n_level, std::string_view key) const;

private:
    std::filesystem::path path_;
    std::vector<std::vector<TableMeta>> levels;
    uint64_t next_{};
    uint8_t version_{};
    uint64_t logNumber_{};
};


#endif //LSMTREE_MANIFEST_H
