#include "database/postgresql.hpp"

DatabaseInterface* PostgresSchemaNode::ownerDatabase() const {
    return parentDbNode ? parentDbNode->parentDb : nullptr;
}

std::string PostgresSchemaNode::getFullPath() const {
    return parentDbNode ? parentDbNode->name + "." + name : name;
}

DatabaseType PostgresSchemaNode::getDatabaseType() const {
    return parentDbNode ? parentDbNode->getDatabaseType() : DatabaseType::POSTGRESQL;
}

LibDatabaseNode* PostgresSchemaNode::databaseNode() const {
    return parentDbNode;
}

void PostgresSchemaNode::startTablesLoadAsync(bool force) {
    LibDatabaseNode::startTablesLoadAsync(force);
    if (!force)
        return;
    if (materializedViewsLoaded)
        startMaterializedViewsLoadAsync(true);
    if (sequencesLoaded)
        startSequencesLoadAsync(true);
}

std::pair<bool, std::string> PostgresSchemaNode::renameSchema(const std::string& newName) {
    try {
        auto r = withHandle([&](dearsql::IDatabase& db) { return db.renameSchema(newName); });
        if (r.first && parentDbNode)
            parentDbNode->startSchemasLoadAsync(true);
        return r;
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

std::pair<bool, std::string> PostgresSchemaNode::dropSchema() {
    try {
        auto r = withHandle([](dearsql::IDatabase& db) { return db.dropSchema(); });
        if (r.first && parentDbNode)
            parentDbNode->startSchemasLoadAsync(true);
        return r;
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

DatabaseInterface* PostgresDatabaseNode::ownerDatabase() const {
    return parentDb;
}

DatabaseType PostgresDatabaseNode::getDatabaseType() const {
    return parentDb ? parentDb->getConnectionInfo().type : DatabaseType::POSTGRESQL;
}

dearsql::DatabasePtr PostgresDatabaseNode::openHandle() {
    auto conn = parentDb->connection();
    return conn ? conn->openDatabase(name) : nullptr;
}

std::unique_ptr<PostgresSchemaNode>
PostgresDatabaseNode::makeSchema(const std::string& schemaName) {
    auto schema = std::make_unique<PostgresSchemaNode>();
    schema->name = schemaName;
    schema->parentDbNode = this;
    return schema;
}
