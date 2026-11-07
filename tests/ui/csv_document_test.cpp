#include "utils/csv_document.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace {

    struct TempFile {
        std::filesystem::path path;
        explicit TempFile(const std::string& bytes, const std::string& ext = ".csv") {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() /
                   std::format("dearsql_csv_doc_{}{}", stamp, ext);
            std::ofstream(path, std::ios::binary) << bytes;
        }
        ~TempFile() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        [[nodiscard]] std::string str() const {
            return path.string();
        }
    };

    std::string readFile(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    CsvDocument loaded(const TempFile& f) {
        CsvDocument doc;
        std::string error;
        EXPECT_TRUE(doc.load(f.str(), error)) << error;
        return doc;
    }

    // saving an unedited file must give back the same bytes
    void expectRoundTrip(const std::string& bytes) {
        TempFile f(bytes);
        CsvDocument doc = loaded(f);
        std::string error;
        ASSERT_TRUE(doc.save(f.str(), error)) << error;
        EXPECT_EQ(readFile(f.path), bytes);
    }

} // namespace

TEST(CsvDocumentTest, RoundTripsDialects) {
    expectRoundTrip("id,name\n1,ada\n2,\"lovelace, ada\"\n");
    expectRoundTrip("id;name\r\n1;ada\r\n2;\"say \"\"hi\"\"\"\r\n");
    expectRoundTrip("a\tb\n1\t2\n");
    expectRoundTrip("a|b\n1|2");                              // no trailing line break
    expectRoundTrip("\"a\",\"b\"\n\"1\",\"2\"\n");            // every field quoted
    expectRoundTrip("\xEF\xBB\xBFname\n\xC3\xA9t\xC3\xA9\n"); // utf-8 with bom
    expectRoundTrip("text\n\"line one\nline two\"\n");        // newline inside a field
}

TEST(CsvDocumentTest, DetectsDialect) {
    TempFile f("a;b;c\r\n1;2;3\r\n4;5;6\r\n");
    const CsvDocument doc = loaded(f);
    EXPECT_EQ(doc.dialect().delimiter, ";");
    EXPECT_EQ(doc.dialect().lineBreak, "\r\n");
    EXPECT_EQ(doc.columnCount(), 3);
    EXPECT_EQ(doc.rowCount(), 2);
    EXPECT_EQ(doc.headers()[2], "c");
}

TEST(CsvDocumentTest, EditKeepsDialect) {
    TempFile f("id;name\r\n1;ada\r\n");
    CsvDocument doc = loaded(f);
    doc.setCell(0, 1, "grace; hopper");
    EXPECT_TRUE(doc.isModified());
    std::string error;
    ASSERT_TRUE(doc.save(f.str(), error)) << error;
    EXPECT_FALSE(doc.isModified());
    EXPECT_EQ(readFile(f.path), "id;name\r\n1;\"grace; hopper\"\r\n");
}

TEST(CsvDocumentTest, Utf16AndWindows1252RoundTrip) {
    // "é,€" as utf-16 le with bom
    const std::string utf16("\xFF\xFE"
                            "a\0,\0b\0\n\0\xE9\0,\0\xAC\x20\n\0",
                            18);
    {
        TempFile f(utf16);
        CsvDocument doc = loaded(f);
        EXPECT_EQ(doc.dialect().encoding, CsvDialect::Encoding::Utf16LE);
        EXPECT_EQ(doc.rows()[0][0], "\xC3\xA9");
        EXPECT_EQ(doc.rows()[0][1], "\xE2\x82\xAC");
        std::string error;
        ASSERT_TRUE(doc.save(f.str(), error));
        EXPECT_EQ(readFile(f.path), utf16);
    }
    {
        const std::string cp1252 = "a,b\n\xE9,\x80\n";
        TempFile f(cp1252);
        CsvDocument doc = loaded(f);
        EXPECT_EQ(doc.dialect().encoding, CsvDialect::Encoding::Windows1252);
        EXPECT_EQ(doc.rows()[0][1], "\xE2\x82\xAC");
        std::string error;
        ASSERT_TRUE(doc.save(f.str(), error));
        EXPECT_EQ(readFile(f.path), cp1252);
    }
}

TEST(CsvDocumentTest, OpenWithChosenFormat) {
    TempFile f("a,b;c\n1,2;3\n");
    CsvDocument doc;
    std::string error;
    CsvDialect d;
    d.delimiter = ";";
    ASSERT_TRUE(doc.load(f.str(), error, d));
    EXPECT_EQ(doc.columnCount(), 2);
    EXPECT_EQ(doc.headers()[0], "a,b");
}

TEST(CsvDocumentTest, PadsRaggedRows) {
    TempFile f("a,b\n1\n1,2,3\n");
    const CsvDocument doc = loaded(f);
    EXPECT_EQ(doc.columnCount(), 3);
    EXPECT_EQ(doc.raggedRows(), 2);
    EXPECT_EQ(doc.headers()[2], "");
    EXPECT_EQ(doc.rows()[0][1], "");
}

TEST(CsvDocumentTest, EmptyFile) {
    TempFile f("");
    CsvDocument doc = loaded(f);
    EXPECT_EQ(doc.rowCount(), 0);
    EXPECT_EQ(doc.columnCount(), 0);
    doc.insertColumn(0, "x");
    doc.insertRows(0);
    doc.setCell(0, 0, "1");
    EXPECT_EQ(doc.toText(), "x\n1\n");
}

TEST(CsvDocumentTest, UndoRedoTracksSavedState) {
    TempFile f("a,b\n1,2\n3,4\n");
    CsvDocument doc = loaded(f);
    doc.setCell(0, 0, "9");
    doc.deleteRows({1});
    EXPECT_EQ(doc.rowCount(), 1);
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.rowCount(), 2);
    EXPECT_EQ(doc.rows()[0][0], "9");
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.rows()[0][0], "1");
    EXPECT_FALSE(doc.isModified());
    EXPECT_FALSE(doc.undo());
    ASSERT_TRUE(doc.redo());
    EXPECT_EQ(doc.rows()[0][0], "9");
    EXPECT_TRUE(doc.isModified());
    ASSERT_TRUE(doc.redo());
    EXPECT_EQ(doc.rowCount(), 1);
    // a new edit drops the redo history
    doc.undo();
    doc.setCell(0, 1, "x");
    EXPECT_FALSE(doc.canRedo());
}

TEST(CsvDocumentTest, SortIsNumericAwareAndCarriesFlags) {
    TempFile f("n\n10\n9\n\nabc\n100\n");
    CsvDocument doc = loaded(f);
    doc.setFlags({0}, true); // the "10" row
    doc.sortBy(0, true);
    std::vector<std::string> got;
    for (const auto& r : doc.rows())
        got.push_back(r[0]);
    EXPECT_EQ(got, (std::vector<std::string>{"9", "10", "100", "abc", ""}));
    EXPECT_EQ(doc.flaggedRows(), std::vector<int>{1});
    doc.sortBy(0, false);
    EXPECT_EQ(doc.rows()[0][0], "abc");
    EXPECT_EQ(doc.rows()[1][0], "100");
    EXPECT_EQ(doc.rows().back()[0], "");
}

TEST(CsvDocumentTest, FindAndReplace) {
    TempFile f("a,b\nFoo,bar\nbaz,food\n");
    CsvDocument doc = loaded(f);
    CsvDocument::FindOptions find;
    find.text = "foo";
    auto hit = doc.findNext(find, 0, 0, true);
    ASSERT_TRUE(hit);
    EXPECT_EQ(*hit, std::make_pair(1, 1)); // after (0,0), wrapping
    hit = doc.findNext(find, 1, 1, true);
    EXPECT_EQ(*hit, std::make_pair(0, 0));
    find.matchCase = true;
    EXPECT_EQ(*doc.findNext(find, 1, 1, true), std::make_pair(1, 1));
    // a filtered view only searches its rows
    find.matchCase = false;
    EXPECT_EQ(*doc.findNext(find, 1, 0, true, {1}), std::make_pair(1, 1));

    find.matchCase = false;
    EXPECT_EQ(doc.replaceAll(find, "x"), 2);
    EXPECT_EQ(doc.rows()[0][0], "x");
    EXPECT_EQ(doc.rows()[1][1], "xd");

    CsvDocument::FindOptions re;
    re.text = "^b(a)";
    re.regex = true;
    EXPECT_EQ(doc.replaceAll(re, "B$1"), 2);
    EXPECT_EQ(doc.rows()[0][1], "Bar");
    EXPECT_EQ(doc.rows()[1][0], "Baz");
}

TEST(CsvDocumentTest, ColumnTools) {
    TempFile f("name,city\nada lovelace,london\ngrace,ny\n");
    CsvDocument doc = loaded(f);
    EXPECT_EQ(doc.splitColumn(0, " "), 2);
    EXPECT_EQ(doc.headers(), (std::vector<std::string>{"name 1", "name 2", "city"}));
    EXPECT_EQ(doc.rows()[1], (std::vector<std::string>{"grace", "", "ny"}));
    doc.mergeColumns(0, 1, " ");
    EXPECT_EQ(doc.rows()[0][0], "ada lovelace");
    EXPECT_EQ(doc.rows()[1][0], "grace");
    EXPECT_TRUE(doc.moveColumns(1, 1, -1));
    EXPECT_EQ(doc.headers()[0], "city");
    EXPECT_FALSE(doc.moveColumns(0, 0, -1));
    doc.insertColumn(1, "age");
    doc.renameColumn(1, "years");
    EXPECT_EQ(doc.headers()[1], "years");
    doc.deleteColumns(1, 1);
    EXPECT_EQ(doc.columnCount(), 2);
}

TEST(CsvDocumentTest, PasteGrowsTable) {
    TempFile f("a\n1\n");
    CsvDocument doc = loaded(f);
    doc.setRange(0, 0, {{"x", "y"}, {"z", "w"}});
    EXPECT_EQ(doc.columnCount(), 2);
    EXPECT_EQ(doc.rowCount(), 2);
    EXPECT_EQ(doc.rows()[1][1], "w");
    doc.undo();
    EXPECT_EQ(doc.columnCount(), 1);
}

TEST(CsvDocumentTest, SwitchHeaderRow) {
    TempFile f("a,b\n1,2\n");
    CsvDocument doc = loaded(f);
    doc.setHasHeader(false);
    EXPECT_EQ(doc.rowCount(), 2);
    EXPECT_EQ(doc.headers()[0], "Column 1");
    EXPECT_EQ(doc.toText(), "a,b\n1,2\n");
    doc.setHasHeader(true);
    EXPECT_EQ(doc.headers()[0], "a");
    EXPECT_EQ(doc.rowCount(), 1);
}

TEST(CsvDocumentTest, FlagsAndConsistency) {
    TempFile f("n\n1\n2.5\nabc\n\n4\n");
    CsvDocument doc = loaded(f);
    EXPECT_EQ(doc.flagInconsistent(0), 1);
    EXPECT_EQ(doc.flaggedRows(), std::vector<int>{2});
    doc.invertFlags();
    EXPECT_EQ(doc.flaggedCount(), 4);
    doc.deleteRows(doc.flaggedRows());
    EXPECT_EQ(doc.rowCount(), 1);
    EXPECT_EQ(doc.rows()[0][0], "abc");
    EXPECT_FALSE(doc.isFlagged(0));
}

TEST(CsvDocumentTest, ExportRowsJsonAndSplit) {
    TempFile f("id;name\n1;a\n2;b\n3;c\n");
    CsvDocument doc = loaded(f);
    TempFile out("", ".csv");
    std::string error;
    ASSERT_TRUE(doc.saveRows(out.str(), {0, 2}, error)) << error;
    EXPECT_EQ(readFile(out.path), "id;name\n1;a\n3;c\n");

    EXPECT_EQ(doc.toJson({1}), "[\n  {\n    \"id\": \"2\",\n    \"name\": \"b\"\n  }\n]\n");

    const auto files = doc.splitFile(out.str(), 2, error);
    ASSERT_EQ(files.size(), 2u) << error;
    EXPECT_EQ(readFile(files[0]), "id;name\n1;a\n2;b\n");
    EXPECT_EQ(readFile(files[1]), "id;name\n3;c\n");
    for (const auto& p : files)
        std::filesystem::remove(p);
}

TEST(CsvDocumentTest, SetCellsIsOneUndoStep) {
    TempFile f("a,b\n1,2\n3,4\n");
    CsvDocument doc = loaded(f);
    doc.setCells({{0, 0, "x"}, {1, 1, "y"}, {9, 9, "skipped"}});
    EXPECT_EQ(doc.rows()[0][0], "x");
    EXPECT_EQ(doc.rows()[1][1], "y");
    doc.undo();
    EXPECT_EQ(doc.rows()[0][0], "1");
    EXPECT_EQ(doc.rows()[1][1], "4");
    EXPECT_FALSE(doc.isModified());
}

TEST(CsvDocumentTest, MatcherRejectsBadRegex) {
    CsvDocument::FindOptions find;
    find.text = "([";
    find.regex = true;
    const CsvDocument::Matcher m(find);
    EXPECT_FALSE(m.valid());
    EXPECT_FALSE(m("(["));
}

TEST(CsvDocumentTest, FormatChangeIsUndoable) {
    TempFile f("a,b\n1,2\n");
    CsvDocument doc = loaded(f);
    CsvDialect d = doc.dialect();
    d.delimiter = ";";
    d.lineBreak = "\r\n";
    doc.setDialect(d);
    EXPECT_TRUE(doc.isModified());
    EXPECT_EQ(doc.toText(), "a;b\r\n1;2\r\n");
    doc.undo();
    EXPECT_FALSE(doc.isModified());
    EXPECT_EQ(doc.toText(), "a,b\n1,2\n");
}
