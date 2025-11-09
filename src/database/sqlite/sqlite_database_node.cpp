#include "database/sqlite/sqlite_database_node.hpp"
#include "database/db.hpp"
#include "database/sqlite.hpp"
#include "utils/logger.hpp"
#include <chrono>
#include <format>
#include <future>
#include <soci/soci.h>
#include <soci/sqlite3/soci-sqlite3.h>

void SQLiteDatabaseNode::checkTablesStatusAsync() {
    // For SQLite, loading is synchronous, so this is a no-op
    if (tablesFuture.valid() &&
        tablesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            tables = tablesFuture.get();
            Logger::info(std::format("Table loading completed. Found {} tables", tables.size()));
            tablesLoaded = true;
            loadingTables = false;
        } catch (const std::exception& e) {
            Logger::error(std::format("Error in table loading: {}", e.what()));
            lastTablesError = e.what();
            tablesLoaded = true;
            loadingTables = false;
        }
    }
}

void SQLiteDatabaseNode::startTablesLoadAsync() {
    Logger::debug("startTablesLoadAsync for SQLite database");
    if (!parentDb) {
        return;
    }

    // Don't start if already loading or loaded
    if (loadingTables.load() || tablesLoaded) {
        return;
    }

    loadingTables = true;

    // Start async loading
    tablesFuture = std::async(std::launch::async, [this]() { return getTablesAsync(); });
}

std::vector<Table> SQLiteDatabaseNode::getTablesAsync() {
    std::vector<Table> result;

    // Check if we're still supposed to be loading
    if (!loadingTables.load()) {
        return result;
    }

    try {
        // Use parent database connection
        if (!parentDb || !parentDb->isConnected()) {
            Logger::error("Parent database not connected");
            return result;
        }

        // Get table names
        std::vector<std::string> tableNames;
        const std::string tableNamesQuery =
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'";
        {
            auto session = parentDb->getSession();
            if (!session) {
                Logger::error("Failed to get database session");
                return result;
            }

            const soci::rowset tableRs = session->prepare << tableNamesQuery;
            for (const auto& row : tableRs) {
                if (!loadingTables.load()) {
                    return result;
                }
                tableNames.push_back(convertRowValue(row, 0));
            }
        }

        Logger::debug("Found " + std::to_string(tableNames.size()) + " tables in database");

        if (tableNames.empty() || !loadingTables.load()) {
            return result;
        }

        // Load table details
        for (const auto& tableName : tableNames) {
            if (!loadingTables.load()) {
                break;
            }

            Table table;
            table.name = tableName;
            table.fullName = parentDb->getName() + "." + tableName;

            // Get table columns
            const std::string columnsQuery = std::format("PRAGMA table_info({})", tableName);
            {
                auto session = parentDb->getSession();
                if (!session) {
                    continue;
                }

                const soci::rowset colRs = session->prepare << columnsQuery;
                for (const auto& row : colRs) {
                    Column col;
                    col.name = convertRowValue(row, 1);                // name
                    col.type = convertRowValue(row, 2);                // type
                    col.isNotNull = convertRowValue(row, 3) == "1";    // notnull
                    col.isPrimaryKey = convertRowValue(row, 5) == "1"; // pk

                    table.columns.push_back(col);
                }
            }

            // Get foreign keys
            const std::string fkQuery = std::format("PRAGMA foreign_key_list({})", tableName);
            {
                auto session = parentDb->getSession();
                if (session) {
                    const soci::rowset fkRs = session->prepare << fkQuery;
                    for (const auto& row : fkRs) {
                        ForeignKey fk;
                        fk.name = "";                              // SQLite doesn't name FKs
                        fk.targetTable = convertRowValue(row, 2);  // table
                        fk.sourceColumn = convertRowValue(row, 3); // from
                        fk.targetColumn = convertRowValue(row, 4); // to

                        table.foreignKeys.push_back(fk);
                    }
                }
            }

            // Get indexes
            const std::string indexQuery = std::format("PRAGMA index_list({})", tableName);
            {
                auto session = parentDb->getSession();
                if (session) {
                    const soci::rowset idxRs = session->prepare << indexQuery;
                    for (const auto& row : idxRs) {
                        Index idx;
                        idx.name = convertRowValue(row, 1);            // name
                        idx.isUnique = convertRowValue(row, 2) == "1"; // unique

                        // Get index columns
                        const std::string idxInfoQuery =
                            std::format("PRAGMA index_info({})", idx.name);
                        auto session2 = parentDb->getSession();
                        if (session2) {
                            const soci::rowset idxInfoRs = session2->prepare << idxInfoQuery;
                            for (const auto& infoRow : idxInfoRs) {
                                idx.columns.push_back(convertRowValue(infoRow, 2)); // name
                            }
                        }

                        table.indexes.push_back(idx);
                    }
                }
            }

            // Build foreign key lookup
            buildForeignKeyLookup(table);

            result.push_back(table);
        }

        // Populate incoming foreign keys
        populateIncomingForeignKeys(result);

        Logger::info("Finished loading tables. Total tables: " + std::to_string(result.size()));
    } catch (const std::exception& e) {
        Logger::error(std::format("Error loading tables: {}", e.what()));
    }

    return result;
}

void SQLiteDatabaseNode::checkViewsStatusAsync() {
    if (viewsFuture.valid() &&
        viewsFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            views = viewsFuture.get();
            Logger::info(std::format("View loading completed. Found {} views", views.size()));
            viewsLoaded = true;
            loadingViews = false;
        } catch (const std::exception& e) {
            Logger::error(std::format("Error in view loading: {}", e.what()));
            lastViewsError = e.what();
            viewsLoaded = true;
            loadingViews = false;
        }
    }
}

void SQLiteDatabaseNode::startViewsLoadAsync() {
    Logger::debug("startViewsLoadAsync for SQLite database");
    if (!parentDb) {
        return;
    }

    // Don't start if already loading or loaded
    if (loadingViews.load() || viewsLoaded) {
        return;
    }

    loadingViews = true;

    // Start async loading
    viewsFuture = std::async(std::launch::async, [this]() { return getViewsAsync(); });
}

std::vector<Table> SQLiteDatabaseNode::getViewsAsync() {
    std::vector<Table> result;

    if (!loadingViews.load()) {
        return result;
    }

    try {
        if (!parentDb || !parentDb->isConnected()) {
            Logger::error("Parent database not connected");
            return result;
        }

        // Get view names
        std::vector<std::string> viewNames;
        const std::string viewNamesQuery = "SELECT name FROM sqlite_master WHERE type='view'";
        {
            auto session = parentDb->getSession();
            if (!session) {
                Logger::error("Failed to get database session");
                return result;
            }

            const soci::rowset viewRs = session->prepare << viewNamesQuery;
            for (const auto& row : viewRs) {
                if (!loadingViews.load()) {
                    return result;
                }
                viewNames.push_back(convertRowValue(row, 0));
            }
        }

        Logger::debug("Found " + std::to_string(viewNames.size()) + " views in database");

        // Load view details
        for (const auto& viewName : viewNames) {
            if (!loadingViews.load()) {
                break;
            }

            Table view;
            view.name = viewName;
            view.fullName = parentDb->getName() + "." + viewName;

            // Get view columns
            const std::string columnsQuery = std::format("PRAGMA table_info({})", viewName);
            {
                auto session = parentDb->getSession();
                if (session) {
                    const soci::rowset colRs = session->prepare << columnsQuery;
                    for (const auto& row : colRs) {
                        Column col;
                        col.name = convertRowValue(row, 1);             // name
                        col.type = convertRowValue(row, 2);             // type
                        col.isNotNull = convertRowValue(row, 3) == "1"; // notnull
                        col.isPrimaryKey = false;                       // views don't have PKs

                        view.columns.push_back(col);
                    }
                }
            }

            result.push_back(view);
        }

        Logger::info("Finished loading views. Total views: " + std::to_string(result.size()));
    } catch (const std::exception& e) {
        Logger::error(std::format("Error loading views: {}", e.what()));
    }

    return result;
}

void SQLiteDatabaseNode::checkSequencesStatusAsync() {
    if (sequencesFuture.valid() &&
        sequencesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            sequences = sequencesFuture.get();
            Logger::info(
                std::format("Sequence loading completed. Found {} sequences", sequences.size()));
            sequencesLoaded = true;
            loadingSequences = false;
        } catch (const std::exception& e) {
            Logger::error(std::format("Error in sequence loading: {}", e.what()));
            lastSequencesError = e.what();
            sequencesLoaded = true;
            loadingSequences = false;
        }
    }
}

void SQLiteDatabaseNode::startSequencesLoadAsync() {
    Logger::debug("startSequencesLoadAsync for SQLite database");
    if (!parentDb) {
        return;
    }

    // Don't start if already loading or loaded
    if (loadingSequences.load() || sequencesLoaded) {
        return;
    }

    loadingSequences = true;

    // Start async loading
    sequencesFuture = std::async(std::launch::async, [this]() { return getSequencesAsync(); });
}

std::vector<std::string> SQLiteDatabaseNode::getSequencesAsync() {
    std::vector<std::string> result;

    if (!loadingSequences.load()) {
        return result;
    }

    try {
        if (!parentDb || !parentDb->isConnected()) {
            Logger::error("Parent database not connected");
            return result;
        }

        // Check if sqlite_sequence table exists
        const std::string checkQuery =
            "SELECT name FROM sqlite_master WHERE type='table' AND name='sqlite_sequence'";
        {
            auto session = parentDb->getSession();
            if (!session) {
                Logger::error("Failed to get database session");
                return result;
            }

            const soci::rowset checkRs = session->prepare << checkQuery;
            bool hasSequenceTable = false;
            for (const auto& row : checkRs) {
                hasSequenceTable = true;
                break;
            }

            if (!hasSequenceTable) {
                Logger::debug("sqlite_sequence table does not exist");
                return result;
            }
        }

        // Get sequences
        const std::string seqQuery = "SELECT name FROM sqlite_sequence";
        {
            auto session = parentDb->getSession();
            if (session) {
                const soci::rowset seqRs = session->prepare << seqQuery;
                for (const auto& row : seqRs) {
                    if (!loadingSequences.load()) {
                        return result;
                    }
                    result.push_back(convertRowValue(row, 0));
                }
            }
        }

        Logger::info("Finished loading sequences. Total sequences: " +
                     std::to_string(result.size()));
    } catch (const std::exception& e) {
        Logger::error(std::format("Error loading sequences: {}", e.what()));
    }

    return result;
}

// ITableDataProvider implementation
std::vector<std::vector<std::string>>
SQLiteDatabaseNode::getTableData(const std::string& tableName, int limit, int offset,
                                 const std::string& whereClause) {
    if (!parentDb) {
        return {};
    }
    return parentDb->getTableData(tableName, limit, offset);
}

std::vector<std::string> SQLiteDatabaseNode::getColumnNames(const std::string& tableName) {
    if (!parentDb) {
        return {};
    }
    return parentDb->getColumnNames(tableName);
}

int SQLiteDatabaseNode::getRowCount(const std::string& tableName, const std::string& whereClause) {
    if (!parentDb) {
        return 0;
    }
    return parentDb->getRowCount(tableName);
}

std::string SQLiteDatabaseNode::executeQuery(const std::string& query) {
    if (!parentDb) {
        return "Error: Parent database not found";
    }
    return parentDb->executeQuery(query);
}

QueryResult SQLiteDatabaseNode::executeQueryWithResult(const std::string& query, int rowLimit) {
    QueryResult result;
    if (!parentDb) {
        result.success = false;
        result.message = "Error: Parent database not found";
        return result;
    }

    try {
        auto [columns, data] = parentDb->executeQueryStructured(query);
        result.success = true;
        result.columnNames = columns;
        result.tableData = data;
        result.errorMessage = "Query executed successfully";
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = std::string("Error: ") + e.what();
    }

    return result;
}
