#include "database/oracle.hpp"

DatabaseInterface* OracleDatabaseNode::ownerDatabase() const {
    return parentDb;
}

dearsql::DatabasePtr OracleDatabaseNode::openHandle() {
    auto conn = parentDb->connection();
    return conn ? conn->openDatabase(name) : nullptr;
}

std::string OracleDatabaseNode::fullNamePrefix() const {
    return parentDb->getConnectionInfo().name + "." + name;
}
