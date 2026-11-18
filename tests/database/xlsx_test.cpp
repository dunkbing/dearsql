#include "database/db.hpp"
#include "utils/xlsx.hpp"
#include <OpenXLSX.hpp>
#include <chrono>
#include <filesystem>
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
    EXPECT_DOUBLE_EQ(*xlsx::dateSerial("2024-01-01 12:00:00"), 45292.5);
    EXPECT_DOUBLE_EQ(*xlsx::dateSerial("2024-01-01T06:00:00.5"), 45292.25 + 0.5 / 86400);
    for (const char* text : {"2024-13-01", "2024-01-01 12:00:00+02", "1899-12-31", "01/02/2024",
                             "2024-01-01 1:00:00", "today"})
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
    ASSERT_EQ(names, (std::vector<std::string>{"people_2024", "people_2024 (2)", "people_2024 (3)"}));
    auto ws = doc.workbook().worksheet("people_2024");
    const auto cell = [&](const char* ref) { return OpenXLSX::XLCellValue(ws.cell(ref).value()); };
    EXPECT_EQ(cell("A1").get<std::string>(), "id");
    EXPECT_EQ(cell("A2").type(), XLValueType::Integer);
    EXPECT_EQ(cell("A2").get<int64_t>(), 1);
    EXPECT_EQ(cell("B2").get<std::string>(), "00123");             // text column stays text
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
        w.addRow({"caf\xE9"});               // latin1
        w.addRow({"ok \xC3\xA9 \xF0\x9F\x98\x80"}); // valid: é and an emoji
        w.addRow({"\xEF\xBF\xBE end"});       // U+FFFE
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
    const auto secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(secs, 10.0);
    std::printf("200k rows: %.2f s\n", secs);
    std::filesystem::remove(path);
}
