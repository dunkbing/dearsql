#pragma once

#include <functional>
#include <iosfwd>
#include <string>

namespace SqlDumpSplitter {

    // Splits a MySQL dump into individually executable statements.
    //
    // Handles the client-side DELIMITER directive, single/double quoted strings,
    // backtick identifiers, and -- # and C-style comments. MySQL conditional
    // comments (/*!...*/) are kept in the statement text and their contents are
    // never treated as statement boundaries, because the server executes them.
    //
    // onStatement receives each statement with the trailing delimiter removed and
    // surrounding whitespace trimmed; empty statements are skipped. Returning
    // false from it stops the scan. Returns false if the scan was stopped that
    // way, true if the whole stream was consumed.
    //
    // compound is true when the statement was terminated by a delimiter other
    // than ';', i.e. it came from a DELIMITER block. Those are the trigger and
    // routine bodies, which contain internal semicolons and so must be sent to
    // the server on their own rather than concatenated with other statements.
    bool split(std::istream& in,
               const std::function<bool(const std::string& statement, bool compound)>& onStatement);

} // namespace SqlDumpSplitter
