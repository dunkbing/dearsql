#pragma once
#include <memory>

class SQLiteDatabase;

namespace SQLiteHierarchy {
    void renderSQLiteHierarchy(const std::shared_ptr<SQLiteDatabase> &sqliteDb);
} // namespace SQLiteHierarchy
