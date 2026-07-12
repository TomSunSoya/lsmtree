export module lsm.manifest;

import lsm.utils;
import std;
import std.compat;

export class Manifest
{
  public:
    explicit Manifest(std::filesystem::path path);

    [[nodiscard]] uint64_t nextNumber() const;
    uint64_t allocateNumber();

    void addTable(uint64_t number, std::string_view minKey, std::string_view maxKey, uint32_t targetLevel);
    void replaceTables(const std::vector<uint64_t>& removed, const std::vector<TableMeta>& added, uint32_t targetLevel);
    void save() const;

    [[nodiscard]] uint64_t logNumber() const { return logNumber_; }

    void setLogNumber(const uint64_t logNumber) { logNumber_ = logNumber; }

    [[nodiscard]] const std::vector<TableMeta>& level(size_t number) const;
    [[nodiscard]] size_t levelCount() const;
    [[nodiscard]] std::set<uint64_t, std::greater<>> allTableNumbers() const;
    [[nodiscard]] std::optional<TableMeta> getTableMeta(uint64_t levelNumber, std::string_view key) const;

  private:
    std::filesystem::path path_{};
    std::vector<std::vector<TableMeta>> levels_;
    uint64_t nextTableNumber_{};
    uint8_t formatVersion_{};
    uint64_t logNumber_{};
};
