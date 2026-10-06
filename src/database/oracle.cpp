#include "database/oracle.hpp"
#include "database/ddl_utils.hpp"
#include "database/oracle/oracle_client_installer.hpp"

OracleDatabase::OracleDatabase(const DatabaseConnectionInfo& info) : ServerDatabase(info) {
    OracleClientInstaller::configure();
}

QueryResult OracleDatabase::executeQuery(const std::string& query, int rowLimit) {
    if (!connect().first) {
        QueryResult r;
        r.statements.push_back({.success = false, .errorMessage = "Not connected to database"});
        return r;
    }
    return getDatabaseData(defaultSchema())->executeQuery(query, rowLimit);
}

std::unique_ptr<OracleDatabaseNode> OracleDatabase::makeNode(const std::string& name) {
    auto node = std::make_unique<OracleDatabaseNode>();
    node->name = name;
    node->parentDb = this;
    return node;
}

std::vector<std::string> OracleDatabase::listDatabaseNames() {
    if (!connectionInfo.showAllDatabases)
        return {defaultSchema()};
    return ServerDatabase::listDatabaseNames();
}

std::string OracleDatabase::defaultSchema() const {
    return ddl_utils::toUpper(connectionInfo.username);
}
