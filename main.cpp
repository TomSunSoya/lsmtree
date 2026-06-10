#include <iostream>
#include <stdexcept>
#include <string>

#include "inc/MemTable.h"

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

void expectMissing(const MemTable &table, const std::string &key)
{
    std::string actual;
    expect(!table.get(key, actual), "expected key to be missing: " + key);
}
}

int main()
{
    try
    {
        MemTable table;

        expectMissing(table, "missing");

        table.put("k", "12");
        expectGet(table, "k", "12");

        table.put("k", "123");
        expectGet(table, "k", "123");

        table.put("", "empty-key");
        expectGet(table, "", "empty-key");

        table.put("with spaces", "value with spaces");
        expectGet(table, "with spaces", "value with spaces");

        for (int i = 0; i < 10; ++i)
            table.put("key-" + std::to_string(i), "value-" + std::to_string(i));

        for (int i = 0; i < 10; ++i)
            expectGet(table, "key-" + std::to_string(i), "value-" + std::to_string(i));

        expectMissing(table, "key-10");

        std::cout << "All MemTable tests passed" << std::endl;
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "MemTable test failed: " << error.what() << std::endl;
        return 1;
    }
}
