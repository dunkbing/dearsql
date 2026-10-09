#pragma once

#include <dearsql/completion.hpp>
#include <dearsql/database.hpp>

// what dearsql::complete may offer for one library database or schema handle:
// its tables, views, sequences and routines (every schema's, qualified, when
// `db` is a database with schemas). throws dearsql::Error like the calls it makes.
dearsql::CompletionCatalog loadCompletionCatalog(const dearsql::DatabasePtr& db);
