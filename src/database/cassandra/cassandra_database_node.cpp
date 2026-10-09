#include "database/cassandra.hpp"

DatabaseInterface* CassandraDatabaseNode::ownerDatabase() const {
    return parentDb;
}

std::string CassandraDatabaseNode::fullNamePrefix() const {
    return parentDb->getConnectionInfo().name + "." + name;
}

dearsql::DatabasePtr CassandraDatabaseNode::openHandle() {
    auto conn = parentDb->connection();
    return conn ? conn->database(name) : nullptr;
}
