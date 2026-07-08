#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Manifest.h"

namespace
{
class ScopedPathCleanup
{
public:
    explicit ScopedPathCleanup(std::filesystem::path path) : path(std::move(path))
    {
        std::filesystem::remove_all(this->path);
    }

    ~ScopedPathCleanup()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

private:
    std::filesystem::path path;
};

void writeFile(const std::filesystem::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "expected file to open for writing: " << path;

    out << content;
    ASSERT_TRUE(out.good()) << "expected file write to succeed: " << path;
}

void expectTables(const Manifest &manifest, std::set<uint64_t, std::greater<>> expected)
{
    EXPECT_EQ(expected, manifest.allTableNumbers());
}

void expectBytesEqual(const std::string &expected, const std::string &actual)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ(static_cast<unsigned char>(expected[i]), static_cast<unsigned char>(actual[i])) << "byte offset " << i;
}
}

TEST(ManifestTest, MissingManifestStartsWithEmptyTableSet)
{
    const std::filesystem::path root("manifest_tests_missing");
    const ScopedPathCleanup cleanup(root);
    ASSERT_TRUE(std::filesystem::create_directories(root));

    const Manifest manifest(root / "MANIFEST");

    EXPECT_TRUE(manifest.allTableNumbers().empty());
    EXPECT_EQ(0, manifest.nextNumber());
}

TEST(ManifestTest, SaveAndReloadPreservesTablesAndNextNumber)
{
    const std::filesystem::path root("manifest_tests_save_reload");
    const ScopedPathCleanup cleanup(root);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    const std::filesystem::path manifestPath = root / "MANIFEST";

    {
        Manifest manifest(manifestPath);
        const auto first = manifest.allocateNumber();
        const auto second = manifest.allocateNumber();
        EXPECT_EQ(0, first);
        EXPECT_EQ(1, second);

        manifest.addTable(first, "apple", "mango");
        manifest.addTable(second, "nectarine", "zucchini");
        ASSERT_NO_THROW(manifest.save());
    }

    Manifest reloaded(manifestPath);
    expectTables(reloaded, {1, 0});
    ASSERT_EQ(2, reloaded.level(0).size());
    EXPECT_EQ("nectarine", reloaded.level(0)[0].minKey);
    EXPECT_EQ("zucchini", reloaded.level(0)[0].maxKey);
    EXPECT_EQ("apple", reloaded.level(0)[1].minKey);
    EXPECT_EQ("mango", reloaded.level(0)[1].maxKey);
    EXPECT_EQ(2, reloaded.nextNumber());
    EXPECT_EQ(2, reloaded.allocateNumber());
}

TEST(ManifestTest, SaveAndReloadPreservesArbitraryKeyBytes)
{
    const std::filesystem::path root("manifest_tests_arbitrary_key_bytes");
    const ScopedPathCleanup cleanup(root);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    const std::filesystem::path manifestPath = root / "MANIFEST";
    const std::string newlineMin = "a\nb";
    const std::string colonMax = "c:d";
    const std::string nulMin("e\0f", 3);
    const std::string mixedMax("g\nh:i\0j", 7);

    {
        Manifest manifest(manifestPath);
        manifest.addTable(9, newlineMin, colonMax);
        manifest.addTable(8, nulMin, mixedMax);
        ASSERT_NO_THROW(manifest.save());
    }

    const Manifest reloaded(manifestPath);
    const auto &level = reloaded.level(0);
    ASSERT_EQ(2, level.size());
    EXPECT_EQ(9, level[0].number);
    EXPECT_EQ(8, level[1].number);
    expectBytesEqual(newlineMin, level[0].minKey);
    expectBytesEqual(colonMax, level[0].maxKey);
    expectBytesEqual(nulMin, level[1].minKey);
    expectBytesEqual(mixedMax, level[1].maxKey);
}

TEST(ManifestTest, StaleTemporaryFileDoesNotOverrideSavedManifest)
{
    const std::filesystem::path root("manifest_tests_stale_tmp");
    const ScopedPathCleanup cleanup(root);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    const std::filesystem::path manifestPath = root / "MANIFEST";
    const std::filesystem::path tempPath(manifestPath.string() + ".tmp");

    {
        Manifest manifest(manifestPath);
        for (uint64_t i = 0; i < 8; ++i)
            EXPECT_EQ(i, manifest.allocateNumber());
        manifest.addTable(7, "first", "last");
        ASSERT_NO_THROW(manifest.save());
    }
    ASSERT_NO_FATAL_FAILURE(writeFile(tempPath, "stale temporary manifest"));

    const Manifest reloaded(manifestPath);
    expectTables(reloaded, {7});
    EXPECT_EQ(8, reloaded.nextNumber());
    EXPECT_TRUE(std::filesystem::is_regular_file(tempPath));
}

TEST(ManifestTest, SavePublishesThroughTemporaryFileAndRemovesStaleTemp)
{
    const std::filesystem::path root("manifest_tests_save_uses_tmp");
    const ScopedPathCleanup cleanup(root);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    const std::filesystem::path manifestPath = root / "MANIFEST";
    const std::filesystem::path tempPath(manifestPath.string() + ".tmp");
    ASSERT_NO_FATAL_FAILURE(writeFile(tempPath, "version:0\nnext:99\ntable:99\n"));

    {
        Manifest manifest(manifestPath);
        manifest.addTable(4, "first", "last");
        ASSERT_NO_THROW(manifest.save());
    }

    EXPECT_FALSE(std::filesystem::exists(tempPath)) << "save should publish by renaming MANIFEST.tmp over MANIFEST";
    const Manifest reloaded(manifestPath);
    expectTables(reloaded, {4});
}

TEST(ManifestTest, ReplaceTablesPersistsCompactionResult)
{
    const std::filesystem::path root("manifest_tests_replace_tables");
    const ScopedPathCleanup cleanup(root);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    const std::filesystem::path manifestPath = root / "MANIFEST";

    {
        Manifest manifest(manifestPath);
        manifest.addTable(5, "a", "c");
        manifest.addTable(3, "d", "f");
        manifest.addTable(1, "g", "i");

        manifest.replaceTables({5, 3}, 7, "a", "f");
        expectTables(manifest, {7, 1});
        ASSERT_NO_THROW(manifest.save());
    }

    const Manifest reloaded(manifestPath);
    expectTables(reloaded, {7, 1});
    ASSERT_EQ(2, reloaded.level(0).size());
    EXPECT_EQ(7, reloaded.level(0)[0].number);
    EXPECT_EQ("a", reloaded.level(0)[0].minKey);
    EXPECT_EQ("f", reloaded.level(0)[0].maxKey);
}
