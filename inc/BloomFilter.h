#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

class BloomFilter
{
  public:
    static std::vector<std::byte> Serialize(const BloomFilter& bloomFilter);
    static BloomFilter fromBytes(std::span<const std::byte> data);
    static BloomFilter forEntries(std::size_t expectedEntries, double falsePositiveProbability);
    static BloomFilter fromParameters(uint64_t bitCount, uint64_t hashCount);

    void add(std::string_view key);
    [[nodiscard]] bool mightContain(std::string_view key) const;

  private:
    BloomFilter(uint64_t bitCount, uint64_t hashCount);

    [[nodiscard]] uint64_t getBitPosition(std::string_view key, uint64_t hashIndex) const;

    std::vector<uint64_t> words_;
    uint64_t bitCount_;
    uint64_t hashCount_;
};
