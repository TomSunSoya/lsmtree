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
    explicit VectorIterator(std::vector<Record> records) : records(std::move(records)) {}

    [[nodiscard]] bool valid() const override { return position < records.size(); }

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

Record value(std::string key, std::string content, const uint64_t seq = 0)
{
    return {std::move(key), seq, Type::VALUE, std::move(content)};
}

Record tombstone(std::string key, const uint64_t seq = 0) { return {std::move(key), seq, Type::TOMBSTONE, ""}; }

std::unique_ptr<Iterator> source(std::vector<Record> records)
{
    return std::make_unique<VectorIterator>(std::move(records));
}

void expectRecords(const std::vector<Record>& actual, const std::vector<Record>& expected)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(expected[i].key, actual[i].key) << "record index: " << i;
        EXPECT_EQ(expected[i].seq, actual[i].seq) << "record index: " << i;
        EXPECT_EQ(expected[i].type, actual[i].type) << "record index: " << i;
        EXPECT_EQ(expected[i].value, actual[i].value) << "record index: " << i;
    }
}
} // namespace

TEST(SnapshotIteratorTest, ConstructorPositionsAtNewestVisibleVersionAndAdvanceFindsTheNextOne)
{
    SnapshotIterator iterator(source({
                                  value("k", "newest", 90),
                                  value("k", "visible", 50),
                                  value("k", "oldest", 10),
                              }),
                              60);

    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ("k", iterator.current().key);
    EXPECT_EQ(50, iterator.current().seq);

    iterator.advance();
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ("k", iterator.current().key);
    EXPECT_EQ(10, iterator.current().seq);
}

TEST(SnapshotIteratorTest, ConstructorSkipsInvisibleKeyAndPositionsAtNextVisibleKey)
{
    SnapshotIterator iterator(source({
                                  value("a", "invisible", 90),
                                  value("b", "visible", 50),
                              }),
                              60);

    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ("b", iterator.current().key);
    EXPECT_EQ(50, iterator.current().seq);
}

TEST(MergeSortedTest, EmptyAndNullSourcesProduceNoRecords)
{
    std::vector<std::unique_ptr<Iterator>> sources;
    sources.push_back(nullptr);
    sources.push_back(source({}));

    EXPECT_TRUE(mergeSorted(sources).empty());
}

TEST(MergeSortedTest, MergesDisjointSourcesInKeyOrder)
{
    std::vector<std::unique_ptr<Iterator>> sources;
    sources.push_back(source({value("beta", "two"), value("delta", "four")}));
    sources.push_back(source({value("alpha", "one"), value("gamma", "three")}));

    const auto merged = mergeSorted(sources);

    expectRecords(merged, {
                              value("alpha", "one"),
                              value("beta", "two"),
                              value("delta", "four"),
                              value("gamma", "three"),
                          });
}

TEST(MergeSortedTest, EarlierSourceWinsDuplicateAndLaterSourceContinues)
{
    std::vector<std::unique_ptr<Iterator>> sources;
    sources.push_back(source({value("alpha", "new", 30), value("gamma", "new-gamma", 50)}));
    sources.push_back(source({
        value("alpha", "old", 20),
        value("beta", "kept", 25),
        value("delta", "also-kept", 40),
    }));

    const auto merged = mergeSorted(sources);

    expectRecords(merged, {
                              value("alpha", "new", 30),
                              value("beta", "kept", 25),
                              value("delta", "also-kept", 40),
                              value("gamma", "new-gamma", 50),
                          });
}

TEST(MergeSortedTest, KeepsTombstoneFromWinningSource)
{
    std::vector<std::unique_ptr<Iterator>> sources;
    sources.push_back(source({tombstone("alpha", 9)}));
    sources.push_back(source({value("alpha", "old"), value("beta", "kept")}));

    const auto merged = mergeSorted(sources);

    expectRecords(merged, {
                              tombstone("alpha", 9),
                              value("beta", "kept"),
                          });
}
