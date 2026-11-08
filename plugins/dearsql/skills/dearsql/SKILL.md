---
name: dearsql
description: Work with the user's databases (PostgreSQL, MySQL, SQLite, DuckDB, SQL Server, Oracle, MongoDB, Redis, Cassandra...) and S3 storage through DearSQL's saved connections. Use when the user asks about their data, a schema, a slow query, missing or duplicate indexes, differences between two databases, or files in an S3 bucket. Not for editing application code that merely contains SQL.
---

# DearSQL

DearSQL is the user's SQL client. Its saved connections (credentials encrypted, SSH tunnels included) are available to you as tools, over MCP (`dearsql --mcp`, tool names below) or as shell commands (`dearsql <tool> ...`, same names, same output). Results are compact text.

## Setup

- MCP tools present (`search_schema`, `run_query`, ...): use them.
- Otherwise run `dearsql tools` once. It lists every command with its flags. `-c <connection>` takes a saved name, a URL or a database file; `-d <database>` picks the database (`db.schema` on PostgreSQL and SQL Server).
- `dearsql: command not found`: ask the user to install it (DearSQL → Settings → Command Line → Install 'dearsql' Command, or `curl -fsSL https://dearsql.dev/install.sh | sh`). Don't hunt for credentials elsewhere.

## Finding your way

1. `list_connections` when you don't know which connection. `*` marks what the user has selected; connection and database default to it.
2. `search_schema` with 1-3 words ("invoice total", typos fine) before writing any SQL. Never guess table or column names.
3. `describe_table` before a join or filter: types, keys, indexes, foreign keys in and out.
4. `run_query` for one read-only statement. Put LIMIT (TOP on SQL Server) on exploration, aggregate in SQL instead of paging rows, and pass the `cursor` a result ends with to get the next page.
5. `profile_table` before filtering on data you haven't seen: per column the null %, distinct count, min and max, and the common values of low-cardinality columns (first 100,000 rows, one bounded query).

## Slow queries and indexes

- `explain_query` shows the plan without running it, then warnings tagged with a code (`[filtered-full-scan]`, `[repeated-full-scan]`, `[sort-spill]`, `[big-sort]`, `[misestimate]`, `[automatic-index]`). `analyze: true` runs the statement for real numbers: read-only statements only, and it takes as long as the query.
- `review_indexes` checks the schema for foreign keys with no index (`[unindexed-fk]`) and indexes another one covers (`[redundant-index]`), with a statement to consider. One table with `table`, or all of them (up to 200).
- `explain_finding <code>` gives the fixed explanation: what it means, how to confirm, the usual fix, when to leave it. Quote it rather than improvising.
- The warnings are rule-based hints, not verdicts. Confirm before recommending: check row counts, how the table is queried, and that an index isn't needed by a constraint. Present `CREATE INDEX` / `DROP INDEX` to the user; on a big production table mention `CREATE INDEX CONCURRENTLY` (PostgreSQL) or an online DDL.

## Comparing schemas

- `diff_schema` compares two databases (`otherConnection`, `otherDatabase`; dev against prod, or two schemas) and lists the statements that would make the first match the second: creates first, drops last, views left out. Nothing runs. Show the script, point out the drops (they lose data), and apply only what the user picks, through `execute_sql`.

## Changes

- `run_query` rejects writes, and so should you unless the user asked for the change.
- When they did: `execute_sql` (MCP; the user approves each call, in the DearSQL chat or in your client) or `dearsql exec` in a shell. Show the exact statement first, one statement per call, with a WHERE you have checked with a `SELECT count(*)`.
- Connections marked read-only in DearSQL refuse writes. Don't work around it.

## S3 storage

- `list_objects` lists buckets, or one folder of a bucket (`bucket`, `prefix`, a `cursor` for more). `read_object` returns the start of a file as text (`maxBytes`, default 16 KB): enough to see a CSV's header or a log's shape.
- `copy_object` (with `move`), `delete_object`, `upload_object`, `download_object` change things: same rules as writes. A key ending in `/` is a folder and everything in it.

## Don'ts

- Don't read DearSQL's files (`~/.dearsql/connections.db`, `master.key`) or ask for passwords: the tools already connect.
- Don't paste large result sets back to the user; summarize, and show the query so they can rerun it.
- Don't run `analyze` or any write against a connection whose name or tags suggest production without saying so first.
