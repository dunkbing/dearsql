#pragma once

#include <string>

class PostgresDatabase;

namespace PostgresHierarchy {
    void renderPostgresHierarchy(const std::shared_ptr<PostgresDatabase>& pgDb);
    void renderSingleDatabaseHierarchy(const std::shared_ptr<PostgresDatabase>& pgDb);
    void renderAllDatabasesHierarchy(const std::shared_ptr<PostgresDatabase>& pgDb);
    void renderSchemasSection(const std::shared_ptr<PostgresDatabase>& pgDb);
    void renderCachedSchemasSection(const std::shared_ptr<PostgresDatabase>& pgDb,
                                    const std::string& dbName);
    void renderSchemaNode(const std::shared_ptr<PostgresDatabase>& pgDb, int schemaIndex);
    void renderCachedSchemaNode(const std::shared_ptr<PostgresDatabase>& pgDb,
                                const std::string& dbName, int schemaIndex);
    void renderTableNode(const std::shared_ptr<PostgresDatabase>& pgDb, int tableIndex);
    void renderViewNode(const std::shared_ptr<PostgresDatabase>& pgDb, int viewIndex);
    void renderSequenceNode(const std::shared_ptr<PostgresDatabase>& pgDb, int sequenceIndex);
} // namespace PostgresHierarchy
