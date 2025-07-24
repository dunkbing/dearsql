#pragma once

class SQLiteDatabase;

namespace SQLiteHierarchy {
    void renderSQLiteHierarchy(SQLiteDatabase *sqliteDb);
    void renderTableNode(SQLiteDatabase *sqliteDb, int tableIndex);
    void renderViewNode(SQLiteDatabase *sqliteDb, int viewIndex);
} // namespace SQLiteHierarchy
