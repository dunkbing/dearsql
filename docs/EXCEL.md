# Excel support (design)

Status:

- Phase 1 (export): built.
- Phase 3 (open and query): built in part. A `.xlsx` opens read-only (sidebar menu **Open Excel File...**, drag and drop, `dearsql query -c book.xlsx`), each sheet a table in a shared in-memory SQLite database (libdearsql `src/xlsx_workbook.cpp`), connected off the UI thread. Not yet: the sheet-tab viewer, number-format rendering, merged cells, the header toggle and ranges, the 1904 date system. Workbooks over 20 MB are refused: OpenXLSX keeps the sheet as a DOM, measured at 7.5 s and 1.1 GB for a 9.8 MB file (200k × 10); streaming the sheet XML is the fix.
- Phase 2 (import): built in part. **Import → Excel workbook** on a table appends the sheet named like the table (else the first), columns matched by name, through the CSV importer (batches, a bad row costs only itself, rows already in stay on cancel). Not yet: sheet picker, preview with editable types, create-new-table, one transaction, errors naming the cell.
- Phase 4 (edit): not started.

## What people need from Excel in a database client

Ranked by how often a DearSQL user meets the need:

1. **Export to Excel.** Query results and tables go to people who live in Excel. Today they get CSV, and Excel mangles it (leading zeros, dates, UTF-8 without a BOM, long numbers in scientific notation).
2. **Import a sheet into a table.** Someone sends a spreadsheet; it has to become a table in Postgres/MySQL/SQLite with sensible column types.
3. **Open and query a workbook.** Look at a `.xlsx` like a database: each sheet a table, filter it, join two sheets, run SQL over it.
4. **Edit a workbook.** Fix a few cells, add or delete rows, and save without destroying the formatting.

Out of scope: formula evaluation, charts, pivot tables, macros, `.xls`/`.xlsb`/`.ods`, password-protected files. DearSQL is a database client, not a spreadsheet; the line is "data in sheets", not "everything Excel does".

## Libraries

**Writing: libxlsxwriter** (vcpkg `libxlsxwriter`, features `openssl-md5` and `dtoa`). OpenXLSX was tried first: it looks every new string up linearly in the shared-string table, so 25,000 unique strings took 3.8 s and 200,000 would take minutes, and it keeps the whole workbook in memory with no freeze-pane or autofilter API. libxlsxwriter in constant-memory mode streams rows through temp files (200,000 rows in under 1 s) and writes a frozen header and an autofilter. Its bundled MD5 clashes with OpenSSL's, hence `openssl-md5`; `dtoa` formats numbers independently of the process locale.

**Reading: OpenXLSX** (BSD-3, C++17, CMake target `OpenXLSX::OpenXLSX`, pugixml + miniz, vcpkg port): reads and writes cell values (text, numbers, booleans, dates), formulas (stored, not evaluated), sheets, merged cells, styles and number formats, comments.

Three things it brings that the design has to respect:

- **Whole workbook in memory** (a pugixml DOM; the README sizes 1M rows × 8-16 columns at about 8 GB). Opening has a size limit and a progress state, and big files open read-only.
- **No formula evaluation.** A formula cell shows the value Excel cached when it last saved. If we write cells, we set the workbook's "recalculate on open" flag so Excel recomputes.
- **Round-trip fidelity is not documented.** Whether saving keeps parts it does not model (charts, images, data validation, tables) has to be tested on real files before editing ships (spike, below). Until proven, editing a workbook that has such parts saves to a copy.
- Housekeeping: development moved from GitHub to Codeberg (`codeberg.org/lars_uffmann/OpenXLSX`), last release v0.5.1 (June 2026). Pin a version through vcpkg; the GitHub repo is marked discontinued.

**Why not DuckDB's `excel` extension** (`read_xlsx`, `COPY ... (FORMAT xlsx)`): it is downloaded from DuckDB's extension repository on first use, so it fails offline and on locked-down machines, iOS has no DuckDB at all, it writes one sheet per file, and it cannot edit. It stays useful for agents running DuckDB themselves, but DearSQL should not depend on it.

## Architecture (mirrors the CSV editor)

- **`XlsxDocument`** (`include/utils/xlsx_document.hpp`, no ImGui; like `CsvDocument`): wraps an OpenXLSX document. Sheets, used range, typed cell reads (value + number-format kind: text, number, date, time, bool, error, formula with cached value), merged ranges, column widths, frozen panes; edits as undo steps; `save()` through a temp file and a rename. Unit-tested without UI.
- **Query side: an in-memory SQLite database per opened workbook.** Each sheet becomes a table (header row → column names, deduplicated and quoted; types inferred from cell kinds over the whole column, not the first row). It runs everywhere SQLite does (desktop, iOS, Android), needs no network, and plugs into `FileDatabase` the way a CSV plugs into DuckDB: the sidebar lists the sheets, the SQL editor and the agent tools query them. Read-only, like the CSV view: edits go through the document, then the sheet's table is rebuilt.
- **`XlsxEditorTab`** (like `CsvEditorTab`): sheet tabs along the bottom, `TableRenderer` grid, the filter box (a SQL `WHERE` against the sheet's table, mapped back to document rows), save/revert/undo/redo.
- **Export** (built) is one more `ExportFormat` (`XLSX`) in the existing `TableExporter` (`include/utils/table_exporter.hpp`): it already pages rows out on a worker with progress and cancel, and its Export submenu is on tables and results. **Import** reuses the dump import UI (`src/ui/database_node.cpp`: progress panel, cancel, preview modal).

## Features

### Phase 1: Export (highest value, smallest)

- **Results → Excel** (built): the **Excel** button under SQL editor results writes every result set of the run, one sheet each (the rows shown, so at most the editor's 1,000-row cap).
- **Table → Excel** from the sidebar menu, all rows (streamed in pages; see limits).
- Types kept: numbers as numbers, dates/timestamps as Excel dates with a date format, booleans as TRUE/FALSE, NULL as an empty cell, text as text (so `00123` stays `00123`). Integers above 2^53 and decimals with more precision than a double are written as text, so nothing is silently rounded.
- Header row bold and frozen, column widths sized to the first 200 rows (capped at 60), an autofilter on the header.
- Limits: Excel's 1,048,576 rows and 16,384 columns per sheet. Past that, the export says so and continues on a new sheet (`Sheet1 (2)`) rather than truncating silently.
- Agents (new, optional): an `export_query` tool that writes a result to a `.xlsx` file, asking for approval in the GUI like writes; in the terminal `dearsql query ... --out results.xlsx`. There is no export tool today.

### Phase 2: Import a sheet into a table

- *Import → From Excel* on a database or schema: pick a file, a sheet and optionally a range (`A1:F200`); preview the first 100 rows with the inferred column names and types (editable: rename, change type, skip column); choose create new table or append to an existing one (columns matched by name).
- Type inference over the whole column: all numbers with no fraction → integer (bigint when needed), numbers → numeric/double, all dates → date or timestamp, TRUE/FALSE → boolean, anything mixed → text. Empty cells → NULL.
- Runs in one transaction with batched inserts, progress and cancel; on error nothing is half-imported, and the failing row and cell (`Sheet1!C42`) are named.
- Reuses the dialect SQL builders for `CREATE TABLE` and quoting (libdearsql).

### Phase 3: Open and query a workbook

- Open `.xlsx` like a `.sqlite` file: drag onto the window, *Open*, double-click in the sidebar, `dearsql query -c book.xlsx`.
- Sidebar: the workbook as a connection, each sheet a table (columns and inferred types in the table inspector).
- Viewer: sheet tabs, typed cells rendered with their number format (dates as dates, percentages, thousands separators), merged cells drawn merged, formula cells show the cached value with the formula in the cell inspector, frozen panes honoured.
- SQL across sheets: `SELECT ... FROM "Orders" o JOIN "Customers" c ON ...`.
- Header detection per sheet (first row all text → header), with a toggle when it guesses wrong; a sheet whose data starts lower down can be pointed at a range.
- Agent tools work unchanged (`list_tables`, `describe_table`, `run_query`) because the sheets are tables.

### Phase 4: Edit and save

- Edit values, paste blocks (already supported by `TableRenderer`), insert/delete rows, sort, undo/redo, filtered rows stay editable (as in the CSV editor).
- Save keeps styles, column widths, merged cells and other sheets as they were; written cells keep their cell's style. Formula cells: editing replaces the formula with a value only after a confirmation; the workbook is flagged to recalculate on open.
- Add, rename, delete and reorder sheets.
- Workbooks with parts the spike shows OpenXLSX drops on save open with editing on but **Save** writes a copy (`book (edited).xlsx`) and says why.

## Spike before Phase 3/4 (1-2 days)

1. Add OpenXLSX through vcpkg (desktop + iOS + Android triplets), check build size.
2. Round trip: open and save unchanged, and with one edited cell, a set of real workbooks (charts, images, data validation, Excel tables, pivot, conditional formatting, comments, external links). Diff the zip parts before and after; record what survives.
3. Memory and time for 100k × 20 and 1M × 10 sheets; set the open-read-only threshold from it.
4. Dates: 1900 vs 1904 date systems, time-only cells, text that looks like a date.

## Limits and risks

- Big workbooks: whole-file memory; a size threshold opens read-only with a warning (number from the spike).
- Formulas are never recomputed by DearSQL; values come from Excel's last save. Files written by tools that do not cache values (some generators) show empty formula cells; say so in the inspector.
- Library maintenance moved to Codeberg; pin and vendor through vcpkg so an upstream move cannot break builds.

## Docs and website when it ships

New `website/src/pages/docs/excel-files.md` (like `csv-files.md`), import/export page, the landing page feature list and the comparison tables (most competitors already export to Excel; check each claim), changelog entry.
