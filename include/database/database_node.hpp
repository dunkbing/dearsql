#pragma once

#include "db.hpp"
#include "db_interface.hpp"
#include "query_executor.hpp"
#include "utils/reaper.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief Unified interface for all database nodes in the hierarchy
 *
 * This interface provides a common contract for database nodes across different
 * database types (PostgreSQL schemas, MySQL databases, SQLite databases).
 * It unifies query execution, schema information, and table data access.
 *
 * Hierarchy by database type:
 * - PostgreSQL: Connection -> Database -> Schema (implements IDatabaseNode)
 * - MySQL: Connection -> Database (implements IDatabaseNode)
 * - SQLite: Connection (implements IDatabaseNode)
 */
class IDatabaseNode : public IQueryExecutor {
public:
    virtual ~IDatabaseNode() = default;

    [[nodiscard]] virtual DatabaseInterface* ownerDatabase() const {
        return nullptr;
    }

    // the node this one lives under (a schema's database); null at the top
    [[nodiscard]] virtual IDatabaseNode* parentNode() const {
        return nullptr;
    }

    [[nodiscard]] virtual std::string getName() const = 0;

    [[nodiscard]] virtual std::string getFullPath() const = 0;

    [[nodiscard]] virtual DatabaseType getDatabaseType() const = 0;

    [[nodiscard]] virtual std::vector<Table>& getTables() = 0;
    [[nodiscard]] virtual const std::vector<Table>& getTables() const = 0;

    [[nodiscard]] virtual std::vector<Table>& getViews() = 0;
    [[nodiscard]] virtual const std::vector<Table>& getViews() const = 0;

    [[nodiscard]] virtual const std::vector<std::string>& getSequences() const {
        static const std::vector<std::string> empty;
        return empty;
    }

    /**
     * @brief Get paginated table data
     * @param tableName Name of the table
     * @param limit Maximum rows to return
     * @param offset Starting row offset
     * @param whereClause Optional WHERE clause filter
     * @param orderBy Optional ORDER BY clause
     * @return Vector of rows (each row is vector of string values)
     */
    virtual std::vector<std::vector<std::string>> getTableData(const Table& table, int limit,
                                                               int offset,
                                                               const std::string& whereClause = "",
                                                               const std::string& orderBy = "") = 0;

    [[nodiscard]] virtual std::vector<std::string> getColumnNames(const Table& table) = 0;

    /**
     * @brief Get total row count for a table
     * @param table Table to query
     * @param whereClause Optional WHERE clause filter
     * @return Total row count
     */
    [[nodiscard]] virtual int getRowCount(const Table& table,
                                          const std::string& whereClause = "") = 0;

    [[nodiscard]] virtual bool isTablesLoaded() const = 0;

    [[nodiscard]] virtual bool isViewsLoaded() const = 0;

    [[nodiscard]] virtual bool isLoadingTables() const = 0;

    [[nodiscard]] virtual bool isLoadingViews() const = 0;

    virtual void startTablesLoadAsync(bool force = false) = 0;

    virtual void startViewsLoadAsync(bool force = false) = 0;

    virtual void checkLoadingStatus() = 0;

    [[nodiscard]] virtual const std::string& getLastTablesError() const {
        static const std::string empty;
        return empty;
    }

    [[nodiscard]] virtual const std::string& getLastViewsError() const {
        static const std::string empty;
        return empty;
    }

    /**
     * @brief Create a table using the node's database connection
     * @param table Table definition
     * @return pair<success, error_message>
     */
    virtual std::pair<bool, std::string> createTable(const Table& table) {
        return {false, "Create table not supported for this database type"};
    }

    virtual void startTableRefreshAsync(const std::string& tableName) = 0;

    [[nodiscard]] virtual bool isTableRefreshing(const std::string& tableName) const = 0;

    virtual void checkTableRefreshStatusAsync(const std::string& tableName) = 0;
};

// UI thread only. a live owner hands over a node it is about to destroy (a
// database dropped or renamed, a schema gone from a relist) so the app can close
// the tabs pointing into it first. the node then dies on the reaper: its
// destructor waits for workers still running on it
inline std::function<void(std::unique_ptr<IDatabaseNode>)> retireNodeHook;

inline void retireNode(std::unique_ptr<IDatabaseNode> node) {
    if (!node)
        return;
    if (retireNodeHook)
        retireNodeHook(std::move(node));
    else
        Reaper::dispose(std::move(node));
}

// an owning reference to the node's connection, for a tab worker that may outlive
// its tab. a node retired from a live owner is covered by its own destructor,
// which waits out calls still running on it
inline std::shared_ptr<DatabaseInterface> keepOwnerAlive(const IDatabaseNode* node) {
    auto* owner = node ? node->ownerDatabase() : nullptr;
    return owner ? owner->keepAlive() : nullptr;
}
