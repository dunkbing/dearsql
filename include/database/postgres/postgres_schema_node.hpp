#pragma once

#include "database/db.hpp"
#include <atomic>
#include <future>
#include <string>
#include <vector>

// Forward declaration
class PostgresDatabaseNode;

/**
 * @brief Per-schema data for PostgreSQL
 *
 * PostgreSQL hierarchy: Database → Schema → Tables/Views/Sequences
 * Each PostgresSchemaNode represents one schema (e.g., "public", "analytics")
 */
class PostgresSchemaNode {
public:
    PostgresDatabaseNode* parentDbNode = nullptr;
    std::string name;

    // Schema contents (only tables, views, sequences for now)
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences;

    // Loading state flags
    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool sequencesLoaded = false;
    std::atomic<bool> loadingTables = false;
    std::atomic<bool> loadingViews = false;
    std::atomic<bool> loadingSequences = false;

    // Async futures
    std::future<std::vector<Table>> tablesFuture;
    std::future<std::vector<Table>> viewsFuture;
    std::future<std::vector<std::string>> sequencesFuture;

    // UI expansion state
    bool tablesExpanded = false;
    bool viewsExpanded = false;
    bool sequencesExpanded = false;

    // Error tracking
    std::string lastTablesError;
    std::string lastViewsError;
    std::string lastSequencesError;

    // Methods
    void startTablesLoadAsync();
    void checkTablesStatusAsync();
    std::vector<Table> getTablesWithColumnsAsync();
};
