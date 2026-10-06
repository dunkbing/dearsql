#pragma once

#include "oracle/oracle_database_node.hpp"
#include "server_database.hpp"

// schemas (owners) play the part of databases; the service name is
// connectionInfo.database
class OracleDatabase final : public ServerDatabase<OracleDatabaseNode> {
public:
    explicit OracleDatabase(const DatabaseConnectionInfo& info);

    // server-level queries run in the login user's schema
    QueryResult executeQuery(const std::string& query, int rowLimit = 1000) override;

protected:
    std::unique_ptr<OracleDatabaseNode> makeNode(const std::string& name) override;
    std::vector<std::string> listDatabaseNames() override;

private:
    [[nodiscard]] std::string defaultSchema() const;
};
