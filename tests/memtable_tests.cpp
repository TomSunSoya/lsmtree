#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "MemTable.h"

namespace
{
void expect(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void expectGet(const MemTable &table, const std::string &key, const std::string &expected)
{
    std::string actual;
    expect(table.get(key, actual), "expected key to exist: " + key);
    expect(actual == expected, "unexpected value for key " + key + ": " + actual);
}

void expectPut(MemTable &table, const std::string &key, const std::string &value)
{
    expect(table.put(key, value), "expected put to succeed for key: " + key);
}

void expectMissing(const MemTable &table, const std::string &key)
{
    std::string actual;
    expect(!table.get(key, actual), "expected key to be missing: " + key);
}

std::string readFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    expect(in.is_open(), "expected file to exist: " + path.string());

    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string escaped(const std::string &value)
{
    std::string result;
    for (const char ch : value)
    {
        if (ch == '\n')
            result += "\\n";
        else if (ch == '\r')
            result += "\\r";
        else
            result += ch;
    }
    return result;
}

void expectFileContent(const std::filesystem::path &path, const std::string &expected)
{
    const std::string actual = readFile(path);
    expect(actual == expected,
           "unexpected file content in " + path.string()
               + ": expected \"" + escaped(expected) + "\""
               + ", actual \"" + escaped(actual) + "\"");
}

void testMemTableReadWrite()
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

void testWALAppendFormat()
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

void testWALAppendsToExistingLog()
{
    const std::filesystem::path logPath("memtable_tests_existing_append.wal");
    std::filesystem::remove(logPath);

    {
        std::ofstream seed(logPath, std::ios::binary);
        expect(seed.is_open(), "expected seed WAL to open");
        seed << "4,seed=5,value\n";
    }

    {
        MemTable table(logPath.string());
        expectPut(table, "next", "record");
    }

    expectFileContent(logPath, "4,seed=5,value\n4,next=6,record\n");

    std::filesystem::remove(logPath);
}

void testWALCreatesParentDirectories()
{
    const std::filesystem::path root("memtable_tests_nested_logs");
    const std::filesystem::path logPath = root / "child" / "wal.log";
    std::filesystem::remove_all(root);

    {
        MemTable table(logPath.string());
        expectPut(table, "parent", "created");

        expect(std::filesystem::exists(logPath), "expected WAL file in nested directory");
        expectFileContent(logPath, "6,parent=7,created\n");
    }

    std::filesystem::remove_all(root);
}

void testWALRecordsEmptyAndMultiDigitLengths()
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
}

int main()
{
    try
    {
        testMemTableReadWrite();
        testWALAppendFormat();
        testWALAppendsToExistingLog();
        testWALCreatesParentDirectories();
        testWALRecordsEmptyAndMultiDigitLengths();

        std::cout << "All MemTable tests passed" << std::endl;
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "MemTable test failed: " << error.what() << std::endl;
        return 1;
    }
}
