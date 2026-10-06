#include "database/mysql.hpp"

DatabaseInterface* MySQLDatabaseNode::ownerDatabase() const {
    return parentDb;
}

DatabaseType MySQLDatabaseNode::getDatabaseType() const {
    return parentDb->getConnectionInfo().type;
}

dearsql::DatabasePtr MySQLDatabaseNode::openHandle() {
    auto conn = parentDb->connection();
    return conn ? conn->openDatabase(name) : nullptr;
}

std::string MySQLDatabaseNode::fullNamePrefix() const {
    return parentDb->getConnectionInfo().name + "." + name;
}
