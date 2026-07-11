#include "DB.h"

#include <algorithm>
#include <iostream>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>

#include "SSTable.h"

namespace
{
using TableNumbers = std::set<uint64_t, std::greater<>>;

constexpr uint64_t kEncodedRecordHeaderSize = sizeof(uint8_t) + 2 * sizeof(uint32_t);

void ensureDataDirectory(const std::filesystem::path& directory)
{
    if (!std::filesystem::exists(directory))
    {
        std::filesystem::create_directories(directory);
        return;
    }

    if (!std::filesystem::is_directory(directory))
        throw std::invalid_argument("Data directory is not a directory!");
}

void cleanupOldWALFiles(const std::filesystem::path& directory, const uint64_t currentFileNumber)
{
    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory))
        return;

    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (error)
            return;
        if (!entry.is_regular_file(error))
        {
            error.clear();
            continue;
        }

        const auto number = parseNumberedFile(entry.path().filename().string(), kWalPrefix, kWalSuffix);
        if (number && *number < currentFileNumber)
            removeFile(entry.path(), "remove wal file failed");
    }
}

void cleanupUntrackedSSTables(const std::filesystem::path& directory, const TableNumbers& activeTables)
{
    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory))
        return;

    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (error)
            return;
        if (!entry.is_regular_file(error))
            continue;
        if (entry.path().extension() != ".sst")
            continue;

        const auto number = parseNumberedFile(entry.path().filename().string(), kSSTablePrefix, kSSTableSuffix);
        if (!number || activeTables.contains(*number))
            continue;
        removeFile(entry.path(), "remove sst file failed");
    }
}

TableNumbers selectCompactionTables(const Manifest& manifest)
{
    const auto& levelZero = manifest.level(0);
    TableNumbers selected;
    for (const TableMeta& table : levelZero)
        selected.insert(table.number);

    std::string levelZeroMin = levelZero.front().minKey;
    std::string levelZeroMax = levelZero.front().maxKey;
    for (const TableMeta& table : levelZero)
    {
        levelZeroMin = std::min(levelZeroMin, table.minKey);
        levelZeroMax = std::max(levelZeroMax, table.maxKey);
    }

    for (const TableMeta& table : manifest.level(1))
    {
        if (table.minKey > levelZeroMax || table.maxKey < levelZeroMin)
            continue;
        selected.insert(table.number);
    }
    return selected;
}

std::vector<std::filesystem::path> tablePaths(const std::filesystem::path& dataDirectory,
                                              const TableNumbers& tableNumbers)
{
    std::vector<std::filesystem::path> paths;
    paths.reserve(tableNumbers.size());
    for (const uint64_t number : tableNumbers)
        paths.push_back(sstablePath(dataDirectory, number));
    return paths;
}

std::vector<std::unique_ptr<Iterator>> openTableIterators(const std::vector<std::filesystem::path>& paths)
{
    std::vector<std::unique_ptr<Iterator>> iterators;
    iterators.reserve(paths.size());
    for (const auto& path : paths)
        iterators.push_back(std::make_unique<SSTableIterator>(path));
    return iterators;
}

uint64_t serializedRecordSize(const Record& record)
{
    return kEncodedRecordHeaderSize + record.key.length() + record.value.length();
}

TableMeta writeCompactionSlice(Manifest& manifest, const std::filesystem::path& dataDirectory,
                               const std::span<Record> records)
{
    const uint64_t outputNumber = manifest.allocateNumber();
    SSTable::addRecordToFile(records, sstablePath(dataDirectory, outputNumber));
    return {outputNumber, records.front().key, records.back().key};
}

std::vector<TableMeta> writeCompactionOutput(Manifest& manifest, const std::filesystem::path& dataDirectory,
                                             const std::span<Record> records, const uint64_t sliceThreshold)
{
    std::vector<TableMeta> outputTables;
    uint64_t currentSliceBytes = 0;
    size_t sliceBegin = 0;

    for (size_t recordIndex = 0; recordIndex < records.size(); ++recordIndex)
    {
        currentSliceBytes += serializedRecordSize(records[recordIndex]);
        if (currentSliceBytes <= sliceThreshold)
            continue;

        outputTables.push_back(
            writeCompactionSlice(manifest, dataDirectory, records.subspan(sliceBegin, recordIndex - sliceBegin + 1)));
        currentSliceBytes = 0;
        sliceBegin = recordIndex + 1;
    }

    if (currentSliceBytes != 0)
        outputTables.push_back(writeCompactionSlice(manifest, dataDirectory, records.subspan(sliceBegin)));
    return outputTables;
}

bool overlapsScanRange(const TableMeta& table, const std::string_view start, const std::string_view end)
{
    return table.maxKey >= start && table.minKey < end;
}

std::vector<std::filesystem::path> selectScanTables(const Manifest& manifest,
                                                    const std::filesystem::path& dataDirectory,
                                                    const std::string_view start, const std::string_view end)
{
    std::vector<std::filesystem::path> paths;
    for (size_t levelNumber = 0; levelNumber < manifest.levelCount(); ++levelNumber)
    {
        for (const TableMeta& table : manifest.level(levelNumber))
        {
            if (overlapsScanRange(table, start, end))
                paths.push_back(sstablePath(dataDirectory, table.number));
        }
    }
    return paths;
}
} // namespace

DB::DB(const std::filesystem::path& dataDirectory, const uint64_t flushThreshold, const uint64_t compactThreshold,
       const uint64_t sliceThreshold)
    : flushThresholdBytes_(flushThreshold), level0CompactionThreshold_(compactThreshold),
      compactionSliceBytes_(sliceThreshold)
{
    ensureDataDirectory(dataDirectory);

    manifest_ = std::make_unique<Manifest>(dataDirectory / "MANIFEST");
    const std::filesystem::path sstableDirectory = dataDirectory / "sstable";
    const std::filesystem::path walDirectory = dataDirectory / "wal";
    SSTable::cleanupOrphanedTemps(sstableDirectory);
    cleanupUntrackedSSTables(sstableDirectory, manifest_->allTableNumbers());

    dataDirectory_ = dataDirectory;
    walFilePath_ = walPath(dataDirectory_, manifest_->logNumber());
    cleanupOldWALFiles(walDirectory, manifest_->logNumber());
    activeMemTable_ = std::make_unique<MemTable>(walFilePath_.string());
}

bool DB::put(const std::string& key, const std::string& value)
{
    if (!activeMemTable_->put(key, value))
        return false;

    try
    {
        if (activeMemTable_->size_bytes() > flushThresholdBytes_)
            flush();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << std::endl;
    }
    return true;
}

bool DB::get(const std::string_view key, std::string& value) const
{
    const Result result = activeMemTable_->get(key, value);
    if (result == Result::ABSENT)
        return searchSSTables(key, value);
    return result == Result::VALUE;
}

bool DB::remove(const std::string& key) { return activeMemTable_->remove(key); }

void DB::flush()
{
    if (activeMemTable_->size() == 0)
        return;

    const uint64_t tableNumber = manifest_->allocateNumber();
    const std::filesystem::path tablePath = sstablePath(dataDirectory_, tableNumber);
    if (!std::filesystem::exists(tablePath.parent_path()))
        std::filesystem::create_directories(tablePath.parent_path());

    const uint64_t nextWALNumber = manifest_->allocateNumber();
    manifest_->setLogNumber(nextWALNumber);

    const auto [minKey, maxKey] = SSTable::build(*activeMemTable_, tablePath);
    manifest_->addTable(tableNumber, minKey, maxKey, 0);
    manifest_->save();

    const std::filesystem::path oldWALPath = walFilePath_;
    const std::filesystem::path nextWALPath = walPath(dataDirectory_, nextWALNumber);
    activeMemTable_ = std::make_unique<MemTable>(nextWALPath.string());
    walFilePath_ = nextWALPath;

    removeFile(oldWALPath, "remove wal file failed");

    if (manifest_->level(0).size() > level0CompactionThreshold_)
        compact();
}

void DB::compact()
{
    if (manifest_->level(0).empty())
        return;

    const TableNumbers selectedTables = selectCompactionTables(*manifest_);
    const std::vector<uint64_t> removedTables(selectedTables.begin(), selectedTables.end());
    const std::vector<std::filesystem::path> inputPaths = tablePaths(dataDirectory_, selectedTables);

    auto iterators = openTableIterators(inputPaths);
    std::vector<Record> records = mergeSorted(iterators);
    const std::vector<TableMeta> outputTables =
        writeCompactionOutput(*manifest_, dataDirectory_, std::span(records), compactionSliceBytes_);

    manifest_->replaceTables(removedTables, outputTables, 1);
    manifest_->save();

    for (const auto& inputPath : inputPaths)
        removeFile(inputPath, "remove old sst file failed");
}

std::vector<Record> DB::scan(const std::string_view start, const std::string_view end) const
{
    std::vector<std::unique_ptr<Iterator>> iterators;
    iterators.push_back(std::make_unique<MemTableIterator>(*activeMemTable_));

    const auto inputPaths = selectScanTables(*manifest_, dataDirectory_, start, end);
    for (const auto& path : inputPaths)
        iterators.push_back(std::make_unique<SSTableIterator>(path.string()));

    auto visibleRecords =
        mergeSorted(iterators) |
        std::views::filter([start, end](const Record& record)
                           { return record.key >= start && record.key < end && record.type == Type::VALUE; });
    return std::ranges::to<std::vector>(visibleRecords);
}

Result DB::searchTable(const uint64_t tableNumber, const std::string_view key, std::string& value) const
{
    const SSTable table(sstablePath(dataDirectory_, tableNumber));
    return table.get(key, value);
}

bool DB::searchSSTables(const std::string_view key, std::string& value) const
{
    for (const TableMeta& table : manifest_->level(0))
    {
        if (key < table.minKey || key > table.maxKey)
            continue;

        const Result result = searchTable(table.number, key, value);
        if (result == Result::VALUE)
            return true;
        if (result == Result::TOMBSTONE)
            return false;
    }

    for (size_t levelNumber = 1; levelNumber < manifest_->levelCount(); ++levelNumber)
    {
        const auto table = manifest_->getTableMeta(levelNumber, key);
        if (!table)
            continue;

        const Result result = searchTable(table->number, key, value);
        if (result == Result::VALUE)
            return true;
        if (result == Result::TOMBSTONE)
            return false;
    }
    return false;
}
