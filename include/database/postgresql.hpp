#pragma once

#include "db_interface.hpp"
#include <pqxx/pqxx>
#ifdef PQXX_HAVE_CXA_DEMANGLE
#undef PQXX_HAVE_CXA_DEMANGLE
#endif

class PostgreSQLDatabase : public DatabaseInterface {
public:
    PostgreSQLDatabase(const std::string& name, const std::string& host, int port, 
                      const std::string& database, const std::string& username, 
                      const std::string& password);
    ~PostgreSQLDatabase() override;

    // Connection management
    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    bool isConnected() const override;

    // Database info
    const std::string& getName() const override;
    const std::string& getConnectionString() const override;
    const std::string& getPath() const override;
    void* getConnection() const override;
    DatabaseType getType() const override;

    // Table management
    void refreshTables() override;
    const std::vector<Table>& getTables() const override;
    std::vector<Table>& getTables() override;
    bool areTablesLoaded() const override;
    void setTablesLoaded(bool loaded) override;

    // Query execution
    std::string executeQuery(const std::string& query) override;
    std::vector<std::vector<std::string>> getTableData(const std::string& tableName, int limit, int offset) override;
    std::vector<std::string> getColumnNames(const std::string& tableName) override;
    int getRowCount(const std::string& tableName) override;

    // UI state
    bool isExpanded() const override;
    void setExpanded(bool expanded) override;

protected:
    std::vector<std::string> getTableNames() override;
    std::vector<Column> getTableColumns(const std::string& tableName) override;

private:
    std::string name;
    std::string host;
    int port;
    std::string database;
    std::string username;
    std::string password;
    std::string connectionString;
    std::unique_ptr<pqxx::connection> connection;
    std::vector<Table> tables;
    bool connected = false;
    bool expanded = false;
    bool tablesLoaded = false;
};