#pragma once

#include <string>

class PostgresDatabase;

namespace PostgresHierarchy {
    void renderPostgresHierarchy(PostgresDatabase *pgDb);
    void renderSingleDatabaseHierarchy(PostgresDatabase *pgDb);
    void renderAllDatabasesHierarchy(PostgresDatabase *pgDb);
    void renderSchemasSection(PostgresDatabase *pgDb);
    void renderSchemasSection(PostgresDatabase *pgDb);
    void renderCachedSchemasSection(PostgresDatabase *pgDb, const std::string &dbName);
    void renderCachedSequencesSection(PostgresDatabase *pgDb, const std::string &dbName);
    void renderSchemaNode(PostgresDatabase *pgDb, int schemaIndex);
    void renderCachedSchemaNode(PostgresDatabase *pgDb, const std::string &dbName, int schemaIndex);
    void renderTableNode(PostgresDatabase *pgDb, int tableIndex);
    void renderViewNode(PostgresDatabase *pgDb, int viewIndex);
    void renderSequenceNode(PostgresDatabase *pgDb, int sequenceIndex);
    void renderCachedSequenceNode(PostgresDatabase *pgDb, const std::string &dbName,
                                  int sequenceIndex);
} // namespace PostgresHierarchy
