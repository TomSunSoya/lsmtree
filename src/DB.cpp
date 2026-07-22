#include "DB.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>

#include "SSTable.h"

namespace
{
using TableNumbers = std::set<uint64_t, std::greater<>>;

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
                                              const std::vector<uint64_t>& tableNumbers)
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
    const auto fileSize = SSTable::addRecordToFile(records, sstablePath(dataDirectory, outputNumber));
    return {outputNumber, fileSize, records.front().key, records.back().key};
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
        if (recordIndex + 1 < records.size() && records[recordIndex].key == records[recordIndex + 1].key)
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
       const uint64_t sliceThreshold, const uint64_t compactBaseThresholdBytes)
    : flushThresholdBytes_(flushThreshold), level0CompactionThreshold_(compactThreshold),
      compactionSliceBytes_(sliceThreshold), compactBaseThresholdBytes_(compactBaseThresholdBytes), nextSeq_(0)
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
    nextSeq_ = std::max(manifest_->lastSeq(), activeMemTable_->getMaxWALSeq()) + 1;
}

bool DB::put(const std::string& key, const std::string& value)
{
    if (!activeMemTable_->put(key, nextSeq_++, value))
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

bool DB::get(const std::string_view key, std::string& value) const { return get(key, nextSeq_ - 1, value); }

bool DB::get(const std::string_view key, const uint64_t readSeq, std::string& value) const
{
    const Result result = activeMemTable_->get(key, readSeq, value);
    if (result == Result::ABSENT)
        return searchSSTables(key, readSeq, value);
    return result == Result::VALUE;
}

bool DB::remove(const std::string& key) { return activeMemTable_->remove(key, nextSeq_++); }

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
    manifest_->addTable(tableNumber, std::filesystem::file_size(tablePath), minKey, maxKey, 0);
    manifest_->setLastSeq(nextSeq_ - 1);
    manifest_->save();

    const std::filesystem::path oldWALPath = walFilePath_;
    const std::filesystem::path nextWALPath = walPath(dataDirectory_, nextWALNumber);
    activeMemTable_ = std::make_unique<MemTable>(nextWALPath.string());
    walFilePath_ = nextWALPath;

    removeFile(oldWALPath, "remove wal file failed");

    maybeCompact();
}

void DB::compact() { compactLevel(0); }

std::vector<Record> DB::scan(const std::string_view start, const std::string_view end, uint64_t readSeq) const
{
    std::vector<std::unique_ptr<Iterator>> iterators;
    iterators.push_back(std::make_unique<MemTableIterator>(*activeMemTable_));

    for (const auto inputPaths = selectScanTables(*manifest_, dataDirectory_, start, end);
         const auto& path : inputPaths)
        iterators.push_back(std::make_unique<SSTableIterator>(path.string()));

    if (readSeq == std::numeric_limits<uint64_t>::max())
        readSeq = nextSeq_ - 1;
    std::ranges::for_each(iterators, [&readSeq](std::unique_ptr<Iterator>& iter)
                          { iter = std::make_unique<SnapshotIterator>(std::move(iter), readSeq); });

    auto result = mergeAll(iterators);
    latestVisiblePerKey(result);
    auto visibleRecords =
        result | std::views::filter([start, end](const Record& record)
                                    { return record.key >= start && record.key < end && record.type == Type::VALUE; });
    return std::ranges::to<std::vector>(visibleRecords);
}

uint64_t DB::getSnapshot() const
{
    compactSeqs_.insert(nextSeq_ - 1);
    return nextSeq_ - 1;
}

void DB::releaseSnapshot(const uint64_t seq)
{
    auto it = compactSeqs_.find(seq);
    if (it != compactSeqs_.end())
        compactSeqs_.erase(it);
}

Snapshot DB::snapshot() { return {this, getSnapshot()}; }

size_t DB::activeSnapshotCount() const { return compactSeqs_.size(); }

Result DB::searchTable(const uint64_t tableNumber, const std::string_view key, const uint64_t readSeq,
                       std::string& value) const
{
    if (const auto iter = tables_.find(tableNumber); iter != tables_.end())
    {
        return iter->second->get(key, readSeq, value);
    }
    auto sstable = std::make_unique<SSTable>(sstablePath(dataDirectory_, tableNumber));
    const auto res = sstable->get(key, readSeq, value);
    tables_[tableNumber] = std::move(sstable);
    return res;
}

bool DB::searchSSTables(const std::string_view key, const uint64_t readSeq, std::string& value) const
{
    for (const TableMeta& table : manifest_->level(0))
    {
        if (key < table.minKey || key > table.maxKey)
            continue;

        const Result result = searchTable(table.number, key, readSeq, value);
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

        const Result result = searchTable(table->number, key, readSeq, value);
        if (result == Result::VALUE)
            return true;
        if (result == Result::TOMBSTONE)
            return false;
    }
    return false;
}

void DB::compactLevel(const uint64_t n)
{
    std::vector<uint64_t> removedTables;
    if (n == 0)
    {
        if (manifest_->level(0).empty())
            return;

        const TableNumbers selectedTables = selectCompactionTables(*manifest_);
        removedTables.assign(selectedTables.begin(), selectedTables.end());
    }
    else
    {
        auto& tables = manifest_->level(n);
        if (tables.empty())
            return;
        TableMeta table;
        if (!cursors_.contains(n))
        {
            cursors_.emplace(n, tables.front().maxKey);
            table = tables.front();
        }
        else
        {
            auto tableIt = std::ranges::upper_bound(tables, cursors_[n], std::less{}, &TableMeta::minKey);
            if (tableIt == tables.end())
                tableIt = tables.begin();
            table = *tableIt;
        }
        cursors_[n] = table.maxKey;
        std::vector<TableMeta> selectedTables;
        selectedTables.push_back(table);
        getNextCrossTable(selectedTables, n + 1);

        removedTables.reserve(selectedTables.size());
        for (const auto& removeTable : selectedTables)
            removedTables.push_back(removeTable.number);
    }

    compactRange(removedTables, n + 1);
}

void DB::maybeCompact()
{
    while (true)
    {
        if (manifest_->level(0).size() > level0CompactionThreshold_)
        {
            compactLevel(0);
            continue;
        }
        auto overLevel = getFirstOverLevel();
        if (!overLevel)
            break;
        compactLevel(*overLevel);
    }
}

void DB::compactRange(const std::vector<uint64_t>& removedTables, const uint64_t targetLevel)
{
    const std::vector<std::filesystem::path> inputPaths = tablePaths(dataDirectory_, removedTables);
    auto iterators = openTableIterators(inputPaths);

    std::vector<Record> records = mergeAll(iterators);
    retainForCompaction(records, smallestActiveSnapShot());
    const std::vector<TableMeta> outputTables =
        writeCompactionOutput(*manifest_, dataDirectory_, std::span(records), compactionSliceBytes_);

    manifest_->replaceTables(removedTables, outputTables, targetLevel);
    manifest_->save();

    for (const auto& inputPath : inputPaths)
        removeFile(inputPath, "remove old sst file failed");

    for (auto tableNumber : removedTables)
        tables_.erase(tableNumber);
}

uint64_t DB::levelBytes(const uint64_t level) const
{
    auto& tables = manifest_->level(level);
    uint64_t totalBytes = 0;
    for (const auto& table : tables)
        totalBytes += table.size;
    return totalBytes;
}

uint64_t DB::budgetFor(const uint64_t n) const
{
    uint64_t totalBytes = compactBaseThresholdBytes_;
    for (uint64_t i = 2; i <= n; ++i)
        totalBytes *= 10;
    return totalBytes;
}

std::optional<uint64_t> DB::getFirstOverLevel() const
{
    for (uint64_t i = 1; i < manifest_->levelCount(); ++i)
    {
        if (budgetFor(i) < levelBytes(i))
            return i;
    }
    return std::nullopt;
}

void DB::getNextCrossTable(std::vector<TableMeta>& tables, const uint64_t nextLevel) const
{
    if (nextLevel >= manifest_->levelCount())
        return;

    auto& nextLevelTables = manifest_->level(nextLevel);
    tables.reserve(nextLevelTables.size());
    const auto curTable = tables.front();
    for (const auto& table : nextLevelTables)
    {
        if (rangesOverlap(table, curTable.minKey, curTable.maxKey))
            tables.push_back(table);
    }
}

uint64_t DB::smallestActiveSnapShot() const { return compactSeqs_.empty() ? nextSeq_ - 1 : *compactSeqs_.begin(); }

Snapshot::Snapshot(Snapshot&& other) noexcept
{
    db_ = other.db_;
    seq_ = other.seq_;
    other.db_ = nullptr;
}

Snapshot& Snapshot::operator=(Snapshot&& other) noexcept
{
    if (&other == this)
        return *this;
    if (db_)
        db_->releaseSnapshot(seq_);
    db_ = other.db_;
    seq_ = other.seq_;
    other.db_ = nullptr;
    return *this;
}

Snapshot::~Snapshot()
{
    if (db_)
        db_->releaseSnapshot(seq_);
}

uint64_t Snapshot::seq() const { return seq_; }

Snapshot::Snapshot(DB* db, const uint64_t seq) : db_(db), seq_(seq) {}
