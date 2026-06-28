#include "BloomFilter.h"

#include <cmath>

std::vector<std::byte> BloomFilter::Serialize(const BloomFilter& bloom_filter)
{
    std::vector<std::byte> serialized(16 + bloom_filter.bit.size() * 8);
    std::memcpy(serialized.data(), &bloom_filter.m, sizeof(bloom_filter.m));
    std::memcpy(serialized.data() + sizeof(bloom_filter.m), &bloom_filter.k, sizeof(bloom_filter.k));
    uint64_t offset = sizeof(bloom_filter.m) + sizeof(bloom_filter.k);
    for (const auto &content : bloom_filter.bit)
    {
        std::memcpy(serialized.data() + offset, &content, sizeof(content));
        offset += sizeof(content);
    }
    return serialized;
}

BloomFilter BloomFilter::fromBytes(const std::span<const std::byte> data)
{
    if (data.size() < 16 || data.size() % sizeof(uint64_t) != 0)
        throw std::length_error("BloomFilter::fromBytes: data size is not a multiple of uint64_t");
    uint64_t m = 0, k = 0;
    std::memcpy(&m, data.data(), sizeof(m));
    if (data.size() != 16 + ((m + 63) / 64) * 8)
        throw std::length_error("BloomFilter::fromBytes: data size error!");

    std::memcpy(&k, data.data() + sizeof(m), sizeof(k));
    BloomFilter bloom_filter(m, k);

    size_t offset = sizeof(m) + sizeof(k);
    for (unsigned long long &i : bloom_filter.bit)
    {
        uint64_t value;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        offset += sizeof(value);
        i = value;
    }
    return bloom_filter;
}

BloomFilter::BloomFilter(const unsigned int n, const double p)
{
    static const double ln2 = std::log(2.0);
    const double mDouble = -static_cast<double>(n) * std::log(p) / (ln2 * ln2);
    m = static_cast<uint64_t>(std::ceil(mDouble));
    k = static_cast<uint64_t>(std::round(mDouble / n * ln2));
    bit.resize((m + 63) / 64);
}

BloomFilter::BloomFilter(const uint64_t m, const uint64_t k) : m(m), k(k)
{
    bit.resize((m + 63) / 64);
}

void BloomFilter::add(const std::string_view key)
{
    for (uint64_t i = 0; i < k; ++i)
    {
        const uint64_t pos = getBitPosition(key, i);
        bit[pos / 64] |= 1ull << (pos % 64);
    }
}

bool BloomFilter::mightContain(const std::string_view key) const
{
    for (uint64_t i = 0; i < k; ++i)
    {
        if (const uint64_t pos = getBitPosition(key, i); (bit[pos / 64] & 1ull << (pos % 64)) == 0)
            return false;
    }
    return true;
}

uint64_t BloomFilter::getBitPosition(const std::string_view key, const uint64_t pos) const
{
    const size_t h = std::hash<std::string_view>{}(key);
    const uint32_t h1 = h >> 32;
    uint32_t h2 = h & 0xFFFFFFFF;
    if (h2 == 0)
    {
        h2 = 1;
    }
    return (h1 + pos * h2) % m;
}
