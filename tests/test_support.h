#pragma once

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace test_support
{
class ScopedPathCleanup
{
  public:
    explicit ScopedPathCleanup(std::filesystem::path path) : paths_{std::move(path)}
    {
        for (const auto& cleanupPath : paths_)
            std::filesystem::remove_all(cleanupPath);
    }

    ScopedPathCleanup(std::initializer_list<std::filesystem::path> paths) : paths_(paths)
    {
        for (const auto& cleanupPath : paths_)
            std::filesystem::remove_all(cleanupPath);
    }

    ~ScopedPathCleanup()
    {
        for (const auto& cleanupPath : paths_)
        {
            std::error_code error;
            std::filesystem::remove_all(cleanupPath, error);
        }
    }

  private:
    std::vector<std::filesystem::path> paths_;
};

inline void readFile(const std::filesystem::path& path, std::string& content)
{
    std::ifstream input(path, std::ios::binary);
    ASSERT_TRUE(input.is_open()) << "expected file to exist: " << path;

    content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

inline void writeFile(const std::filesystem::path& path, std::string_view content)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open()) << "expected file to open for writing: " << path;

    output << content;
    ASSERT_TRUE(output.good()) << "expected file write to succeed: " << path;
}

inline void expectFileContent(const std::filesystem::path& path, std::string_view expected)
{
    std::string actual;
    readFile(path, actual);
    if (::testing::Test::HasFatalFailure())
        return;

    EXPECT_EQ(expected, std::string_view(actual)) << "unexpected file content in " << path;
}
} // namespace test_support
