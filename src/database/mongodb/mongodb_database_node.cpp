#include "database/mongodb.hpp"

DatabaseInterface* MongoDBDatabaseNode::ownerDatabase() const {
    return parentDb;
}

dearsql::DatabasePtr MongoDBDatabaseNode::openHandle() {
    auto conn = parentDb->connection();
    return conn ? conn->database(name) : nullptr;
}

std::string MongoDBDatabaseNode::fullNamePrefix() const {
    return parentDb->getConnectionInfo().name + "." + name;
}
