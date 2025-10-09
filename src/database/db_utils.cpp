#include "database/db.hpp"
#include <chrono>
#include <soci/soci.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

std::string convertRowValue(const soci::row& row, const std::size_t columnIndex) {
    if (row.get_indicator(columnIndex) == soci::i_null) {
        return "NULL";
    }

    switch (const soci::column_properties& cp = row.get_properties(columnIndex); cp.get_db_type()) {
    case soci::db_string:
        return row.get<std::string>(columnIndex);
    case soci::db_wstring: {
        auto ws = row.get<std::wstring>(columnIndex);
        return {ws.begin(), ws.end()};
    }
    case soci::db_int8:
        return std::to_string(row.get<int8_t>(columnIndex));
    case soci::db_uint8:
        return std::to_string(row.get<uint8_t>(columnIndex));
    case soci::db_int16:
        return std::to_string(row.get<int16_t>(columnIndex));
    case soci::db_uint16:
        return std::to_string(row.get<uint16_t>(columnIndex));
    case soci::db_int32:
        return std::to_string(row.get<int32_t>(columnIndex));
    case soci::db_uint32:
        return std::to_string(row.get<uint32_t>(columnIndex));
    case soci::db_int64:
        return std::to_string(row.get<int64_t>(columnIndex));
    case soci::db_uint64:
        return std::to_string(row.get<uint64_t>(columnIndex));
    case soci::db_double:
        return std::to_string(row.get<double>(columnIndex));
    case soci::db_date: {
        const auto date = row.get<std::tm>(columnIndex);
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &date);
        return {buffer};
    }
    case soci::db_blob:
        return "[BINARY DATA]";
    case soci::db_xml:
        try {
            return row.get<std::string>(columnIndex);
        } catch (const std::bad_cast&) {
            return "[XML DATA]";
        }
    default:
        try {
            return row.get<std::string>(columnIndex);
        } catch (const std::bad_cast&) {
            return "[UNKNOWN DATA TYPE]";
        }
    }
}

void buildForeignKeyLookup(Table& table) {
    table.foreignKeysByColumn.clear();
    for (const auto& fk : table.foreignKeys) {
        table.foreignKeysByColumn[fk.sourceColumn] = fk;
    }
}

void populateIncomingForeignKeys(std::vector<Table>& tables) {
    std::unordered_map<std::string, Table*> tableLookup;
    tableLookup.reserve(tables.size());

    for (auto& table : tables) {
        table.incomingForeignKeys.clear();
        tableLookup[table.name] = &table;
    }

    for (const auto& sourceTable : tables) {
        for (const auto& fk : sourceTable.foreignKeys) {
            if (const auto targetIt = tableLookup.find(fk.targetTable);
                targetIt != tableLookup.end()) {
                ForeignKey incoming = fk;
                incoming.targetTable = sourceTable.name; // Table referencing the target
                incoming.sourceColumn = fk.sourceColumn;
                incoming.targetColumn = fk.targetColumn;
                incoming.onDelete = fk.onDelete;
                incoming.onUpdate = fk.onUpdate;
                targetIt->second->incomingForeignKeys.push_back(std::move(incoming));
            }
        }
    }
}

TableDataLoadState* TableDataLoader::findState(const std::string& tableName) {
    const auto it = states.find(tableName);
    if (it == states.end()) {
        return nullptr;
    }
    return &it->second;
}

const TableDataLoadState* TableDataLoader::findState(const std::string& tableName) const {
    const auto it = states.find(tableName);
    if (it == states.end()) {
        return nullptr;
    }
    return &it->second;
}

bool TableDataLoader::start(const std::string& tableName, Task task) {
    auto& state = states[tableName];

    bool expected = false;
    if (!state.loading.compare_exchange_strong(expected, true)) {
        return false; // Already loading
    }

    state.ready = false;
    state.tableData.clear();
    state.columnNames.clear();
    state.rowCount = 0;
    state.lastError.clear();

    state.future = std::async(std::launch::async, [task, &state]() { task(state); });

    return true;
}

void TableDataLoader::check(const std::string& tableName) {
    auto* state = findState(tableName);
    if (!state || !state->loading.load()) {
        return;
    }

    if (!state->future.valid() ||
        state->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    try {
        state->future.get();
        state->ready.store(true);
    } catch (const std::exception& e) {
        state->ready.store(false);
        state->tableData.clear();
        state->columnNames.clear();
        state->rowCount = 0;
        state->lastError = e.what();
    }

    state->loading.store(false);
}

void TableDataLoader::checkAll() {
    for (auto& [name, state] : states) {
        if (state.loading.load()) {
            check(name);
        }
    }
}

bool TableDataLoader::isLoading(const std::string& tableName) const {
    const auto* state = findState(tableName);
    return state != nullptr && state->loading.load();
}

bool TableDataLoader::isAnyLoading() const {
    for (const auto& entry : states) {
        const auto& state = entry.second;
        if (state.loading.load()) {
            return true;
        }
    }
    return false;
}

bool TableDataLoader::hasResult(const std::string& tableName) const {
    const auto* state = findState(tableName);
    return state != nullptr && state->ready.load();
}

bool TableDataLoader::hasAnyResult() const {
    for (const auto& entry : states) {
        const auto& state = entry.second;
        if (state.ready.load()) {
            return true;
        }
    }
    return false;
}

std::vector<std::vector<std::string>>
TableDataLoader::getTableData(const std::string& tableName) const {
    const auto* state = findState(tableName);
    if (state && state->ready.load()) {
        return state->tableData;
    }
    return {};
}

std::vector<std::vector<std::string>> TableDataLoader::getFirstAvailableTableData() const {
    for (const auto& entry : states) {
        const auto& state = entry.second;
        if (state.ready.load()) {
            return state.tableData;
        }
    }
    return {};
}

std::vector<std::string> TableDataLoader::getColumnNames(const std::string& tableName) const {
    const auto* state = findState(tableName);
    if (state && state->ready.load()) {
        return state->columnNames;
    }
    return {};
}

std::vector<std::string> TableDataLoader::getFirstAvailableColumnNames() const {
    for (const auto& entry : states) {
        const auto& state = entry.second;
        if (state.ready.load()) {
            return state.columnNames;
        }
    }
    return {};
}

int TableDataLoader::getRowCount(const std::string& tableName) const {
    const auto* state = findState(tableName);
    if (state && state->ready.load()) {
        return state->rowCount;
    }
    return 0;
}

int TableDataLoader::getFirstAvailableRowCount() const {
    for (const auto& entry : states) {
        const auto& state = entry.second;
        if (state.ready.load()) {
            return state.rowCount;
        }
    }
    return 0;
}

std::string TableDataLoader::getLastError(const std::string& tableName) const {
    const auto* state = findState(tableName);
    if (state) {
        return state->lastError;
    }
    return {};
}

void TableDataLoader::clear(const std::string& tableName) {
    auto* state = findState(tableName);
    if (!state) {
        return;
    }

    state->ready = false;
    state->tableData.clear();
    state->columnNames.clear();
    state->rowCount = 0;
    state->lastError.clear();
}

void TableDataLoader::clearAll() {
    for (auto& entry : states) {
        auto& state = entry.second;
        state.ready = false;
        state.tableData.clear();
        state.columnNames.clear();
        state.rowCount = 0;
        state.lastError.clear();
    }
}

void TableDataLoader::cancelAllAndWait() {
    for (auto& entry : states) {
        auto& state = entry.second;
        state.loading.store(false);
        if (state.future.valid()) {
            state.future.wait();
        }
    }
}

std::string buildCondition(const std::vector<std::string>& conditions, const std::string& op) {
    if (conditions.empty()) {
        return "";
    }

    std::ostringstream oss;
    for (size_t i = 0; i < conditions.size(); ++i) {
        if (i > 0) {
            oss << " " << op << " ";
        }
        oss << "(" << conditions[i] << ")";
    }
    return oss.str();
}
