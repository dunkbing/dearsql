#include "database/mongodb/mongodb_database_node.hpp"
#include "database/mongodb.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <set>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>

namespace {

    // Convert a BSON element to a human-readable string
    std::string bsonElementToString(const bsoncxx::document::element& elem) {
        if (!elem) {
            return "";
        }

        switch (elem.type()) {
        case bsoncxx::type::k_string:
            return std::string(elem.get_string().value);
        case bsoncxx::type::k_int32:
            return std::to_string(elem.get_int32().value);
        case bsoncxx::type::k_int64:
            return std::to_string(elem.get_int64().value);
        case bsoncxx::type::k_double:
            return std::to_string(elem.get_double().value);
        case bsoncxx::type::k_bool:
            return elem.get_bool().value ? "true" : "false";
        case bsoncxx::type::k_oid:
            return elem.get_oid().value.to_string();
        case bsoncxx::type::k_date: {
            // Convert milliseconds since epoch to ISO 8601 format
            const auto millis = elem.get_date().value.count();
            const auto seconds = millis / 1000;
            const auto time = static_cast<std::time_t>(seconds);
            std::tm tm{};
            gmtime_r(&time, &tm);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
            return std::string(buf);
        }
        case bsoncxx::type::k_null:
            return "null";
        case bsoncxx::type::k_decimal128:
            return elem.get_decimal128().value.to_string();
        case bsoncxx::type::k_document:
            return bsoncxx::to_json(elem.get_document().value);
        case bsoncxx::type::k_array:
            return bsoncxx::to_json(elem.get_array().value);
        default:
            // For any other types, wrap in a document to convert to JSON
            try {
                auto wrapper = bsoncxx::builder::stream::document{}
                               << "v" << elem.get_value() << bsoncxx::builder::stream::finalize;
                return bsoncxx::to_json(wrapper.view());
            } catch (...) {
                return "<unknown>";
            }
        }
    }

} // namespace

std::string MongoDBDatabaseNode::getFullPath() const {
    return name;
}

void MongoDBDatabaseNode::startTablesLoadAsync(bool force) {
    startCollectionsLoadAsync(force);
}

void MongoDBDatabaseNode::startViewsLoadAsync(bool force) {
    // MongoDB views are just special collections, handled in collectionsLoader
    // For now, views are empty
    if (force) {
        views.clear();
        viewsLoaded = false;
    }
    viewsLoaded = true;
}

void MongoDBDatabaseNode::startCollectionsLoadAsync(bool force) {
    Logger::debug("startCollectionsLoadAsync for db: " + name + (force ? " (force refresh)" : ""));
    if (!parentDb) {
        return;
    }

    if (collectionsLoader.isRunning()) {
        return;
    }

    if (force) {
        collections.clear();
        collectionsLoaded = false;
        lastCollectionsError.clear();
    }

    if (!force && collectionsLoaded) {
        return;
    }

    collectionsLoader.start([this]() { return getCollectionsAsync(); });
}

void MongoDBDatabaseNode::checkCollectionsStatusAsync() {
    collectionsLoader.check([this](const std::vector<Table>& result) {
        collections = result;
        Logger::info(
            std::format("Async collection loading completed for database {}. Found {} collections",
                        name, collections.size()));
        collectionsLoaded = true;
    });
}

std::vector<Table> MongoDBDatabaseNode::getCollectionsAsync() {
    std::vector<Table> result;

    if (!collectionsLoader.isRunning()) {
        return result;
    }

    try {
        if (!parentDb) {
            return result;
        }

        auto client = parentDb->getClient();
        auto db = (*client)[name];
        auto collectionNames = db.list_collection_names();

        Logger::debug(
            std::format("Found {} collections in database {}", collectionNames.size(), name));

        for (const auto& collName : collectionNames) {
            if (!collectionsLoader.isRunning()) {
                break;
            }

            // Skip system collections
            if (collName.starts_with("system.")) {
                continue;
            }

            Table collection;
            collection.name = collName;
            collection.fullName = parentDb->getConnectionInfo().name + "." + name + "." + collName;

            // Infer schema by sampling documents
            collection.columns = inferSchemaFromSample(collName, 100);

            result.push_back(std::move(collection));
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting collections for database {}: {}", name, e.what()));
        lastCollectionsError = e.what();
    }

    return result;
}

std::vector<Column> MongoDBDatabaseNode::inferSchemaFromSample(const std::string& collectionName,
                                                               int sampleSize) {
    std::vector<Column> columns;
    std::set<std::string> seenFields;

    try {
        auto client = parentDb->getClient();
        auto db = (*client)[name];
        auto coll = db[collectionName];

        mongocxx::options::find opts;
        opts.limit(sampleSize);

        auto cursor = coll.find({}, opts);

        for (auto&& doc : cursor) {
            for (auto&& elem : doc) {
                std::string fieldName(elem.key());
                if (seenFields.contains(fieldName)) {
                    continue;
                }
                seenFields.insert(fieldName);

                Column col;
                col.name = fieldName;

                // Map BSON type to string
                switch (elem.type()) {
                case bsoncxx::type::k_double:
                    col.type = "double";
                    break;
                case bsoncxx::type::k_string:
                    col.type = "string";
                    break;
                case bsoncxx::type::k_document:
                    col.type = "object";
                    break;
                case bsoncxx::type::k_array:
                    col.type = "array";
                    break;
                case bsoncxx::type::k_binary:
                    col.type = "binary";
                    break;
                case bsoncxx::type::k_oid:
                    col.type = "objectId";
                    break;
                case bsoncxx::type::k_bool:
                    col.type = "bool";
                    break;
                case bsoncxx::type::k_date:
                    col.type = "date";
                    break;
                case bsoncxx::type::k_null:
                    col.type = "null";
                    break;
                case bsoncxx::type::k_int32:
                    col.type = "int32";
                    break;
                case bsoncxx::type::k_int64:
                    col.type = "int64";
                    break;
                case bsoncxx::type::k_decimal128:
                    col.type = "decimal128";
                    break;
                default:
                    col.type = "unknown";
                    break;
                }

                // _id is always the primary key
                if (fieldName == "_id") {
                    col.isPrimaryKey = true;
                    col.isNotNull = true;
                }

                columns.push_back(col);
            }
        }

        // Sort columns with _id first
        std::ranges::sort(columns, [](const Column& a, const Column& b) {
            if (a.name == "_id")
                return true;
            if (b.name == "_id")
                return false;
            return a.name < b.name;
        });

    } catch (const std::exception& e) {
        Logger::error(std::format("Error inferring schema for {}: {}", collectionName, e.what()));
    }

    return columns;
}

void MongoDBDatabaseNode::startTableRefreshAsync(const std::string& collectionName) {
    Logger::debug(std::format("Starting async refresh for collection: {}", collectionName));

    if (collectionRefreshLoaders.contains(collectionName) &&
        collectionRefreshLoaders[collectionName].isRunning()) {
        Logger::debug(std::format("Collection {} is already being refreshed", collectionName));
        return;
    }

    collectionRefreshLoaders[collectionName].start(
        [this, collectionName]() { return refreshCollectionAsync(collectionName); });
    Logger::debug(std::format("Async refresh started for collection: {}", collectionName));
}

void MongoDBDatabaseNode::checkTableRefreshStatusAsync(const std::string& collectionName) {
    auto it = collectionRefreshLoaders.find(collectionName);
    if (it == collectionRefreshLoaders.end()) {
        return;
    }

    it->second.check([this, collectionName](const Table& refreshedCollection) {
        const auto collIt = std::ranges::find_if(
            collections, [&collectionName](const Table& t) { return t.name == collectionName; });

        if (collIt != collections.end()) {
            *collIt = refreshedCollection;
            Logger::info(std::format("Collection {} refreshed successfully", collectionName));
        }

        collectionRefreshLoaders.erase(collectionName);
    });
}

Table MongoDBDatabaseNode::refreshCollectionAsync(const std::string& collectionName) {
    Logger::debug(std::format("Refreshing collection: {}", collectionName));

    Table refreshedCollection;
    refreshedCollection.name = collectionName;
    refreshedCollection.fullName =
        parentDb->getConnectionInfo().name + "." + name + "." + collectionName;

    try {
        refreshedCollection.columns = inferSchemaFromSample(collectionName, 100);
    } catch (const std::exception& e) {
        Logger::error(std::format("Error refreshing collection {}: {}", collectionName, e.what()));
        throw;
    }

    return refreshedCollection;
}

bool MongoDBDatabaseNode::isTableRefreshing(const std::string& collectionName) const {
    auto it = collectionRefreshLoaders.find(collectionName);
    bool isRefreshing = it != collectionRefreshLoaders.end() && it->second.isRunning();
    if (isRefreshing) {
        Logger::debug(std::format("Collection {} is currently refreshing", collectionName));
    }
    return isRefreshing;
}

std::vector<std::vector<std::string>>
MongoDBDatabaseNode::getTableData(const std::string& collectionName, const int limit,
                                  const int offset, const std::string& filter,
                                  const std::string& sort) {
    std::vector<std::vector<std::string>> result;

    try {
        // Get column names to know which fields to extract
        auto columnNames = getColumnNames(collectionName);

        auto client = parentDb->getClient();
        auto db = (*client)[name];
        auto coll = db[collectionName];

        mongocxx::options::find opts;
        opts.limit(limit);
        opts.skip(offset);

        bsoncxx::document::view_or_value filterDoc = bsoncxx::builder::stream::document{}
                                                     << bsoncxx::builder::stream::finalize;
        if (!filter.empty()) {
            try {
                filterDoc = bsoncxx::from_json(filter);
            } catch (...) {
                // Invalid filter, use empty
            }
        }

        if (!sort.empty()) {
            try {
                opts.sort(bsoncxx::from_json(sort));
            } catch (...) {
                // Invalid sort, ignore
            }
        }

        auto cursor = coll.find(filterDoc, opts);

        for (auto&& doc : cursor) {
            std::vector<std::string> row;
            row.reserve(columnNames.size());

            // Extract value for each column
            for (const auto& colName : columnNames) {
                if (auto elem = doc[colName]) {
                    row.push_back(bsonElementToString(elem));
                } else {
                    row.push_back(""); // Field not present in this document
                }
            }

            result.push_back(std::move(row));
        }
    } catch (const std::exception& e) {
        Logger::error(
            std::format("Error getting collection data for {}: {}", collectionName, e.what()));
    }

    return result;
}

std::vector<std::string> MongoDBDatabaseNode::getColumnNames(const std::string& collectionName) {
    // Find the collection in our loaded collections to get inferred schema
    const auto it = std::ranges::find_if(
        collections, [&collectionName](const Table& t) { return t.name == collectionName; });

    if (it != collections.end() && !it->columns.empty()) {
        std::vector<std::string> names;
        names.reserve(it->columns.size());
        for (const auto& col : it->columns) {
            names.push_back(col.name);
        }
        return names;
    }

    // Fallback if schema not yet inferred
    return {"_id", "document"};
}

int MongoDBDatabaseNode::getRowCount(const std::string& collectionName, const std::string& filter) {
    try {
        auto client = parentDb->getClient();
        auto db = (*client)[name];
        auto coll = db[collectionName];

        bsoncxx::document::view_or_value filterDoc = bsoncxx::builder::stream::document{}
                                                     << bsoncxx::builder::stream::finalize;
        if (!filter.empty()) {
            try {
                filterDoc = bsoncxx::from_json(filter);
            } catch (...) {
                // Invalid filter, use empty
            }
        }

        return static_cast<int>(coll.count_documents(filterDoc));
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting row count for {}: {}", collectionName, e.what()));
        return 0;
    }
}

std::vector<QueryResult> MongoDBDatabaseNode::executeQueryWithResult(const std::string& query,
                                                                     const int rowLimit) {
    // Delegate to parent for JSON command execution
    if (parentDb) {
        return parentDb->executeQueryWithResult(query, rowLimit);
    }

    QueryResult result;
    result.success = false;
    result.errorMessage = "No parent database connection";
    return {result};
}

std::pair<bool, std::string> MongoDBDatabaseNode::executeQuery(const std::string& query) {
    // Delegate to parent for JSON command execution
    if (parentDb) {
        return parentDb->executeQuery(query);
    }
    return {false, "No parent database connection"};
}

void MongoDBDatabaseNode::checkLoadingStatus() {
    checkCollectionsStatusAsync();
    // Views are handled as part of collections
}
