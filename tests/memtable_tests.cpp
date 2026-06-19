#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "MemTable.h"

namespace
{
size_t entrySize(const std::string_view key, const std::string_view value)
{
    return key.size() + value.size() + sizeof(uint8_t);
}

void expectGet(const MemTable &table, const std::string &key, const std::string &expected)
{
    std::string actual;
    ASSERT_TRUE(table.get(key, actual)) << "expected key to exist: " << key;
    EXPECT_EQ(expected, actual) << "unexpected value for key: " << key;
}

void expectPut(MemTable &table, const std::string &key, const std::string &value)
{
    ASSERT_TRUE(table.put(key, value)) << "expected put to succeed for key: " << key;
}

void expectRemove(MemTable &table, const std::string &key)
{
    ASSERT_TRUE(table.remove(key)) << "expected remove to succeed for key: " << key;
}

void expectMissing(const MemTable &table, const std::string &key)
{
    std::string actual;
    EXPECT_FALSE(table.get(key, actual)) << "expected key to be missing: " << key;
}

void readFile(const std::filesystem::path &path, std::string &content)
{
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.is_open()) << "expected file to exist: " << path;

    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeFile(const std::filesystem::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "expected file to open for writing: " << path;

    out << content;
    ASSERT_TRUE(out.good()) << "expected file write to succeed: " << path;
}

void expectFileContent(const std::filesystem::path &path, const std::string &expected)
{
    std::string actual;
    readFile(path, actual);
    if (::testing::Test::HasFatalFailure())
        return;

    EXPECT_EQ(expected, actual) << "unexpected file content in " << path;
}
}

TEST(MemTableTest, ReadWrite)
{
    const std::filesystem::path logPath("memtable_tests_read_write.wal");
    std::filesystem::remove(logPath);

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

    std::filesystem::remove(logPath);
}

TEST(MemTableTest, SizeBytesStartsAtZero)
{
    const std::filesystem::path logPath("memtable_tests_size_empty.wal");
    std::filesystem::remove(logPath);

    {
        const MemTable table(logPath.string());

        EXPECT_EQ(0u, table.size_bytes());
    }

    std::filesystem::remove(logPath);
}

TEST(MemTableTest, SizeBytesTracksInsertedKeysAndValues)
{
    const std::filesystem::path logPath("memtable_tests_size_insert.wal");
    std::filesystem::remove(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "one"));
        EXPECT_EQ(entrySize("alpha", "one"), table.size_bytes());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "beta", "two"));
        EXPECT_EQ(entrySize("alpha", "one") + entrySize("beta", "two"), table.size_bytes());
    }

    std::filesystem::remove(logPath);
}

TEST(MemTableTest, SizeBytesAdjustsWhenUpdatingExistingKey)
{
    const std::filesystem::path logPath("memtable_tests_size_update.wal");
    std::filesystem::remove(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "old"));
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "new-value"));

        EXPECT_EQ(entrySize("key", "new-value"), table.size_bytes());
    }

    std::filesystem::remove(logPath);
}

TEST(MemTableTest, SizeBytesHandlesEmptyKeysAndValues)
{
    const std::filesystem::path logPath("memtable_tests_size_empty_fields.wal");
    std::filesystem::remove(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "", "value"));
        EXPECT_EQ(entrySize("", "value"), table.size_bytes());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "empty-value", ""));
        EXPECT_EQ(entrySize("", "value") + entrySize("empty-value", ""), table.size_bytes());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "", ""));
        EXPECT_EQ(entrySize("", "") + entrySize("empty-value", ""), table.size_bytes());
    }

    std::filesystem::remove(logPath);
}

TEST(MemTableTest, RemoveHidesExistingKeyAndWritesTombstone)
{
    const std::filesystem::path logPath("memtable_tests_remove_tombstone.wal");
    std::filesystem::remove(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));

        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "alpha"));
        expectMissing(table, "alpha");
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,5,alpha=3,one\nD,5,alpha=0,\n"));
    std::filesystem::remove(logPath);
}

TEST(MemTableTest, RemoveMissingKeyRecordsTombstoneAndReplaysAsMissing)
{
    const std::filesystem::path logPath("memtable_tests_remove_missing_tombstone.wal");
    std::filesystem::remove(logPath);

    {
        MemTable table(logPath.string());

        expectMissing(table, "ghost");
        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "ghost"));
        expectMissing(table, "ghost");
        EXPECT_EQ(entrySize("ghost", ""), table.size_bytes());
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "D,5,ghost=0,\n"));

    {
        MemTable table(logPath.string());

        expectMissing(table, "ghost");
    }

    std::filesystem::remove(logPath);
}

TEST(MemTableTest, PutAfterRemoveRestoresKeyWithNewValue)
{
    const std::filesystem::path logPath("memtable_tests_put_after_remove.wal");
    std::filesystem::remove(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "old"));
        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "key"));
        expectMissing(table, "key");

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "key", "new"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "key", "new"));
        EXPECT_EQ(entrySize("key", "new"), table.size_bytes());
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,3,key=3,old\nD,3,key=0,\nP,3,key=3,new\n"));
    std::filesystem::remove(logPath);
}

TEST(MemTableTest, SizeBytesTracksTombstoneAfterRemove)
{
    const std::filesystem::path logPath("memtable_tests_size_after_remove.wal");
    std::filesystem::remove(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "alpha", "one"));
        EXPECT_EQ(entrySize("alpha", "one"), table.size_bytes());

        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "alpha"));
        EXPECT_EQ(entrySize("alpha", ""), table.size_bytes());
    }

    std::filesystem::remove(logPath);
}

TEST(MemTableTest, WALRestoresExistingRecords)
{
    const std::filesystem::path logPath("memtable_tests_restore_existing.wal");
    std::filesystem::remove(logPath);
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, "P,5,alpha=3,one\nP,4,beta=3,two\n"));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        ASSERT_NO_FATAL_FAILURE(expectGet(table, "beta", "two"));
        expectMissing(table, "missing");
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,5,alpha=3,one\nP,4,beta=3,two\n"));
    std::filesystem::remove(logPath);
}

TEST(MemTableTest, WALReplayKeepsLatestValueForDuplicateKey)
{
    const std::filesystem::path logPath("memtable_tests_restore_duplicate.wal");
    std::filesystem::remove(logPath);
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, "P,1,k=3,old\nP,1,k=3,new\n"));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "k", "new"));
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,1,k=3,old\nP,1,k=3,new\n"));
    std::filesystem::remove(logPath);
}

TEST(MemTableTest, WALReplayKeepsTombstoneForDeletedKey)
{
    const std::filesystem::path logPath("memtable_tests_restore_tombstone.wal");
    std::filesystem::remove(logPath);

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectPut(table, "k", "old"));
        ASSERT_NO_FATAL_FAILURE(expectRemove(table, "k"));
        expectMissing(table, "k");
    }

    {
        MemTable table(logPath.string());

        expectMissing(table, "k");
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,1,k=3,old\nD,1,k=0,\n"));
    std::filesystem::remove(logPath);
}

TEST(MemTableTest, WALTruncatesIncompleteTailDuringRestore)
{
    const std::filesystem::path logPath("memtable_tests_restore_truncate_tail.wal");
    std::filesystem::remove(logPath);
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, "P,5,alpha=3,one\nP,4,beta=3"));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        expectMissing(table, "beta");
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,5,alpha=3,one\n"));
    std::filesystem::remove(logPath);
}

TEST(MemTableTest, WALCanAppendAfterTruncatingDamagedTailAndRestoreAgain)
{
    const std::filesystem::path logPath("memtable_tests_restore_append_after_truncate.wal");
    std::filesystem::remove(logPath);
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
    std::filesystem::remove(logPath);
}

TEST(MemTableTest, WALTreatsOverflowLengthAsDamagedTail)
{
    const std::filesystem::path logPath("memtable_tests_restore_overflow_length.wal");
    std::filesystem::remove(logPath);
    ASSERT_NO_FATAL_FAILURE(writeFile(logPath, "P,5,alpha=3,one\nP,99999999999999999999999,bad=1,x\n"));

    {
        MemTable table(logPath.string());

        ASSERT_NO_FATAL_FAILURE(expectGet(table, "alpha", "one"));
        expectMissing(table, "bad");
    }

    ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,5,alpha=3,one\n"));
    std::filesystem::remove(logPath);
}

TEST(MemTableTest, WALAppendFormat)
{
    const std::filesystem::path logPath("memtable_tests_wal_append.wal");
    std::filesystem::remove(logPath);

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

    std::filesystem::remove(logPath);
}

TEST(MemTableTest, WALAppendsToExistingLog)
{
    const std::filesystem::path logPath("memtable_tests_existing_append.wal");
    std::filesystem::remove(logPath);

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

    std::filesystem::remove(logPath);
}

TEST(MemTableTest, WALCreatesParentDirectories)
{
    const std::filesystem::path root("memtable_tests_nested_logs");
    const std::filesystem::path logPath = root / "child" / "wal.log";
    std::filesystem::remove_all(root);

    {
        MemTable table(logPath.string());
        ASSERT_NO_FATAL_FAILURE(expectPut(table, "parent", "created"));

        EXPECT_TRUE(std::filesystem::exists(logPath)) << "expected WAL file in nested directory";
        ASSERT_NO_FATAL_FAILURE(expectFileContent(logPath, "P,6,parent=7,created\n"));
    }

    std::filesystem::remove_all(root);
}

TEST(MemTableTest, WALRecordsEmptyAndMultiDigitLengths)
{
    const std::filesystem::path logPath("memtable_tests_lengths.wal");
    std::filesystem::remove(logPath);

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

    std::filesystem::remove(logPath);
}
