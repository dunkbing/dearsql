#include "database/file_database.hpp"
#include "utils/table_exporter.hpp"
#include "utils/table_importer.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <thread>

namespace {
    class TableTransferTest : public ::testing::Test {
    protected:
        void SetUp() override {
            dir_ = std::filesystem::temp_directory_path() /
                   std::format("dearsql_transfer_{}",
                               std::chrono::steady_clock::now().time_since_epoch().count());
            std::filesystem::create_directories(dir_);
            DatabaseConnectionInfo info;
            info.type = DatabaseType::SQLITE;
            info.path = (dir_ / "t.db").string();
            db_ = std::make_unique<FileDatabase>(info);
            ASSERT_TRUE(db_->connect().first);
            ASSERT_TRUE(
                db_->executeQuery("CREATE TABLE items (id INTEGER NOT NULL, name TEXT)").success());
        }
        void TearDown() override {
            db_.reset();
            std::filesystem::remove_all(dir_);
        }

        std::string writeCsv(int rows, int badRow = -1) {
            const auto path = (dir_ / "in.csv").string();
            std::ofstream out(path);
            out << "id,name\n";
            for (int i = 0; i < rows; ++i) {
                // an empty id is NULL, which NOT NULL rejects
                out << (i == badRow ? "" : std::to_string(i)) << ",\"name, " << i << "\"\n";
            }
            return path;
        }

        long long count() {
            auto r = db_->executeQuery("SELECT COUNT(*) FROM items");
            return r.success() && !r.empty() ? std::stoll(r[0].tableData[0][0]) : -1;
        }

        std::filesystem::path dir_;
        std::unique_ptr<FileDatabase> db_;
    };
} // namespace

TEST_F(TableTransferTest, CsvImportLandsEveryRow) {
    const auto path = writeCsv(1234);
    TableImporter::Progress progress;
    const auto result = TableImporter::importCsv(db_.get(), "items", path, progress);
    EXPECT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.inserted, 1234);
    EXPECT_EQ(count(), 1234);
    EXPECT_EQ(progress.bytesRead.load(), progress.totalBytes.load());
}

// a failing batch is retried row by row: only the bad row is lost
TEST_F(TableTransferTest, CsvImportBadRowCostsOnlyItself) {
    const auto path = writeCsv(1000, 700);
    TableImporter::Progress progress;
    const auto result = TableImporter::importCsv(db_.get(), "items", path, progress);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.inserted, 999);
    EXPECT_EQ(result.failed, 1);
    EXPECT_FALSE(result.error.empty());
    EXPECT_EQ(count(), 999);
}

TEST_F(TableTransferTest, CsvImportCancels) {
    const auto path = writeCsv(20000);
    TableImporter::Progress progress;
    TableImporter::Result result;
    std::thread worker(
        [&] { result = TableImporter::importCsv(db_.get(), "items", path, progress); });
    while (progress.inserted.load() < 1000 && !progress.cancelRequested.load())
        std::this_thread::yield();
    progress.cancelRequested = true;
    worker.join();
    EXPECT_TRUE(result.cancelled);
    EXPECT_FALSE(result.success);
    EXPECT_LT(result.inserted, 20000);
    EXPECT_EQ(count(), result.inserted);
}

// pages past one export batch (10000 rows) and writes every row once
TEST_F(TableTransferTest, CsvExportWritesEveryRow) {
    TableImporter::Progress in;
    ASSERT_TRUE(TableImporter::importCsv(db_.get(), "items", writeCsv(25000), in).success);

    TableExporter::Request request;
    request.format = ExportFormat::CSV;
    request.tables.push_back(Table{.name = "items"});
    request.path = (dir_ / "out.csv").string();
    TableExporter::Progress progress;
    const auto result = TableExporter::run(db_.get(), request, progress);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.rows, 25000);

    std::ifstream file(request.path);
    std::string line;
    int lines = 0;
    while (std::getline(file, line))
        ++lines;
    EXPECT_EQ(lines, 25001); // header + rows
}

TEST_F(TableTransferTest, ExportCancelledBeforeStartWritesNothing) {
    TableExporter::Request request;
    request.format = ExportFormat::JSON;
    request.tables.push_back(Table{.name = "items"});
    request.path = (dir_ / "out.json").string();
    TableExporter::Progress progress;
    progress.cancelRequested = true;
    const auto result = TableExporter::run(db_.get(), request, progress);
    EXPECT_TRUE(result.cancelled);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.rows, 0);
}

// structure only: every table's CREATE TABLE and indexes in one file, no rows
TEST_F(TableTransferTest, DdlExportWritesStructureOnly) {
    ASSERT_TRUE(db_->executeQuery("CREATE INDEX items_name ON items (name);"
                                  "CREATE TABLE tags (id INTEGER PRIMARY KEY);"
                                  "INSERT INTO items VALUES (1, 'a')")
                    .success());
    TableExporter::Request request;
    request.format = ExportFormat::DDL;
    request.tables = {Table{.name = "items"}, Table{.name = "tags"}};
    request.path = (dir_ / "schema.sql").string();
    TableExporter::Progress progress;
    const auto result = TableExporter::run(db_.get(), request, progress);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.tables, 2);
    EXPECT_EQ(result.rows, 0);

    std::ifstream file(request.path);
    const std::string sql{std::istreambuf_iterator<char>(file), {}};
    EXPECT_NE(sql.find("CREATE TABLE items"), std::string::npos) << sql;
    EXPECT_NE(sql.find("CREATE INDEX items_name"), std::string::npos) << sql;
    EXPECT_NE(sql.find("CREATE TABLE tags"), std::string::npos) << sql;
    EXPECT_EQ(sql.find("INSERT"), std::string::npos) << sql;

    EXPECT_FALSE(db_->getTableDdl(Table{.name = "missing"}).first);
}

TEST_F(TableTransferTest, DdlExportPutsReferencedTablesFirst) {
    ASSERT_TRUE(
        db_->executeQuery("CREATE TABLE child (id INTEGER, p INTEGER REFERENCES parent(id));"
                          "CREATE TABLE parent (id INTEGER PRIMARY KEY)")
            .success());
    TableExporter::Request request;
    request.format = ExportFormat::DDL;
    request.tables = {Table{.name = "child", .foreignKeys = {ForeignKey{.targetTable = "parent"}}},
                      Table{.name = "parent"}};
    request.path = (dir_ / "fk.sql").string();
    TableExporter::Progress progress;
    ASSERT_TRUE(TableExporter::run(db_.get(), request, progress).success);

    std::ifstream file(request.path);
    const std::string sql{std::istreambuf_iterator<char>(file), {}};
    EXPECT_LT(sql.find("CREATE TABLE parent"), sql.find("CREATE TABLE child")) << sql;
}
