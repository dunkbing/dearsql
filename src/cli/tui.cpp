#include "cli/tui.hpp"

#include <algorithm>
#include <format>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>

using namespace ftxui;

namespace {

    constexpr int PAGE = 200;
    constexpr size_t CELL = 40;

    // ponytail: every call blocks the ui; fine for a terminal client, add a worker
    // thread if slow catalogs make the tree feel stuck
    struct TreeNode {
        enum class Kind { Connection, Database, Schema, Table, View } kind;
        std::string label;
        std::string connection;
        dearsql::DatabasePtr db; // database or schema handle the table lives in
        dearsql::Table table;
        int depth = 0;
        bool expanded = false;
        bool loaded = false;
        std::vector<TreeNode> children;
    };

    std::string clip(const std::string& v) {
        if (dearsql::isNullSentinel(v))
            return "NULL";
        if (dearsql::isBoolSentinel(v))
            return dearsql::boolSentinelValue(v) ? "true" : "false";
        std::string out;
        for (char c : v)
            out += c == '\n' || c == '\t' ? ' ' : c;
        return out.size() > CELL ? out.substr(0, CELL - 1) + "…" : out;
    }

    struct Grid {
        std::vector<std::string> columns;
        std::vector<std::vector<std::string>> rows;
        int row = 0; // selected
        int col = 0; // first visible column
    };

    Element renderGrid(const Grid& g, int height, const std::string& empty) {
        if (g.columns.empty())
            return text(empty) | dim | center;
        const int first = std::max(0, std::min(g.row - height / 2, (int)g.rows.size() - height));
        std::vector<Elements> cells;
        Elements header;
        for (size_t c = g.col; c < g.columns.size(); ++c)
            header.push_back(text(" " + clip(g.columns[c]) + " ") | bold | color(Color::Cyan));
        cells.push_back(header);
        for (int r = first; r < (int)g.rows.size() && r < first + height; ++r) {
            Elements line;
            for (size_t c = g.col; c < g.rows[r].size(); ++c) {
                const auto& v = g.rows[r][c];
                auto cell = text(" " + clip(v) + " ");
                line.push_back(dearsql::isNullSentinel(v) ? cell | dim | italic : cell);
            }
            cells.push_back(line);
        }
        auto table = ftxui::Table(cells);
        table.SelectRow(0).BorderBottom(LIGHT);
        if (g.row >= first && g.row < first + height)
            table.SelectRow(g.row - first + 1).Decorate(bgcolor(Color::Blue) | color(Color::White));
        return hbox({text(" "), table.Render()});
    }

    // "row 3 of 8 · col 2 of 5" under a grid
    std::string gridPosition(const Grid& g) {
        if (g.rows.empty())
            return g.columns.empty() ? "" : "no rows";
        return std::format("row {} of {} · {} columns{}", g.row + 1, g.rows.size(),
                           g.columns.size(), g.col ? std::format(" (from {})", g.col + 1) : "");
    }

    bool gridKeys(Grid& g, const Event& e) {
        const int n = static_cast<int>(g.rows.size());
        if (e == Event::ArrowDown)
            g.row = std::min(n - 1, g.row + 1);
        else if (e == Event::ArrowUp)
            g.row = std::max(0, g.row - 1);
        else if (e == Event::PageDown)
            g.row = std::min(n - 1, g.row + 20);
        else if (e == Event::PageUp)
            g.row = std::max(0, g.row - 20);
        else if (e == Event::ArrowRight)
            g.col = std::min(std::max(0, (int)g.columns.size() - 1), g.col + 1);
        else if (e == Event::ArrowLeft)
            g.col = std::max(0, g.col - 1);
        else
            return false;
        return true;
    }

    Element section(const std::string& title) {
        return text(title) | bold | color(Color::Cyan);
    }

    Element renderStructure(const dearsql::Table& t) {
        if (t.name.empty())
            return text("open a table to see its structure") | dim | center;
        Elements out;
        out.push_back(hbox({text((t.schema.empty() ? "" : t.schema + ".") + t.name) | bold,
                            text(t.sizeBytes >= 0 ? "  " + dearsql::formatByteSize(t.sizeBytes)
                                                  : "") |
                                dim}));
        out.push_back(text(""));

        std::vector<Elements> cols = {{text("column") | dim, text("type") | dim,
                                       text("null") | dim, text("default") | dim}};
        for (const auto& c : t.columns) {
            auto name = text(c.name + " ");
            cols.push_back({c.isPrimaryKey ? hbox({name | bold, text("PK ") | color(Color::Magenta) | bold}) : name,
                            text(c.type + " ") | color(Color::Yellow),
                            text(c.isNotNull || c.isPrimaryKey ? "no " : "yes ") | dim,
                            text(clip(c.defaultValue)) | dim});
        }
        auto columns = ftxui::Table(cols);
        columns.SelectAll().SeparatorVertical(EMPTY);
        out.push_back(section("Columns"));
        out.push_back(columns.Render());

        if (!t.indexes.empty()) {
            out.push_back(text(""));
            out.push_back(section("Indexes"));
            for (const auto& i : t.indexes) {
                std::string cols;
                for (const auto& c : i.columns)
                    cols += (cols.empty() ? "" : ", ") + c;
                out.push_back(hbox({text("  " + i.name + " "), text("(" + cols + ")") | dim,
                                    text(i.isUnique ? "  unique" : "") | color(Color::Magenta)}));
            }
        }
        if (!t.foreignKeys.empty()) {
            out.push_back(text(""));
            out.push_back(section("Foreign keys"));
            for (const auto& fk : t.foreignKeys)
                out.push_back(hbox({text("  " + fk.sourceColumn), text(" → ") | dim,
                                    text(fk.targetTable) | color(Color::Green),
                                    text("(" + fk.targetColumn + ")")}));
        }
        if (!t.definition.empty()) {
            out.push_back(text(""));
            out.push_back(section("Definition"));
            out.push_back(paragraph(t.definition));
        }
        return hbox({text(" "), vbox(std::move(out))});
    }

} // namespace

int runTui(CliConnections& connections, const std::string& initial) {
    auto screen = ScreenInteractive::Fullscreen();
    std::string status = "Pick a connection";
    const std::string hints = "↵ open  ←→ fold  ⇥ pane  F2 data  F3 structure  F4 sql  F5 run  q quit";

    std::vector<TreeNode> roots;
    for (auto* e : connections.entries()) {
        roots.push_back({.kind = TreeNode::Kind::Connection,
                         .label = e->info.name + "  " + databaseTypeToString(e->info.type),
                         .connection = e->info.name});
    }

    auto tablesOf = [](const dearsql::DatabasePtr& db, const std::string& conn, int depth) {
        std::vector<TreeNode> out;
        for (auto& t : db->tables())
            out.push_back({.kind = TreeNode::Kind::Table,
                           .label = t.name,
                           .connection = conn,
                           .db = db,
                           .table = t,
                           .depth = depth,
                           .loaded = true});
        for (auto& v : db->views())
            out.push_back({.kind = TreeNode::Kind::View,
                           .label = v.name + "  (view)",
                           .connection = conn,
                           .db = db,
                           .table = v,
                           .depth = depth,
                           .loaded = true});
        return out;
    };

    auto expand = [&](TreeNode& n) {
        if (n.loaded) {
            n.expanded = true;
            return;
        }
        try {
            if (n.kind == TreeNode::Kind::Connection) {
                if (auto err = connections.open(n.connection); !err.empty()) {
                    status = n.connection + ": " + err;
                    return;
                }
                auto conn = connections.connection(n.connection);
                auto dbs = conn->databases();
                if (isFileDatabase(conn->type()) && !dbs.empty()) {
                    n.children = tablesOf(dbs[0], n.connection, n.depth + 1);
                } else {
                    for (auto& db : dbs)
                        n.children.push_back({.kind = TreeNode::Kind::Database,
                                              .label = db->name(),
                                              .connection = n.connection,
                                              .db = db,
                                              .depth = n.depth + 1});
                }
            } else if (n.kind == TreeNode::Kind::Database || n.kind == TreeNode::Kind::Schema) {
                auto schemas = n.kind == TreeNode::Kind::Database
                                   ? n.db->schemas()
                                   : std::vector<dearsql::DatabasePtr>{};
                if (!schemas.empty()) {
                    for (auto& s : schemas)
                        n.children.push_back({.kind = TreeNode::Kind::Schema,
                                              .label = s->name(),
                                              .connection = n.connection,
                                              .db = s,
                                              .depth = n.depth + 1});
                } else {
                    n.children = tablesOf(n.db, n.connection, n.depth + 1);
                }
            }
            n.loaded = true;
            n.expanded = true;
            status = std::format("{}: {} items", n.label, n.children.size());
        } catch (const std::exception& e) {
            status = n.label + ": " + e.what();
        }
    };

    // the tree flattened into menu lines, rebuilt each frame
    std::vector<TreeNode*> visible;
    std::vector<std::string> labels;
    auto flatten = [&] {
        visible.clear();
        labels.clear();
        std::function<void(std::vector<TreeNode>&)> walk = [&](std::vector<TreeNode>& nodes) {
            for (auto& n : nodes) {
                visible.push_back(&n);
                const bool leaf = n.kind == TreeNode::Kind::Table || n.kind == TreeNode::Kind::View;
                labels.push_back(std::string(n.depth * 2, ' ') +
                                 (leaf         ? "  "
                                  : n.expanded ? "▾ "
                                               : "▸ ") +
                                 n.label);
                if (n.expanded)
                    walk(n.children);
            }
        };
        walk(roots);
    };
    flatten();

    int selected = 0;
    int tab = 0;
    Grid data;
    Grid result;
    dearsql::Table structure;
    std::string sql;
    std::string resultInfo;
    TreeNode* current = nullptr; // last opened table
    dearsql::DatabasePtr target; // where the SQL tab runs

    auto openTable = [&](TreeNode& n) {
        current = &n;
        target = n.db;
        data = Grid{};
        try {
            data.columns = n.db->getColumnNames(n.table);
            data.rows = n.db->getTableData(n.table, PAGE, 0);
            structure =
                n.kind == TreeNode::Kind::Table ? n.db->describeTable(n.table.name) : n.table;
            status = std::format("{}: first {} rows", n.table.name, data.rows.size());
        } catch (const std::exception& e) {
            status = n.table.name + ": " + e.what();
        }
        if (sql.empty())
            sql = "SELECT * FROM " + n.table.name + " LIMIT 100";
    };

    auto runSql = [&] {
        if (!target) {
            status = "open a table or database first; the query runs there";
            return;
        }
        auto r = target->execute(sql, 1000);
        result = Grid{};
        if (!r.success()) {
            resultInfo = "Error: " + r.errorMessage();
            return;
        }
        resultInfo = std::format("{:.0f} ms", r.executionTimeMs);
        for (const auto& s : r.statements) {
            if (!s.columnNames.empty()) {
                result.columns = s.columnNames;
                result.rows = s.tableData;
                resultInfo = std::format("{} rows · {}", s.tableData.size(), resultInfo);
            } else if (s.affectedRows) {
                resultInfo = std::format("{} rows affected · {}", s.affectedRows, resultInfo);
            }
        }
    };

    // enter on a tree line: open a table, fold anything else
    auto activate = [&] {
        if (selected < 0 || selected >= (int)visible.size())
            return;
        TreeNode& n = *visible[selected];
        if (n.kind == TreeNode::Kind::Table || n.kind == TreeNode::Kind::View) {
            openTable(n);
            tab = 0;
        } else {
            if (n.expanded)
                n.expanded = false;
            else
                expand(n);
            if (n.db)
                target = n.db;
        }
        flatten();
    };

    MenuOption menuOpt = MenuOption::Vertical();
    menuOpt.entries_option.transform = [&](const EntryState& s) {
        if (s.index < 0 || s.index >= (int)visible.size())
            return text(s.label);
        const TreeNode& n = *visible[s.index];
        const bool leaf = n.kind == TreeNode::Kind::Table || n.kind == TreeNode::Kind::View;
        Element label;
        switch (n.kind) {
        case TreeNode::Kind::Connection: {
            const auto gap = n.label.find("  ");
            label = hbox({text(n.label.substr(0, gap)) | bold,
                          text(gap == std::string::npos ? "" : n.label.substr(gap)) | dim});
            break;
        }
        case TreeNode::Kind::Database:
        case TreeNode::Kind::Schema:
            label = text(n.label) | color(Color::Cyan);
            break;
        case TreeNode::Kind::Table:
            label = text(n.label);
            break;
        case TreeNode::Kind::View:
            label = text(n.label) | italic | dim;
            break;
        }
        auto line = hbox({text(std::string(n.depth * 2, ' ')),
                          text(leaf ? "  " : n.expanded ? "▾ " : "▸ ") | dim, label, filler()});
        if (s.active && s.focused)
            return line | bgcolor(Color::Blue) | color(Color::White);
        if (s.active)
            return line | inverted;
        return n.kind == TreeNode::Kind::Table && current == &n ? line | bold : line;
    };
    auto tree = Menu(&labels, &selected, menuOpt);
    tree |= CatchEvent([&](Event e) {
        if (selected < 0 || selected >= (int)visible.size())
            return false;
        if (e == Event::Return) {
            activate();
            return true;
        }
        TreeNode& n = *visible[selected];
        if (e == Event::ArrowRight && !n.expanded && n.kind != TreeNode::Kind::Table &&
            n.kind != TreeNode::Kind::View) {
            expand(n);
            flatten();
            return true;
        }
        if (e == Event::ArrowLeft && n.expanded) {
            n.expanded = false;
            flatten();
            return true;
        }
        return false;
    });

    auto dataView =
        Renderer([&](bool) {
            return vbox({hbox({text(current ? " " + current->table.name : "") | bold, filler(),
                               text(gridPosition(data) + " ") | dim}),
                         renderGrid(data, std::max(5, screen.dimy() - 9),
                                    "open a table from the tree (↵)") |
                             flex});
        }) |
        CatchEvent([&](Event e) { return gridKeys(data, e); });

    auto structView = Renderer([&](bool) {
        return renderStructure(structure) | vscroll_indicator | frame;
    });

    InputOption inputOpt;
    inputOpt.multiline = true;
    auto editor = Input(&sql, "SQL — F5 runs it", inputOpt);
    auto resultView =
        Renderer([&](bool) {
            const bool failed = resultInfo.starts_with("Error");
            return vbox({hbox({text(" " + resultInfo) | (failed ? color(Color::Red) : dim),
                               filler(), text(gridPosition(result) + " ") | dim}),
                         renderGrid(result, std::max(5, screen.dimy() / 2 - 6),
                                    failed ? "" : "F5 runs the query")});
        }) |
        CatchEvent([&](Event e) { return gridKeys(result, e); });
    auto sqlView = Container::Vertical({editor, resultView});
    auto sqlPane = Renderer(sqlView, [&] {
        return vbox({window(text(" Query ") | dim, editor->Render() | size(HEIGHT, LESS_THAN, 10)),
                     resultView->Render() | flex});
    });

    auto right = Container::Tab({dataView, structView, sqlPane}, &tab);
    auto layout = Container::Horizontal({tree, right});

    auto root = Renderer(layout, [&] {
        const std::vector<std::string> names = {"F2 Data", "F3 Structure", "F4 SQL"};
        Elements tabs;
        for (int i = 0; i < 3; ++i) {
            auto t = text(" " + names[i] + " ");
            tabs.push_back(i == tab ? t | bold | bgcolor(Color::Blue) | color(Color::White)
                                    : t | dim);
        }
        auto paneTitle = [](const std::string& t, bool focused) {
            return text(" " + t + " ") | (focused ? bold : dim);
        };
        return vbox({
            hbox({text(" DearSQL ") | bold | color(Color::Cyan), text(" "), hbox(tabs), filler(),
                  text(target ? "⛁ " + target->name() + " " : "") | color(Color::Green)}),
            hbox({window(paneTitle("Connections", tree->Focused()),
                         tree->Render() | vscroll_indicator | frame) |
                      size(WIDTH, EQUAL, 36),
                  window(paneTitle(tab == 0   ? "Data"
                                   : tab == 1 ? "Structure"
                                              : "SQL",
                                   !tree->Focused()),
                         right->Render()) |
                      flex}) |
                flex,
            hbox({text(" " + status), filler(), text(hints + " ") | dim}),
        });
    });

    root |= CatchEvent([&](Event e) {
        if (e == Event::F2) {
            tab = 0;
            return true;
        }
        if (e == Event::F3) {
            tab = 1;
            return true;
        }
        if (e == Event::F4) {
            tab = 2;
            layout->SetActiveChild(right);
            editor->TakeFocus();
            return true;
        }
        if (e == Event::F5) {
            tab = 2;
            runSql();
            return true;
        }
        if (e == Event::Tab) {
            if (tree->Focused())
                right->TakeFocus();
            else
                tree->TakeFocus();
            return true;
        }
        if (e == Event::Character('q') && tree->Focused()) {
            screen.Exit();
            return true;
        }
        return false;
    });

    // open the connection named on the command line
    if (!initial.empty()) {
        for (size_t i = 0; i < roots.size(); ++i) {
            if (roots[i].connection == initial) {
                expand(roots[i]);
                flatten();
                selected = static_cast<int>(i) + (roots[i].children.empty() ? 0 : 1);
            }
        }
    }

    screen.Loop(root);
    return 0;
}
