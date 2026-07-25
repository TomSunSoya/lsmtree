#pragma once

#include "utils.h"
#include <vector>

// DB::write() does not clear records; call clear() explicitly before reusing a batch.
class WriteBatch
{
  public:
    WriteBatch() = default;
    void put(const std::string& key, const std::string& value);
    void remove(const std::string& key);
    void clear();

  private:
    struct BatchRecord
    {
        std::string key;
        Type type;
        std::string value;
    };
    std::vector<BatchRecord> records_;
    friend class DB;
};
