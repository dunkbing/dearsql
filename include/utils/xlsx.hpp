#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// .xlsx files (desktop only): written with libxlsxwriter, read with OpenXLSX. cells come and go in the grid's
// encoding: strings, with the NULL and boolean sentinels
namespace xlsx {
    constexpr size_t kMaxRows = 1048576;     // per sheet, header included
    constexpr size_t kMaxCellChars = 32767;  // longer text makes excel "repair" the file

    // writes a new workbook (libxlsxwriter, constant-memory mode: rows stream through temp
    // files, so any size fits). rows go strictly in order. nothing reaches `path` until
    // save() succeeds
    class Writer {
    public:
        // rowsPerSheet: data rows before a sheet continues on "<name> (2)"
        explicit Writer(std::string path, size_t rowsPerSheet = kMaxRows - 1);
        ~Writer();
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;

        // columnTypes may be empty or short (a query result): those cells are typed by their text
        void beginSheet(const std::string& name, const std::vector<std::string>& columns,
                        const std::vector<std::string>& columnTypes = {});
        void addRow(const std::vector<std::string>& row);
        // false with error set; the first failure of any call above lands here
        bool save(std::string& error);

        size_t sheets() const;
        size_t truncatedCells() const; // text cut at kMaxCellChars

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // pure helpers, exposed for tests
    // a valid sheet name: no []:*?/\, 31 characters at most, unique ignoring case
    std::string sheetName(const std::string& wanted, const std::vector<std::string>& taken);
    // a number excel keeps exactly: no leading zeros, 15 significant digits at most
    std::optional<double> plainNumber(const std::string& v);
    // yyyy-mm-dd, optionally with " hh:mm:ss[.fff]" or "T...", as an excel date serial
    std::optional<double> dateSerial(const std::string& v);
} // namespace xlsx
