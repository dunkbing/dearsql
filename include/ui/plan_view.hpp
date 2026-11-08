#pragma once

#include <dearsql/explain.hpp>

// a query plan as a tree table: operation, rows (estimated vs actual), cost, own time
namespace PlanView {
    void render(const dearsql::Plan& plan, bool& showRaw);
}
