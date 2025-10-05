#pragma once

#include <string>

/**
 * Holds UI-specific state for database connections.
 * This is separated from database operations to maintain clean separation of concerns.
 */
struct DatabaseUIState {
    // Expansion state in the sidebar tree
    bool expanded = false;

    // Connection attempt tracking
    bool attemptedConnection = false;
    std::string lastConnectionError;

    // Persistent connection ID for app state
    int savedConnectionId = -1;

    // Clear all UI state
    void reset() {
        expanded = false;
        attemptedConnection = false;
        lastConnectionError.clear();
        savedConnectionId = -1;
    }
};
