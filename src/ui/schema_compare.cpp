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
#include <dearsql/profile.hpp>
#include <dearsql/schema_diff.hpp>
#include <format>
#include <imgui.h>

namespace SchemaCompare {

    namespace {
        struct Side {
            std::vector<Table> tables;
            std::string schema;
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

        // every table described (columns, indexes, foreign keys). throws past 200 or on
        // any failure: a table left out would show up as one to drop
        Side describe(dearsql::IDatabase& db) {
            Side side{.schema = db.schemaName()};
            const auto listed = db.tables();
            if (listed.size() > 200)
                throw std::runtime_error(
                    std::format("{} has {} tables; Compare Schema handles up to 200.", db.name(),
                                listed.size()));
            for (const auto& t : listed) {
                auto full = db.describeTable(t.name);
                if (full.schema.empty())
                    full.schema = t.schema;
                side.tables.push_back(std::move(full));
            }
            return side;
        }

        Side load(IDatabaseNode* node) {
            if (auto* lib = dynamic_cast<LibDatabaseNode*>(node))
                return lib->withHandle([](dearsql::IDatabase& db) { return describe(db); });
            if (auto* file = dynamic_cast<FileDatabase*>(node))
                if (auto conn = file->libConnection())
                    if (auto h = conn->database())
                        return describe(*h);
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
                const auto a = load(from);
                const auto b = load(to);
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
                Alert::show("Compare Failed", r.error);
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
