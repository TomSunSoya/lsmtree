#pragma once

#include <cstdint>
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

#include "utils.h"

namespace test_support
{
inline constexpr std::string_view kWalHeader{"LWAL\x02", 5};
inline constexpr std::string_view kWalFrameMagic{"WFRM", 4};
inline constexpr std::string_view kWalCommitMagic{"WCMT", 4};

template <typename Value> void appendValue(std::string& output, const Value& value)
{
    output.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

inline std::string walFrame(const std::string_view payload)
{
    std::string frame(kWalFrameMagic);
    appendValue(frame, static_cast<uint64_t>(payload.size()));
    frame.append(payload);
    appendValue(frame, crc32(payload));
    frame.append(kWalCommitMagic);
    return frame;
}

inline std::string walContent() { return std::string(kWalHeader); }

inline std::string walContent(const std::string_view payload)
{
    std::string content(kWalHeader);
    content += walFrame(payload);
    return content;
}

inline std::string walContent(const std::initializer_list<std::string_view> payloads)
{
    std::string content(kWalHeader);
    for (const std::string_view payload : payloads)
        content += walFrame(payload);
    return content;
}

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
