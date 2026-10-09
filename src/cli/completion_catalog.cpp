#include "cli/completion_catalog.hpp"

namespace {

    // one handle's objects, filed under `schema`
    void addObjects(dearsql::CompletionCatalog& out, dearsql::IDatabase& db,
                    const std::string& schema) {
        for (auto& t : db.tables()) {
            t.schema = schema;
            out.tables.push_back(std::move(t));
        }
        auto views = db.views();
        auto mviews = db.materializedViews();
        views.insert(views.end(), std::make_move_iterator(mviews.begin()),
                     std::make_move_iterator(mviews.end()));
        for (auto& v : views) {
            v.schema = schema;
            out.views.push_back(std::move(v));
        }
        // nice to have: a backend that cannot list them still completes tables
        try {
            for (auto& s : db.sequences())
                out.sequences.push_back({schema, std::move(s)});
            auto routines = db.routines();
            out.routines.insert(out.routines.end(), std::make_move_iterator(routines.begin()),
                                std::make_move_iterator(routines.end()));
        } catch (const std::exception&) {
        }
        if (!schema.empty())
            out.schemas.push_back(schema);
    }

} // namespace

dearsql::CompletionCatalog loadCompletionCatalog(const dearsql::DatabasePtr& db) {
    dearsql::CompletionCatalog out;
    if (!db)
        return out;
    // a database with schemas (postgres, sql server): all of them, qualified
    if (auto schemas = db->schemas(); !schemas.empty()) {
        for (auto& s : schemas)
            addObjects(out, *s, s->name());
        const auto type = db->type();
        out.defaultSchema = type == dearsql::DatabaseType::MSSQL ? "dbo" : "public";
        return out;
    }
    // a schema handle, or a backend without schemas
    out.defaultSchema = db->schemaName();
    addObjects(out, *db, out.defaultSchema);
    return out;
}
