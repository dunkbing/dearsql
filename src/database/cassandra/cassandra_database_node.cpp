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

DatabaseType CassandraDatabaseNode::getDatabaseType() const {
    auto* owner = ownerDatabase();
    return owner ? owner->getConnectionInfo().type : DatabaseType::CASSANDRA;
}
