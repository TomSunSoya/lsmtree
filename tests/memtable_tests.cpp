#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "MemTable.h"
#include "test_support.h"

namespace
{
using test_support::expectFileContent;
using test_support::ScopedPathCleanup;
using test_support::writeFile;

size_t entrySize(const std::string_view key, const std::string_view value)
{
    return key.size() + value.size() + sizeof(uint8_t);
}

void expectGet(const MemTable& table, const std::string& key, const std::string& expected)
{
    std::string actual;
    ASSERT_EQ(Result::VALUE, table.get(key, actual)) << "expected key to exist: " << key;
    EXPECT_EQ(expected, actual) << "unexpected value for key: " << key;
}

void expectPut(MemTable& table, const std::string& key, const std::string& value)
{
    ASSERT_TRUE(table.put(key, value)) << "expected put to succeed for key: " << key;
}

void expectRemove(MemTable& table, const std::string& key)
{
    ASSERT_TRUE(table.remove(key)) << "expected remove to succeed for key: " << key;
}

void expectMissing(const MemTable& table, const std::string& key)
{
    std::string actual;
    EXPECT_NE(Result::VALUE, table.get(key, actual)) << "expected key to be missing: " << key;
}

void expectTombstone(const MemTable& table, const std::string& key)
{
    std::string actual = "unchanged";
    EXPECT_EQ(Result::TOMBSTONE, table.get(key, actual)) << "expected tombstone for key: " << key;
}

void expectIteratorRecord(const Iterator& iterator, const std::string& key, const Type type, const std::string& value)
{
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(key, iterator.current().key);
    EXPECT_EQ(type, iterator.current().type);
    EXPECT_EQ(value, iterator.current().value);
}

} // namespace

TEST(MemTableTest, ReadWrite)
{
    const std::filesystem::path logPath("memtable_tests_read_write.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        expectMissing(table, "missing");

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "k", "12"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "k", "12"));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "k", "123"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "k", "123"));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "", "empty-key"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "", "empty-key"));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "with spaces", "value with spaces"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "with spaces", "value with spaces"));

        for (int i = 0; i < 10; ++i)
        {
            ASSERT_NO_FATAL_FAILURE(expectPut(table, "key-" + std::to_string(i), "value-" + std::to_string(i)));
        }

        for (int i = 0; i < 10; ++i)
        {
            ASSERT_NO_FATAL_FAILURE(expectGet(table, "key-" + std::to_string(i), "value-" + std::to_string(i)));
        }

        expectMissing(table, "key-10");
    }
}

TEST(MemTableTest, SizeBytesStartsAtZero)
{
    const std::filesystem::path logPath("memtable_tests_size_empty.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        const MemTable table(logPath.string());

        EXPECT_EQ(0u, table.size_bytes());
    }
}

TEST(MemTableTest, SizeBytesTracksInsertedKeysAndValues)
{
    const std::filesystem::path logPath("memtable_tests_size_insert.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "one"));
        EXPECT_EQ(entrySize("alpha", "one"), table.size_bytes());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "beta", "two"));
        EXPECT_EQ(entrySize("alpha", "one") + entrySize("beta", "two"), table.size_bytes());
    }
}

TEST(MemTableTest, SizeBytesAdjustsWhenUpdatingExistingKey)
{
    const std::filesystem::path logPath("memtable_tests_size_update.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "old"));
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "new-value"));

        EXPECT_EQ(entrySize("key", "new-value"), table.size_bytes());
    }
}

TEST(MemTableTest, SizeBytesHandlesEmptyKeysAndValues)
{
    const std::filesystem::path logPath("memtable_tests_size_empty_fields.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "", "value"));
        EXPECT_EQ(entrySize("", "value"), table.size_bytes());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "empty-value", ""));
        EXPECT_EQ(entrySize("", "value") + entrySize("empty-value", ""), table.size_bytes());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "", ""));
        EXPECT_EQ(entrySize("", "") + entrySize("empty-value", ""), table.size_bytes());
    }
}

TEST(MemTableTest, RemoveHidesExistingKeyAndWritesTombstone)
{
    const std::filesystem::path logPath("memtable_tests_remove_tombstone.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));

        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "alpha"));
        expectTombstone(table, "alpha");
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,5,alpha=3,one\nD,5,alpha=0,\n"));
}

TEST(MemTableTest, RemoveMissingKeyRecordsTombstoneAndReplaysAsMissing)
{
    const std::filesystem::path logPath("memtable_tests_remove_missing_tombstone.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        expectMissing(table, "ghost");
        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "ghost"));
        expectTombstone(table, "ghost");
        EXPECT_EQ(entrySize("ghost", ""), table.size_bytes());
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "D,5,ghost=0,\n"));

    {
        MemTable table(logPath.string());

        expectTombstone(table, "ghost");
    }
}

TEST(MemTableTest, PutAfterRemoveRestoresKeyWithNewValue)
{
    const std::filesystem::path logPath("memtable_tests_put_after_remove.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "old"));
        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "key"));
        expectTombstone(table, "key");

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "new"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "key", "new"));
        EXPECT_EQ(entrySize("key", "new"), table.size_bytes());
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,3,key=3,old\nD,3,key=0,\nP,3,key=3,new\n"));
}

TEST(MemTableTest, SizeBytesTracksTombstoneAfterRemove)
{
    const std::filesystem::path logPath("memtable_tests_size_after_remove.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "one"));
        EXPECT_EQ(entrySize("alpha", "one"), table.size_bytes());

        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "alpha"));
        EXPECT_EQ(entrySize("alpha", ""), table.size_bytes());
    }
}

TEST(MemTableTest, WALRestoresExistingRecords)
{
    const std::filesystem::path logPath("memtable_tests_restore_existing.wal");
    const ScopedPathCleanup cleanup(logPath);
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, "P,5,alpha=3,one\nP,4,beta=3,two\n"));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "beta", "two"));
        expectMissing(table, "missing");
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,5,alpha=3,one\nP,4,beta=3,two\n"));
}

TEST(MemTableTest, WALReplayKeepsLatestValueForDuplicateKey)
{
    const std::filesystem::path logPath("memtable_tests_restore_duplicate.wal");
    const ScopedPathCleanup cleanup(logPath);
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, "P,1,k=3,old\nP,1,k=3,new\n"));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "k", "new"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,1,k=3,old\nP,1,k=3,new\n"));
}

TEST(MemTableTest, WALReplayKeepsTombstoneForDeletedKey)
{
    const std::filesystem::path logPath("memtable_tests_restore_tombstone.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "k", "old"));
        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "k"));
        expectTombstone(table, "k");
    }

    {
        MemTable table(logPath.string());

        expectTombstone(table, "k");
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,1,k=3,old\nD,1,k=0,\n"));
}

TEST(MemTableTest, WALTruncatesIncompleteTailDuringRestore)
{
    const std::filesystem::path logPath("memtable_tests_restore_truncate_tail.wal");
    const ScopedPathCleanup cleanup(logPath);
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, "P,5,alpha=3,one\nP,4,beta=3"));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        expectMissing(table, "beta");
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,5,alpha=3,one\n"));
}

TEST(MemTableTest, WALCanAppendAfterTruncatingDamagedTailAndRestoreAgain)
{
    const std::filesystem::path logPath("memtable_tests_restore_append_after_truncate.wal");
    const ScopedPathCleanup cleanup(logPath);
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, "P,5,alpha=3,one\nP,4,beta=3"));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        expectMissing(table, "beta");
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "gamma", "three"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "gamma", "three"));
    }

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "gamma", "three"));
        expectMissing(table, "beta");
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,5,alpha=3,one\nP,5,gamma=5,three\n"));
}

TEST(MemTableTest, WALTreatsOverflowLengthAsDamagedTail)
{
    const std::filesystem::path logPath("memtable_tests_restore_overflow_length.wal");
    const ScopedPathCleanup cleanup(logPath);
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, "P,5,alpha=3,one\nP,99999999999999999999999,bad=1,x\n"));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        expectMissing(table, "bad");
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,5,alpha=3,one\n"));
}

TEST(MemTableTest, WALAppendFormat)
{
    const std::filesystem::path logPath("memtable_tests_wal_append.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());
        std::string expected;

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "one"));
        expected += "P,5,alpha=3,one\n";
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "beta", "two"));
        expected += "P,4,beta=3,two\n";
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "a,b=c", "v=1,ok"));
        expected += "P,5,a,b=c=6,v=1,ok\n";
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "a", "vvv\n\n\rvvv"));
        expected += "P,1,a=9,vvv\n\n\rvvv\n";
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));
    }
}

TEST(MemTableTest, WALAppendsToExistingLog)
{
    const std::filesystem::path logPath("memtable_tests_existing_append.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        std::ofstream seed(logPath, std::ios::binary);
        ASSERT_TRUE(seed.is_open()) << "expected seed WAL to open";
        seed << "P,4,seed=5,value\n";
    }

    {
        MemTable table(logPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "next", "record"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,4,seed=5,value\nP,4,next=6,record\n"));
}

TEST(MemTableTest, WALCreatesParentDirectories)
{
    const std::filesystem::path root("memtable_tests_nested_logs");
    const ScopedPathCleanup cleanup(root);
    const std::filesystem::path logPath = root / "child" / "wal.log";

    {
        MemTable table(logPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "parent", "created"));

        EXPECT_TRUE(std::filesystem::exists(logPath)) << "expected WAL file in nested directory";
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,6,parent=7,created\n"));
    }
}

TEST(MemTableTest, WALRecordsEmptyAndMultiDigitLengths)
{
    const std::filesystem::path logPath("memtable_tests_lengths.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());
        std::string expected;

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "empty-value", ""));
        expected += "P,11,empty-value=0,\n";
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "empty-value", ""));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "tenletters", "0123456789abc"));
        expected += "P,10,tenletters=13,0123456789abc\n";
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "tenletters", "0123456789abc"));

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "", ""));
        expected += "P,0,=0,\n";
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, expected));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "", ""));
    }
}

TEST(MemTableTest, IteratorIsInvalidForEmptyTable)
{
    const std::filesystem::path logPath("memtable_tests_iterator_empty.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        const MemTable table(logPath.string());
        const MemTableIterator iterator(table);

        EXPECT_FALSE(iterator.valid());
    }
}

TEST(MemTableTest, IteratorTraversesSortedValuesAndTombstonesThroughBaseInterface)
{
    const std::filesystem::path logPath("memtable_tests_iterator_records.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "gamma", "three"));
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "beta", "two"));
        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "beta"));

        MemTableIterator concreteIterator(table);
        Iterator& iterator = concreteIterator;

        ASSERT_NO_FATAL_FAILURE(expectIteratorRecord(iterator, "alpha", Type::VALUE, "one"));
        iterator.advance();
        ASSERT_NO_FATAL_FAILURE(expectIteratorRecord(iterator, "beta", Type::TOMBSTONE, ""));
        iterator.advance();
        ASSERT_NO_FATAL_FAILURE(expectIteratorRecord(iterator, "gamma", Type::VALUE, "three"));
        iterator.advance();
        EXPECT_FALSE(iterator.valid());
    }
}

TEST(MemTableTest, IteratorBecomesInvalidAfterLastRecord)
{
    const std::filesystem::path logPath("memtable_tests_iterator_past_end.wal");
    const ScopedPathCleanup cleanup(logPath);

    {
        MemTable table(logPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "only", "value"));

        MemTableIterator iterator(table);
        ASSERT_TRUE(iterator.valid());
        iterator.advance();
        EXPECT_FALSE(iterator.valid());
    }
}
