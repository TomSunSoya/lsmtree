#include "BloomFilter.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace
{
constexpr size_t kBitsPerWord = 64;
constexpr size_t kSerializedHeaderSize = 2 * sizeof(uint64_t);
constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325;
constexpr uint64_t kFnvPrime = 0x100000001b3;

size_t wordCountFor(const uint64_t bitCount) { return (bitCount + kBitsPerWord - 1) / kBitsPerWord; }

uint64_t getFNV1a(std::string_view key)
{
    uint64_t hash = kFnvOffsetBasis;
    for (const char character : key)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= kFnvPrime;
    }
    return hash;
}
} // namespace

std::vector<std::byte> BloomFilter::Serialize(const BloomFilter& bloomFilter)
{
    std::vector<std::byte> serialized(kSerializedHeaderSize + bloomFilter.words_.size() * sizeof(uint64_t));
    std::memcpy(serialized.data(), &bloomFilter.bitCount_, sizeof(bloomFilter.bitCount_));
    std::memcpy(serialized.data() + sizeof(bloomFilter.bitCount_), &bloomFilter.hashCount_,
                sizeof(bloomFilter.hashCount_));

    size_t offset = kSerializedHeaderSize;
    for (const uint64_t word : bloomFilter.words_)
    {
        std::memcpy(serialized.data() + offset, &word, sizeof(word));
        offset += sizeof(word);
    }
    return serialized;
}

BloomFilter BloomFilter::fromBytes(const std::span<const std::byte> data)
{
    if (data.size() < kSerializedHeaderSize || data.size() % sizeof(uint64_t) != 0)
        throw std::length_error("BloomFilter::fromBytes: data size is not a multiple of uint64_t");

    uint64_t bitCount = 0;
    std::memcpy(&bitCount, data.data(), sizeof(bitCount));
    if (data.size() != kSerializedHeaderSize + wordCountFor(bitCount) * sizeof(uint64_t))
        throw std::length_error("BloomFilter::fromBytes: data size error!");

    uint64_t hashCount = 0;
    std::memcpy(&hashCount, data.data() + sizeof(bitCount), sizeof(hashCount));
    BloomFilter bloomFilter(bitCount, hashCount);

    size_t offset = kSerializedHeaderSize;
    for (uint64_t& word : bloomFilter.words_)
    {
        std::memcpy(&word, data.data() + offset, sizeof(word));
        offset += sizeof(word);
    }
    return bloomFilter;
}

BloomFilter BloomFilter::forEntries(const std::size_t expectedEntries, const double falsePositiveProbability)
{
    static const double naturalLogOfTwo = std::log(2.0);
    const double calculatedBitCount = -static_cast<double>(expectedEntries) * std::log(falsePositiveProbability) /
                                      (naturalLogOfTwo * naturalLogOfTwo);

    const auto bitCount = static_cast<uint64_t>(std::ceil(calculatedBitCount));
    const auto hashCount = static_cast<uint64_t>(std::round(calculatedBitCount / expectedEntries * naturalLogOfTwo));
    return BloomFilter(bitCount, hashCount);
}

BloomFilter BloomFilter::fromParameters(const uint64_t bitCount, const uint64_t hashCount)
{
    return BloomFilter(bitCount, hashCount);
}

BloomFilter::BloomFilter(uint64_t bitCount, uint64_t hashCount) : bitCount_(bitCount), hashCount_(hashCount)
{
    words_.resize(wordCountFor(bitCount_));
}

void BloomFilter::add(std::string_view key)
{
    for (uint64_t hashIndex = 0; hashIndex < hashCount_; ++hashIndex)
    {
        const uint64_t bitPosition = getBitPosition(key, hashIndex);
        words_[bitPosition / kBitsPerWord] |= uint64_t{1} << (bitPosition % kBitsPerWord);
    }
}

bool BloomFilter::mightContain(const std::string_view key) const
{
    for (uint64_t hashIndex = 0; hashIndex < hashCount_; ++hashIndex)
    {
        const uint64_t bitPosition = getBitPosition(key, hashIndex);
        const uint64_t mask = uint64_t{1} << (bitPosition % kBitsPerWord);
        if ((words_[bitPosition / kBitsPerWord] & mask) == 0)
            return false;
    }
    return true;
}

uint64_t BloomFilter::getBitPosition(std::string_view key, uint64_t hashIndex) const
{
    const auto hashValue = getFNV1a(key);
    const uint32_t firstHash = hashValue >> 32;
    uint32_t secondHash = hashValue & 0xFFFFFFFF;
    if (secondHash == 0)
        secondHash = 1;

    return (firstHash + hashIndex * secondHash) % bitCount_;
}
