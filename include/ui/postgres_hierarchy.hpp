#pragma once

class PostgresDatabase;

namespace PostgresHierarchy {
    void renderPostgresHierarchy(PostgresDatabase *pgDb);
    void renderSingleDatabaseHierarchy(PostgresDatabase *pgDb);
    void renderAllDatabasesHierarchy(PostgresDatabase *pgDb);
    void renderSchemasSection(PostgresDatabase *pgDb);
    void renderSchemaNode(PostgresDatabase *pgDb, int schemaIndex);
    void renderTableNode(PostgresDatabase *pgDb, int tableIndex);
    void renderViewNode(PostgresDatabase *pgDb, int viewIndex);
    void renderSequenceNode(PostgresDatabase *pgDb, int sequenceIndex);
} // namespace PostgresHierarchy
