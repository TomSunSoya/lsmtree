#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include "MemTable.h"

namespace
{
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

void expectMissing(const MemTable &table, const std::string &key)
{
    std::string actual;
    EXPECT_FALSE(table.get(key, actual)) << "expected key to be missing: " << key;
}

std::string readFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.is_open()) << "expected file to exist: " << path;

    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void expectFileContent(const std::filesystem::path &path, const std::string &expected)
{
    EXPECT_EQ(expected, readFile(path)) << "unexpected file content in " << path;
}
}

TEST(MemTableTest, ReadWrite)
{
    const std::filesystem::path logPath("memtable_tests_read_write.wal");
    std::filesystem::remove(logPath);

    {
        MemTable table(logPath.string());

        expectMissing(table, "missing");

        expectPut(table, "k", "12");
        expectGet(table, "k", "12");

        expectPut(table, "k", "123");
        expectGet(table, "k", "123");

        expectPut(table, "", "empty-key");
        expectGet(table, "", "empty-key");

        expectPut(table, "with spaces", "value with spaces");
        expectGet(table, "with spaces", "value with spaces");

        for (int i = 0; i < 10; ++i)
            expectPut(table, "key-" + std::to_string(i), "value-" + std::to_string(i));

        for (int i = 0; i < 10; ++i)
            expectGet(table, "key-" + std::to_string(i), "value-" + std::to_string(i));

        expectMissing(table, "key-10");
    }

    std::filesystem::remove(logPath);
}

TEST(MemTableTest, WALAppendFormat)
{
    const std::filesystem::path logPath("memtable_tests_wal_append.wal");
    std::filesystem::remove(logPath);

    {
        MemTable table(logPath.string());
        std::string expected;

        expectPut(table, "alpha", "one");
        expected += "5,alpha=3,one\n";
        expectFileContent(logPath, expected);

        expectPut(table, "beta", "two");
        expected += "4,beta=3,two\n";
        expectFileContent(logPath, expected);

        expectPut(table, "a,b=c", "v=1,ok");
        expected += "5,a,b=c=6,v=1,ok\n";
        expectFileContent(logPath, expected);

        expectPut(table, "a", "vvv\n\n\rvvv");
        expected += "1,a=9,vvv\n\n\rvvv\n";
        expectFileContent(logPath, expected);
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
        seed << "4,seed=5,value\n";
    }

    {
        MemTable table(logPath.string());
        expectPut(table, "next", "record");
    }

    expectFileContent(logPath, "4,seed=5,value\n4,next=6,record\n");

    std::filesystem::remove(logPath);
}

TEST(MemTableTest, WALCreatesParentDirectories)
{
    const std::filesystem::path root("memtable_tests_nested_logs");
    const std::filesystem::path logPath = root / "child" / "wal.log";
    std::filesystem::remove_all(root);

    {
        MemTable table(logPath.string());
        expectPut(table, "parent", "created");

        EXPECT_TRUE(std::filesystem::exists(logPath)) << "expected WAL file in nested directory";
        expectFileContent(logPath, "6,parent=7,created\n");
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

        expectPut(table, "empty-value", "");
        expected += "11,empty-value=0,\n";
        expectFileContent(logPath, expected);
        expectGet(table, "empty-value", "");

        expectPut(table, "tenletters", "0123456789abc");
        expected += "10,tenletters=13,0123456789abc\n";
        expectFileContent(logPath, expected);
        expectGet(table, "tenletters", "0123456789abc");

        expectPut(table, "", "");
        expected += "0,=0,\n";
        expectFileContent(logPath, expected);
        expectGet(table, "", "");
    }

    std::filesystem::remove(logPath);
}
