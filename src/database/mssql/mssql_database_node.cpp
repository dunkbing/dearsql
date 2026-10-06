#include "database/mssql.hpp"

DatabaseInterface* MSSQLDatabaseNode::ownerDatabase() const {
    return parentDb;
}

dearsql::DatabasePtr MSSQLDatabaseNode::openHandle() {
    auto conn = parentDb->connection();
    return conn ? conn->openDatabase(name) : nullptr;
}

std::unique_ptr<MSSQLSchemaNode> MSSQLDatabaseNode::makeSchema(const std::string& schemaName) {
    auto schema = std::make_unique<MSSQLSchemaNode>();
    schema->name = schemaName;
    schema->parentDbNode = this;
    return schema;
}

DatabaseInterface* MSSQLSchemaNode::ownerDatabase() const {
    return parentDbNode->parentDb;
}

std::string MSSQLSchemaNode::getFullPath() const {
    return parentDbNode->name + "." + name;
}

LibDatabaseNode* MSSQLSchemaNode::databaseNode() const {
    return parentDbNode;
}

std::string MSSQLSchemaNode::fullNamePrefix() const {
    return parentDbNode->parentDb->getConnectionInfo().name + "." + getFullPath();
}
