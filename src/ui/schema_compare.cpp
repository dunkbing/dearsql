#include "ui/schema_compare.hpp"

#include "application.hpp"
#include "database/async_helper.hpp"
#include "database/file_database.hpp"
#include "database/lib_database_node.hpp"
#include "platform/alert.hpp"
#include "themes.hpp"
#include "ui/tab/sql_editor_tab.hpp"
#include "ui/tab_manager.hpp"
#include "utils/menu.hpp"
#include <dearsql/findings.hpp>
#include <dearsql/profile.hpp>
#include <dearsql/schema_diff.hpp>
#include <format>
#include <imgui.h>

namespace SchemaCompare {

    namespace {
        struct Side {
            std::vector<Table> tables;
            std::string schema;
            size_t listed = 0; // tables in the database; more than described when cut
        };
        struct Result {
            IDatabaseNode* node = nullptr; // the side the script is for
            std::string error;
            std::string script;
            std::shared_ptr<void> keep; // both nodes alive until the tab is open
        };

        // ponytail: leaked on purpose, so quitting never waits on a running compare
        AsyncOperation<Result>& op() {
            static auto* o = new AsyncOperation<Result>();
            return *o;
        }

        std::string label(IDatabaseNode* node) {
            auto* owner = node->ownerDatabase();
            const std::string conn = owner ? owner->getConnectionInfo().name : "";
            return conn.empty() ? node->getFullPath() : conn + " / " + node->getFullPath();
        }

        constexpr size_t kMaxTables = 200;

        // tables described (columns, indexes, foreign keys). whole: throws past 200, since a
        // table left out of a compare would show up as one to drop; otherwise the first 200
        Side describe(dearsql::IDatabase& db, bool whole) {
            Side side{.schema = db.schemaName()};
            auto listed = db.tables();
            side.listed = listed.size();
            if (whole && listed.size() > kMaxTables)
                throw std::runtime_error(
                    std::format("{} has {} tables; Compare Schema handles up to {}.", db.name(),
                                listed.size(), kMaxTables));
            if (listed.size() > kMaxTables)
                listed.resize(kMaxTables);
            for (const auto& t : listed) {
                auto full = db.describeTable(t.name);
                if (full.schema.empty())
                    full.schema = t.schema;
                side.tables.push_back(std::move(full));
            }
            return side;
        }

        Side load(IDatabaseNode* node, bool whole) {
            if (auto* lib = dynamic_cast<LibDatabaseNode*>(node))
                return lib->withHandle(
                    [whole](dearsql::IDatabase& db) { return describe(db, whole); });
            if (auto* file = dynamic_cast<FileDatabase*>(node))
                if (auto conn = file->libConnection())
                    if (auto h = conn->database())
                        return describe(*h, whole);
            throw std::runtime_error("this database cannot be described");
        }

    } // namespace

    void compare(IDatabaseNode* from, IDatabaseNode* to) {
        const std::string fromLabel = label(from), toLabel = label(to);
        const auto type = from->getDatabaseType();
        auto keep = std::make_shared<std::pair<std::shared_ptr<void>, std::shared_ptr<void>>>(
            keepOwnerAlive(from), keepOwnerAlive(to));
        op().start([=]() -> Result {
            Result r{.node = from, .keep = keep};
            try {
                const auto a = load(from, true);
                const auto b = load(to, true);
                const auto changes = dearsql::diffSchemas(a.tables, b.tables, type, a.schema);
                r.script = std::format("-- statements that make {} match {}\n", fromLabel, toLabel);
                if (changes.empty()) {
                    r.script += "-- nothing differs in tables, columns, indexes or foreign "
                                "keys (views are not compared)\n";
                    return r;
                }
                r.script += std::format("-- {} change{}, views not compared. nothing has run: "
                                        "review them (drops lose data), then Run\n",
                                        changes.size(), changes.size() == 1 ? "" : "s");
                for (const auto& c : changes) {
                    r.script += "\n-- " + c.message + "\n";
                    if (!c.sql.empty())
                        r.script += c.sql + "\n";
                }
            } catch (const std::exception& e) {
                r.error = e.what();
            }
            return r;
        });
    }

    void reviewIndexes(IDatabaseNode* node) {
        const std::string name = label(node);
        const auto type = node->getDatabaseType();
        auto keep = keepOwnerAlive(node);
        op().start([=]() -> Result {
            Result r{.node = node, .keep = keep};
            try {
                const auto side = load(node, false);
                const auto findings = dearsql::reviewIndexes(side.tables, type);
                r.script = std::format("-- index review of {}: {} finding{}", name, findings.size(),
                                       findings.size() == 1 ? "" : "s");
                if (side.listed > side.tables.size())
                    r.script +=
                        std::format(", first {} of {} tables", side.tables.size(), side.listed);
                r.script += "\n-- rule-based hints, not verdicts: check how each table is queried. "
                            "nothing has run\n";
                if (findings.empty())
                    r.script += "-- no unindexed foreign keys or redundant indexes\n";
                for (const auto& f : findings) {
                    r.script += std::format("\n-- [{}] {}: {}\n", f.code, f.table, f.message);
                    if (const auto* doc = dearsql::findingDoc(f.code))
                        r.script += std::format("-- leave it when: {}\n", doc->ignore);
                    if (!f.suggestion.empty())
                        r.script += f.suggestion + "\n";
                }
            } catch (const std::exception& e) {
                r.error = e.what();
            }
            return r;
        });
    }

    namespace {

        // still open: a node retired while the compare ran gets no tab
        bool stillOpen(IDatabaseNode* node) {
            for (const auto& db : Application::getInstance().getDatabases()) {
                if (!db)
                    continue;
                // a database with loaded schemas lists the schemas, not itself
                for (auto* n : db->loadedNodes())
                    if (n == node || n->parentNode() == node)
                        return true;
            }
            return false;
        }
    } // namespace

    void renderMenu(IDatabaseNode* node) {
        if (!node)
            return;
        const bool busy = op().isRunning();
        if (ImGui::MenuItem(busy ? "Reviewing..." : "Review Indexes", nullptr, false, !busy))
            reviewIndexes(node);
        if (!UIUtils::BeginSubMenu(busy ? "Comparing Schema..." : "Compare Schema With", !busy))
            return;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        bool any = false;
        for (const auto& db : Application::getInstance().getDatabases()) {
            if (!db || !db->isConnected())
                continue;
            for (auto* other : db->loadedNodes()) {
                if (other == node ||
                    !dearsql::sameDialect(node->getDatabaseType(), other->getDatabaseType()))
                    continue;
                any = true;
                if (ImGui::MenuItem(label(other).c_str()))
                    compare(node, other);
            }
        }
        if (!any)
            ImGui::TextDisabled("Open another database of this kind to compare");
        ImGui::PopStyleVar();
        ImGui::EndMenu();
    }

    void poll() {
        op().check([](Result r) {
            if (!r.error.empty()) {
                Alert::show("Could Not Read the Schema", r.error);
                return;
            }
            if (!stillOpen(r.node))
                return;
            auto tab = std::dynamic_pointer_cast<SQLEditorTab>(
                Application::getInstance().getTabManager()->createSQLEditorTab("", r.node));
            if (tab)
                tab->setQuery(r.script);
        });
    }

    void openProfile(IDatabaseNode* node, const Table& table) {
        const auto sql = dearsql::profileRowsSql(node->getDatabaseType(), table);
        if (!sql)
            return;
        auto tab = std::dynamic_pointer_cast<SQLEditorTab>(
            Application::getInstance().getTabManager()->createSQLEditorTab("", node));
        if (!tab)
            return;
        tab->setQuery(*sql);
        tab->run();
    }

} // namespace SchemaCompare
