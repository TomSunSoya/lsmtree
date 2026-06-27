#include <cstdint>
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

void readFile(const std::filesystem::path &path, std::string &content)
{
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.is_open()) << "expected file to exist: " << path;

    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void expectTables(const Manifest &manifest, std::set<uint64_t, std::greater<>> expected)
{
    EXPECT_EQ(expected, manifest.tables());
}
}

TEST(ManifestTest, MissingManifestStartsWithEmptyTableSet)
{
    const std::filesystem::path root("manifest_tests_missing");
    const ScopedPathCleanup cleanup(root);
    ASSERT_TRUE(std::filesystem::create_directories(root));

    const Manifest manifest(root / "MANIFEST");

    EXPECT_TRUE(manifest.tables().empty());
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

        manifest.addTable(first);
        manifest.addTable(second);
        ASSERT_NO_THROW(manifest.save());
    }

    Manifest reloaded(manifestPath);
    expectTables(reloaded, {1, 0});
    EXPECT_EQ(2, reloaded.nextNumber());
    EXPECT_EQ(2, reloaded.allocateNumber());
}

TEST(ManifestTest, StaleTemporaryFileDoesNotOverrideSavedManifest)
{
    const std::filesystem::path root("manifest_tests_stale_tmp");
    const ScopedPathCleanup cleanup(root);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    const std::filesystem::path manifestPath = root / "MANIFEST";
    const std::filesystem::path tempPath(manifestPath.string() + ".tmp");

    ASSERT_NO_FATAL_FAILURE(writeFile(manifestPath, "version:0\nnext:8\ntable:7\n"));
    ASSERT_NO_FATAL_FAILURE(writeFile(tempPath, "version:0\nnext:1000\ntable:999\n"));

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
        manifest.addTable(4);
        ASSERT_NO_THROW(manifest.save());
    }

    EXPECT_FALSE(std::filesystem::exists(tempPath)) << "save should publish by renaming MANIFEST.tmp over MANIFEST";

    std::string content;
    ASSERT_NO_FATAL_FAILURE(readFile(manifestPath, content));
    EXPECT_EQ("log:0\nversion:0\nnext:0\ntable:4\n", content);
}

TEST(ManifestTest, ReplaceTablesPersistsCompactionResult)
{
    const std::filesystem::path root("manifest_tests_replace_tables");
    const ScopedPathCleanup cleanup(root);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    const std::filesystem::path manifestPath = root / "MANIFEST";

    {
        Manifest manifest(manifestPath);
        manifest.addTable(5);
        manifest.addTable(3);
        manifest.addTable(1);

        manifest.replaceTables({5, 3}, 7);
        expectTables(manifest, {7, 1});
        ASSERT_NO_THROW(manifest.save());
    }

    const Manifest reloaded(manifestPath);
    expectTables(reloaded, {7, 1});
}
