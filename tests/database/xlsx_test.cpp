#include "database/db.hpp"
#include "utils/xlsx.hpp"
#include <OpenXLSX.hpp>
#include <chrono>
#include <dearsql/factory.hpp>
#include <fstream>
#include <filesystem>
#include <format>
#include <gtest/gtest.h>

using OpenXLSX::XLValueType;

namespace {
    std::string tmpPath(const char* name) {
        return (std::filesystem::temp_directory_path() / name).string();
    }
} // namespace

TEST(Xlsx, SheetNamesAreValidAndUnique) {
    EXPECT_EQ(xlsx::sheetName("orders", {}), "orders");
    EXPECT_EQ(xlsx::sheetName("a/b:c[d]*?\\", {}), "a_b_c_d____");
    EXPECT_EQ(xlsx::sheetName("'quoted'", {}), "quoted");
    EXPECT_EQ(xlsx::sheetName("", {}), "Sheet");
    EXPECT_EQ(xlsx::sheetName("Orders", {"orders"}), "Orders (2)");
    EXPECT_EQ(xlsx::sheetName("x", {"x", "X (2)"}), "x (3)");
    const std::string longName(40, 'n');
    EXPECT_EQ(xlsx::sheetName(longName, {}).size(), 31u);
    const auto second = xlsx::sheetName(longName, {std::string(31, 'n')});
    EXPECT_EQ(second.size(), 31u);
    EXPECT_TRUE(second.ends_with(" (2)"));
}

TEST(Xlsx, OnlyExactNumbersBecomeNumbers) {
    EXPECT_EQ(xlsx::plainNumber("42"), 42.0);
    EXPECT_EQ(xlsx::plainNumber("-0.5"), -0.5);
    EXPECT_EQ(xlsx::plainNumber("19.90"), 19.9);
    EXPECT_EQ(xlsx::plainNumber("0"), 0.0);
    for (const char* text : {"007", "1e5", "12345678901234567", "1.", "", "-", "1 000", "0x1F",
                             "123456789012.3456", "NaN"})
        EXPECT_FALSE(xlsx::plainNumber(text)) << text;
}

TEST(Xlsx, DatesBecomeSerials) {
    EXPECT_EQ(xlsx::dateSerial("1900-03-01"), 61.0);
    EXPECT_EQ(xlsx::dateSerial("2024-01-01"), 45292.0);
    EXPECT_TRUE(xlsx::dateSerial("2024-02-29")); // leap day
    EXPECT_DOUBLE_EQ(*xlsx::dateSerial("2024-01-01 12:00:00"), 45292.5);
    EXPECT_DOUBLE_EQ(*xlsx::dateSerial("2024-01-01T06:00:00.5"), 45292.25 + 0.5 / 86400);
    for (const char* text : {"2024-13-01", "2024-01-01 12:00:00+02", "1899-12-31", "01/02/2024",
                             "2024-01-01 1:00:00", "today", "2026-02-31", "2025-02-29", "2024-04-31"})
        EXPECT_FALSE(xlsx::dateSerial(text)) << text;
}

TEST(Xlsx, WritesTypedCellsAndSplitsLongSheets) {
    const auto path = tmpPath("dearsql_xlsx_test.xlsx");
    std::filesystem::remove(path);
    {
        xlsx::Writer w(path, 3); // 3 data rows per sheet, so the split is exercised
        w.beginSheet("people/2024", {"id", "zip", "big_id", "price", "born", "active", "note"},
                     {"integer", "varchar(10)", "", "", "date", "boolean", "text"});
        w.addRow({"1", "00123", "12345678901234567890", "19.90", "1990-05-17",
                  std::string(BOOL_TRUE_SENTINEL), std::string(NULL_SENTINEL)});
        w.addRow({"2", "02134", "7", "0.5", "not a date", std::string(BOOL_FALSE_SENTINEL),
                  std::string("bell\x07 removed")});
        w.addRow({"3", "1", "1", "1", "2000-01-01", std::string(NULL_SENTINEL), "x"});
        w.addRow({"4", "1", "1", "1", "2000-01-01", std::string(NULL_SENTINEL),
                  std::string(40000, 'y')});
        w.beginSheet("people/2024", {"n"}); // same name again: made unique
        w.addRow({"5"});
        std::string error;
        ASSERT_TRUE(w.save(error)) << error;
        EXPECT_EQ(w.sheets(), 3u);
        EXPECT_EQ(w.truncatedCells(), 1u);
    }
    ASSERT_FALSE(std::filesystem::exists(path + ".part"));

    OpenXLSX::XLDocument doc;
    doc.open(path);
    const auto names = doc.workbook().worksheetNames();
    ASSERT_EQ(names,
              (std::vector<std::string>{"people_2024", "people_2024 (2)", "people_2024 (3)"}));
    auto ws = doc.workbook().worksheet("people_2024");
    const auto cell = [&](const char* ref) { return OpenXLSX::XLCellValue(ws.cell(ref).value()); };
    EXPECT_EQ(cell("A1").get<std::string>(), "id");
    EXPECT_EQ(cell("A2").type(), XLValueType::Integer);
    EXPECT_EQ(cell("A2").get<int64_t>(), 1);
    EXPECT_EQ(cell("B2").get<std::string>(), "00123");                // text column stays text
    EXPECT_EQ(cell("C2").get<std::string>(), "12345678901234567890"); // would round as a number
    EXPECT_EQ(cell("D2").type(), XLValueType::Float);
    EXPECT_DOUBLE_EQ(cell("D2").get<double>(), 19.9);
    EXPECT_DOUBLE_EQ(cell("E2").get<double>(), 33010.0); // 1990-05-17
    EXPECT_EQ(cell("E3").get<std::string>(), "not a date");
    EXPECT_EQ(cell("F2").type(), XLValueType::Boolean);
    EXPECT_TRUE(cell("F2").get<bool>());
    EXPECT_FALSE(cell("F3").get<bool>());
    EXPECT_EQ(cell("G2").type(), XLValueType::Empty); // NULL
    EXPECT_EQ(cell("G3").get<std::string>(), "bell removed");
    EXPECT_EQ(ws.rowCount(), 4u); // header + 3

    auto rest = doc.workbook().worksheet("people_2024 (2)");
    EXPECT_EQ(rest.cell("A1").value().get<std::string>(), "id");
    EXPECT_EQ(rest.cell("A2").value().get<int64_t>(), 4);
    EXPECT_EQ(rest.cell("G2").value().get<std::string>().size(), xlsx::kMaxCellChars);
    doc.close();
    std::filesystem::remove(path);
}

TEST(Xlsx, TooManyColumnsIsAnError) {
    const auto path = tmpPath("dearsql_xlsx_wide.xlsx");
    xlsx::Writer w(path);
    w.beginSheet("wide", std::vector<std::string>(16385, "c"));
    std::string error;
    EXPECT_FALSE(w.save(error));
    EXPECT_NE(error.find("16,384"), std::string::npos) << error;
    EXPECT_FALSE(std::filesystem::exists(path));
}

// text a database hands back that excel would call corrupt
TEST(Xlsx, BadUtf8BecomesReplacementCharacters) {
    const auto path = tmpPath("dearsql_xlsx_utf8.xlsx");
    {
        xlsx::Writer w(path);
        w.beginSheet("t", {"s"}, {"text"});
        w.addRow({"caf\xE9"});                      // latin1
        w.addRow({"ok \xC3\xA9 \xF0\x9F\x98\x80"}); // valid: é and an emoji
        w.addRow({"\xEF\xBF\xBE end"});             // U+FFFE
        std::string error;
        ASSERT_TRUE(w.save(error)) << error;
    }
    OpenXLSX::XLDocument doc;
    doc.open(path);
    auto ws = doc.workbook().worksheet("t");
    const auto text = [&](const char* ref) {
        return OpenXLSX::XLCellValue(ws.cell(ref).value()).get<std::string>();
    };
    EXPECT_EQ(text("A2"), "caf\xEF\xBF\xBD");
    EXPECT_EQ(text("A3"), "ok \xC3\xA9 \xF0\x9F\x98\x80");
    EXPECT_EQ(text("A4"), "\xEF\xBF\xBD end");
    doc.close();
    std::filesystem::remove(path);
}

// every value unique: a writer that looks strings up linearly takes minutes here
TEST(Xlsx, ManyUniqueStringsStayFast) {
    const auto path = tmpPath("dearsql_xlsx_big.xlsx");
    const auto t0 = std::chrono::steady_clock::now();
    {
        xlsx::Writer w(path);
        w.beginSheet("big", {"id", "name"}, {"integer", "text"});
        for (int i = 0; i < 200000; ++i)
            w.addRow({std::to_string(i), "name " + std::to_string(i)});
        std::string error;
        ASSERT_TRUE(w.save(error)) << error;
    }
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(secs, 10.0);
    std::printf("200k rows: %.2f s\n", secs);
    std::filesystem::remove(path);
}

// the limit is excel's characters (utf-16 units), not utf-8 bytes
TEST(Xlsx, LongTextIsCutAtCharactersNotBytes) {
    const auto path = tmpPath("dearsql_xlsx_cjk.xlsx");
    std::string fits, tooLong;
    for (int i = 0; i < 20000; ++i)
        fits += "\xE4\xB8\xAD"; // 中, 3 bytes
    for (int i = 0; i < 40000; ++i)
        tooLong += "\xE4\xB8\xAD";
    {
        xlsx::Writer w(path);
        w.beginSheet("t", {"s"}, {"text"});
        w.addRow({fits});
        w.addRow({tooLong});
        std::string error;
        ASSERT_TRUE(w.save(error)) << error;
        EXPECT_EQ(w.truncatedCells(), 1u);
    }
    OpenXLSX::XLDocument doc;
    doc.open(path);
    auto ws = doc.workbook().worksheet("t");
    EXPECT_EQ(ws.cell("A2").value().get<std::string>(), fits);
    EXPECT_EQ(ws.cell("A3").value().get<std::string>().size(), 3 * xlsx::kMaxCellChars);
    doc.close();
    std::filesystem::remove(path);
}

namespace {
    // openxlsx has no autofilter getter, so read the sheet xml from the zip
    std::string sheetXml(const std::string& path, int n) {
        std::string out;
        if (FILE* p = popen(std::format("unzip -p '{}' xl/worksheets/sheet{}.xml", path, n).c_str(),
                            "r")) {
            char buf[4096];
            for (size_t r; (r = fread(buf, 1, sizeof buf, p)) > 0;)
                out.append(buf, r);
            pclose(p);
        }
        return out;
    }
} // namespace

// each sheet's autofilter spans its own columns, not the next sheet's
TEST(Xlsx, AutofilterCoversEachSheetsColumns) {
    const auto path = tmpPath("dearsql_xlsx_filter.xlsx");
    {
        xlsx::Writer w(path);
        w.beginSheet("wide", {"a", "b", "c"});
        w.addRow({"1", "2", "3"});
        w.beginSheet("narrow", {"x"});
        w.addRow({"1"});
        std::string error;
        ASSERT_TRUE(w.save(error)) << error;
    }
    const auto first = sheetXml(path, 1);
    if (first.empty())
        GTEST_SKIP() << "no unzip";
    EXPECT_NE(first.find("<autoFilter ref=\"A1:C2\""), std::string::npos);
    EXPECT_NE(sheetXml(path, 2).find("<autoFilter ref=\"A1:A2\""), std::string::npos);
    std::filesystem::remove(path);
}

namespace {
    std::vector<std::vector<std::string>> rows(dearsql::IDatabase& db, const std::string& sql) {
        auto r = db.execute(sql, 0);
        EXPECT_TRUE(r.success()) << sql << ": " << r.errorMessage();
        return r.success() && !r.statements.empty() ? r.statements[0].tableData
                                                    : std::vector<std::vector<std::string>>{};
    }
} // namespace

// a workbook opens like a database: a table per sheet, typed columns, read-only
TEST(Xlsx, WorkbookOpensAsTables) {
    const auto path = tmpPath("dearsql_open_test.xlsx");
    {
        xlsx::Writer w(path);
        w.beginSheet("orders", {"id", "customer", "total", "placed", "paid"},
                     {"integer", "text", "", "date", "boolean"});
        w.addRow({"1", "ada", "19.9", "2024-01-02", std::string(BOOL_TRUE_SENTINEL)});
        w.addRow({"2", "grace", "5", "2024-02-03", std::string(BOOL_FALSE_SENTINEL)});
        w.addRow({"3", "ada", std::string(NULL_SENTINEL), "2024-03-04 10:30:00",
                  std::string(NULL_SENTINEL)});
        w.beginSheet("customers", {"name", "city"});
        w.addRow({"ada", "london"});
        w.addRow({"grace", "new york"});
        std::string error;
        ASSERT_TRUE(w.save(error)) << error;
    }
    // a sheet written the other common way (shared strings), with no header row
    {
        OpenXLSX::XLDocument doc;
        doc.open(path);
        doc.workbook().addWorksheet("raw");
        auto ws = doc.workbook().worksheet("raw");
        ws.cell("A1").value() = 10;
        ws.cell("B1").value() = "x";
        ws.cell("A2").value() = 20;
        ws.cell("C2").value() = "z"; // B2 missing
        doc.save();
        doc.close();
    }

    dearsql::ConnectionInfo info;
    info.type = dearsql::DatabaseType::SQLITE;
    info.path = path;
    info.name = "book";
    auto conn = dearsql::makeConnection(info);
    ASSERT_TRUE(conn);
    auto [ok, err] = conn->open();
    ASSERT_TRUE(ok) << err;
    auto db = conn->database();
    ASSERT_TRUE(db);

    std::vector<std::string> tables;
    for (const auto& t : db->tables())
        tables.push_back(t.name);
    std::ranges::sort(tables);
    EXPECT_EQ(tables, (std::vector<std::string>{"customers", "orders", "raw"}));

    const auto types = [&](const char* table) {
        std::vector<std::string> out;
        for (const auto& c : db->describeTable(table).columns)
            out.push_back(c.name + " " + c.type);
        return out;
    };
    EXPECT_EQ(types("orders"), (std::vector<std::string>{"id INTEGER", "customer TEXT",
                                                         "total REAL", "placed DATETIME",
                                                         "paid BOOLEAN"}));
    EXPECT_EQ(types("raw"), (std::vector<std::string>{"A INTEGER", "B TEXT", "C TEXT"}));

    // sql across sheets
    const auto joined = rows(*db, "SELECT o.id, c.city, o.placed FROM orders o JOIN customers c "
                                  "ON c.name = o.customer ORDER BY o.id");
    ASSERT_EQ(joined.size(), 3u);
    EXPECT_EQ(joined[0], (std::vector<std::string>{"1", "london", "2024-01-02"}));
    EXPECT_EQ(joined[2][2], "2024-03-04 10:30:00");
    EXPECT_EQ(rows(*db, "SELECT SUM(total) FROM orders")[0][0], "24.9");
    EXPECT_EQ(rows(*db, "SELECT COUNT(*) FROM raw WHERE B IS NULL")[0][0], "1");

    // workers get their own handles on the same data, and nothing can write
    auto worker = conn->openDatabase("");
    ASSERT_TRUE(worker);
    EXPECT_EQ(rows(*worker, "SELECT COUNT(*) FROM orders")[0][0], "3");
    EXPECT_FALSE(worker->execute("DELETE FROM orders", 0).success());
    EXPECT_FALSE(db->execute("DROP TABLE orders", 0).success());

    conn->close();
    std::filesystem::remove(path);
}

TEST(Xlsx, UnreadableWorkbookFailsToOpen) {
    const auto path = tmpPath("dearsql_not_a_workbook.xlsx");
    {
        std::ofstream(path) << "not a zip";
    }
    dearsql::ConnectionInfo info;
    info.type = dearsql::DatabaseType::SQLITE;
    info.path = path;
    auto conn = dearsql::makeConnection(info);
    auto [ok, err] = conn->open();
    EXPECT_FALSE(ok);
    EXPECT_NE(err.find("workbook"), std::string::npos) << err;
    std::filesystem::remove(path);
}

// a report-style sheet: a title above the header, an empty margin column, an error value in a
// number column and a formula column. agents see the notes through describe_table
TEST(Xlsx, ReportSheetFindsItsHeader) {
    const auto path = tmpPath("dearsql_report_test.xlsx");
    std::filesystem::remove(path);
    {
        OpenXLSX::XLDocument doc;
        doc.create(path, OpenXLSX::XLForceOverwrite);
        auto ws = doc.workbook().worksheet("Sheet1");
        ws.setName("Sales Q3");
        ws.cell("B1").value() = "Q3 Sales Report";
        const char* header[] = {"Region", "Rep", "Units", "Price", "Total"};
        for (int c = 0; c < 5; ++c)
            ws.cell(3, c + 2).value() = header[c];
        const char* region[] = {"North", "South", "East", "West"};
        const int units[] = {10, 4, 7, 2};
        const double price[] = {12.5, 99, 0, 250};
        for (int r = 0; r < 4; ++r) {
            const uint32_t row = r + 4;
            ws.cell(row, 2).value() = region[r];
            ws.cell(row, 3).value() = std::string("rep") + std::to_string(r);
            ws.cell(row, 4).value() = units[r];
            if (r == 2)
                ws.cell(row, 5).value() = OpenXLSX::XLCellValue().setError("#N/A");
            else
                ws.cell(row, 5).value() = price[r];
            ws.cell(row, 6).formula() = std::format("D{0}*E{0}", row);
        }
        doc.save();
        doc.close();
    }

    dearsql::ConnectionInfo info;
    info.type = dearsql::DatabaseType::SQLITE;
    info.path = path;
    auto conn = dearsql::makeConnection(info);
    ASSERT_TRUE(conn);
    auto [ok, err] = conn->open();
    ASSERT_TRUE(ok) << err;
    auto db = conn->database();
    const auto t = db->describeTable("Sales Q3");
    std::vector<std::string> cols;
    for (const auto& c : t.columns)
        cols.push_back(c.name + " " + c.type);
    // the title row and the empty column A are left out; the #N/A leaves Price a number
    // (Total's type is whatever value the writer cached for its formula)
    ASSERT_EQ(cols.size(), 5u);
    cols.pop_back();
    EXPECT_EQ(cols, (std::vector<std::string>{"Region TEXT", "Rep TEXT", "Units INTEGER", "Price REAL"}));
    EXPECT_EQ(t.columns[4].name, "Total");
    EXPECT_NE(t.comment.find("header row 3"), std::string::npos) << t.comment;
    EXPECT_NE(t.comment.find("data rows 4-7"), std::string::npos) << t.comment;
    EXPECT_NE(t.comment.find("columns B-F"), std::string::npos) << t.comment;
    EXPECT_NE(t.comment.find("Q3 Sales Report"), std::string::npos) << t.comment;
    EXPECT_EQ(t.columns[0].comment, "column B");
    EXPECT_NE(t.columns[3].comment.find("1 error cell"), std::string::npos) << t.columns[3].comment;
    EXPECT_NE(t.columns[4].comment.find("=D4*E4 in F4"), std::string::npos) << t.columns[4].comment;

    EXPECT_EQ(rows(*db, "SELECT COUNT(*) FROM \"Sales Q3\"")[0][0], "4");
    EXPECT_EQ(rows(*db, "SELECT Region FROM \"Sales Q3\" WHERE Price > 50 ORDER BY Price")[0][0],
              "South");
    EXPECT_EQ(rows(*db, "SELECT COUNT(*) FROM \"Sales Q3\" WHERE Price IS NULL")[0][0], "1");
    conn->close();
    std::filesystem::remove(path);
}

// a plain sqlite table with no notes keeps empty comments
TEST(Xlsx, PlainSqliteTablesHaveNoNotes) {
    dearsql::ConnectionInfo info;
    info.type = dearsql::DatabaseType::SQLITE;
    info.path = ":memory:";
    auto conn = dearsql::makeConnection(info);
    ASSERT_TRUE(conn->open().first);
    auto db = conn->database();
    ASSERT_TRUE(db->execute("CREATE TABLE t (a INTEGER /* not ours */, b TEXT)", 0).success());
    const auto t = db->describeTable("t");
    EXPECT_EQ(t.comment, "");
    EXPECT_EQ(t.columns[1].comment, "");
}

TEST(Xlsx, SaveWithoutReplaceKeepsAFileMadeMeanwhile) {
    const auto path = (std::filesystem::temp_directory_path() / "dearsql_noreplace.xlsx").string();
    std::filesystem::remove(path);
    xlsx::Writer writer(path);
    writer.beginSheet("Sheet1", {"a"});
    writer.addRow({"1"});
    { std::ofstream(path) << "someone else's"; } // appears while the writer works
    std::string error;
    EXPECT_FALSE(writer.save(error, false));
    EXPECT_NE(error.find("already exists"), std::string::npos) << error;
    std::ifstream in(path);
    std::string kept((std::istreambuf_iterator<char>(in)), {});
    EXPECT_EQ(kept, "someone else's");
    EXPECT_FALSE(std::filesystem::exists(path + ".part"));
    std::filesystem::remove(path);
}
