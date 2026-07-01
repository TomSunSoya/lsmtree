#include "utils.h"

#include <cassert>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace
{
class VectorIterator final : public Iterator
{
public:
    explicit VectorIterator(std::vector<Record> records) : records(std::move(records))
    {
    }

    [[nodiscard]] bool valid() const override
    {
        return position < records.size();
    }

    [[nodiscard]] const Record& current() const override
    {
        assert(valid());
        return records[position];
    }

    void advance() override
    {
        if (valid())
            ++position;
    }

private:
    std::vector<Record> records;
    size_t position = 0;
};

Record value(std::string key, std::string content)
{
    return {std::move(key), Type::VALUE, std::move(content)};
}

Record tombstone(std::string key)
{
    return {std::move(key), Type::TOMBSTONE, ""};
}

std::unique_ptr<Iterator> source(std::vector<Record> records)
{
    return std::make_unique<VectorIterator>(std::move(records));
}

void expectRecords(const std::vector<Record> &actual, const std::vector<Record> &expected)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(expected[i].key, actual[i].key) << "record index: " << i;
        EXPECT_EQ(expected[i].type, actual[i].type) << "record index: " << i;
        EXPECT_EQ(expected[i].value, actual[i].value) << "record index: " << i;
    }
}
}

TEST(MergeSortedTest, EmptyAndNullSourcesProduceNoRecords)
{
    std::vector<std::unique_ptr<Iterator>> sources;
    sources.push_back(nullptr);
    sources.push_back(source({}));

    EXPECT_TRUE(mergeSorted(std::move(sources)).empty());
}

TEST(MergeSortedTest, MergesDisjointSourcesInKeyOrder)
{
    std::vector<std::unique_ptr<Iterator>> sources;
    sources.push_back(source({value("beta", "two"), value("delta", "four")}));
    sources.push_back(source({value("alpha", "one"), value("gamma", "three")}));

    const auto merged = mergeSorted(std::move(sources));

    expectRecords(
        merged,
        {
            value("alpha", "one"),
            value("beta", "two"),
            value("delta", "four"),
            value("gamma", "three"),
        });
}

TEST(MergeSortedTest, EarlierSourceWinsDuplicateAndLaterSourceContinues)
{
    std::vector<std::unique_ptr<Iterator>> sources;
    sources.push_back(source({value("alpha", "new"), value("gamma", "new-gamma")}));
    sources.push_back(source({
        value("alpha", "old"),
        value("beta", "kept"),
        value("delta", "also-kept"),
    }));

    const auto merged = mergeSorted(std::move(sources));

    expectRecords(
        merged,
        {
            value("alpha", "new"),
            value("beta", "kept"),
            value("delta", "also-kept"),
            value("gamma", "new-gamma"),
        });
}

TEST(MergeSortedTest, KeepsTombstoneFromWinningSource)
{
    std::vector<std::unique_ptr<Iterator>> sources;
    sources.push_back(source({tombstone("alpha")}));
    sources.push_back(source({value("alpha", "old"), value("beta", "kept")}));

    const auto merged = mergeSorted(std::move(sources));

    expectRecords(
        merged,
        {
            tombstone("alpha"),
            value("beta", "kept"),
        });
}
