#pragma once

#include "database/db.hpp"

class IDatabaseNode;

// Compare Schema With (a database's menu) and Profile Columns (a table's menu). Both
// open their result as a script in a new SQL editor tab: the diff to review and run
// by hand, the profile query run once.
namespace SchemaCompare {
    // submenu listing the other open databases (or schemas) of the same dialect;
    // picking one describes both sides on a worker
    void renderMenu(IDatabaseNode* node);
    // per frame: opens the script tab once a compare finishes
    void poll();
    // the diff that makes `from` match `to`, on a worker; poll opens it
    void compare(IDatabaseNode* from, IDatabaseNode* to);
    // a SQL editor on node with the table's per-column profile, already running
    void openProfile(IDatabaseNode* node, const Table& table);
} // namespace SchemaCompare
