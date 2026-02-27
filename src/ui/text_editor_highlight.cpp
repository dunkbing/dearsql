#include "ui/text_editor.hpp"
#include "utils/logger.hpp"
#include <cstring>
#include <format>
#include <tree_sitter/api.h>

extern "C" const TSLanguage* tree_sitter_sql();

namespace dearsql {

    // Embedded SQL highlight query - maps AST node types to color categories
    // Based on tree-sitter-sql grammar from https://github.com/DerekStride/tree-sitter-sql
    static constexpr const char* SQL_HIGHLIGHT_QUERY = R"scm(
; SQL Keywords
[
  (keyword_select)
  (keyword_from)
  (keyword_where)
  (keyword_and)
  (keyword_or)
  (keyword_not)
  (keyword_in)
  (keyword_is)
  (keyword_null)
  (keyword_as)
  (keyword_on)
  (keyword_join)
  (keyword_left)
  (keyword_right)
  (keyword_inner)
  (keyword_outer)
  (keyword_cross)
  (keyword_full)
  (keyword_natural)
  (keyword_group)
  (keyword_by)
  (keyword_order)
  (keyword_asc)
  (keyword_desc)
  (keyword_having)
  (keyword_limit)
  (keyword_offset)
  (keyword_union)
  (keyword_all)
  (keyword_distinct)
  (keyword_between)
  (keyword_like)
  (keyword_exists)
  (keyword_case)
  (keyword_when)
  (keyword_then)
  (keyword_else)
  (keyword_end)
  (keyword_begin)
  (keyword_commit)
  (keyword_rollback)
  (keyword_with)
  (keyword_recursive)
  (keyword_returning)
  (keyword_conflict)
  (keyword_cascade)
  (keyword_restrict)
  (keyword_references)
  (keyword_primary)
  (keyword_key)
  (keyword_foreign)
  (keyword_unique)
  (keyword_check)
  (keyword_default)
  (keyword_constraint)
  (keyword_if)
  (keyword_replace)
  (keyword_temporary)
  (keyword_temp)
  (keyword_explain)
  (keyword_analyze)
  (keyword_vacuum)
  (keyword_trigger)
  (keyword_procedure)
  (keyword_function)
  (keyword_returns)
  (keyword_declare)
  (keyword_into)
  (keyword_values)
  (keyword_set)
  (keyword_insert)
  (keyword_update)
  (keyword_delete)
  (keyword_create)
  (keyword_drop)
  (keyword_alter)
  (keyword_table)
  (keyword_index)
  (keyword_view)
  (keyword_database)
  (keyword_schema)
  (keyword_do)
  (keyword_nothing)
  (keyword_duplicate)
  (keyword_for)
  (keyword_each)
  (keyword_row)
  (keyword_over)
  (keyword_partition)
  (keyword_window)
  (keyword_filter)
  (keyword_except)
  (keyword_intersect)
  (keyword_true)
  (keyword_false)
  (keyword_use)
  (keyword_show)
  (keyword_tables)
  (keyword_columns)
  (keyword_type)
  (keyword_column)
  (keyword_add)
  (keyword_rename)
  (keyword_to)
  (keyword_only)
  (keyword_using)
  (keyword_materialized)
  (keyword_concurrently)
  (keyword_refresh)
  (keyword_extension)
  (keyword_sequence)
  (keyword_role)
  (keyword_policy)
  (keyword_merge)
  (keyword_matched)
  (keyword_truncate)
  (keyword_copy)
  (keyword_execute)
  (keyword_return)
  (keyword_language)
  (keyword_transaction)
  (keyword_session)
  (keyword_isolation)
  (keyword_level)
  (keyword_read)
  (keyword_write)
  (keyword_serializable)
  (keyword_committed)
  (keyword_uncommitted)
  (keyword_repeatable)
  (keyword_snapshot)
  (keyword_some)
  (keyword_any)
  (keyword_similar)
  (keyword_match)
  (keyword_lateral)
  (keyword_no)
  (keyword_action)
  (keyword_option)
  (keyword_current)
  (keyword_collate)
  (keyword_comment)
  (keyword_owner)
  (keyword_owned)
  (keyword_enable)
  (keyword_disable)
  (keyword_start)
  (keyword_restart)
  (keyword_cache)
  (keyword_cycle)
  (keyword_increment)
  (keyword_minvalue)
  (keyword_maxvalue)
  (keyword_generated)
  (keyword_always)
  (keyword_stored)
  (keyword_virtual)
  (keyword_deferred)
  (keyword_immediate)
  (keyword_deferrable)
  (keyword_initially)
  (keyword_encoding)
  (keyword_tablespace)
  (keyword_storage)
  (keyword_include)
  (keyword_replication)
  (keyword_connection)
  (keyword_password)
  (keyword_valid)
  (keyword_until)
  (keyword_admin)
  (keyword_input)
  (keyword_new)
  (keyword_old)
  (keyword_referencing)
  (keyword_instead)
  (keyword_before)
  (keyword_after)
  (keyword_each)
  (keyword_parallel)
  (keyword_safe)
  (keyword_unsafe)
  (keyword_restricted)
  (keyword_cost)
  (keyword_rows)
  (keyword_volatile)
  (keyword_stable)
  (keyword_immutable)
  (keyword_strict)
  (keyword_called)
  (keyword_security)
  (keyword_definer)
  (keyword_invoker)
  (keyword_leakproof)
  (keyword_support)
  (keyword_setof)
  (keyword_variadic)
  (keyword_inout)
  (keyword_out)
  (keyword_atomic)
  (keyword_while)
  (keyword_statistics)
  (keyword_public)
  (keyword_logged)
  (keyword_unlogged)
  (keyword_ignore)
] @keyword

; Data types
[
  (keyword_int)
  (keyword_bigint)
  (keyword_smallint)
  (keyword_tinyint)
  (keyword_mediumint)
  (keyword_float)
  (keyword_double)
  (keyword_decimal)
  (keyword_numeric)
  (keyword_real)
  (keyword_precision)
  (keyword_char)
  (keyword_character)
  (keyword_varchar)
  (keyword_nchar)
  (keyword_nvarchar)
  (keyword_text)
  (keyword_binary)
  (keyword_varbinary)
  (keyword_boolean)
  (keyword_date)
  (keyword_datetime)
  (keyword_datetime2)
  (keyword_time)
  (keyword_timestamp)
  (keyword_timestamptz)
  (keyword_interval)
  (keyword_serial)
  (keyword_bigserial)
  (keyword_smallserial)
  (keyword_money)
  (keyword_smallmoney)
  (keyword_smalldatetime)
  (keyword_datetimeoffset)
  (keyword_json)
  (keyword_jsonb)
  (keyword_xml)
  (keyword_uuid)
  (keyword_bytea)
  (keyword_inet)
  (keyword_bit)
  (keyword_enum)
  (keyword_image)
  (keyword_regclass)
  (keyword_regtype)
  (keyword_regproc)
  (keyword_regnamespace)
  (keyword_geometry)
  (keyword_geography)
  (keyword_box2d)
  (keyword_box3d)
  (keyword_varying)
  (keyword_unsigned)
  (keyword_zerofill)
  (keyword_auto_increment)
  (keyword_oid)
  (keyword_name)
  (keyword_string)
  (keyword_object_id)
] @type

; Comments
(comment) @comment
(marginalia) @comment

; Strings
(literal) @string

; Function calls
(invocation
  unit: (object_reference
    name: (identifier) @function))

; Numbers (handled via literal, but we can be more specific)
; The grammar uses (literal) for both strings and numbers

; Star/wildcard
(all_fields) @operator
)scm";

    void TextEditor::initTreeSitter() {
        tsParser_ = ts_parser_new();
        const TSLanguage* lang = tree_sitter_sql();
        ts_parser_set_language(tsParser_, lang);

        // Compile the highlight query
        uint32_t errorOffset;
        TSQueryError errorType;
        tsQuery_ = ts_query_new(lang, SQL_HIGHLIGHT_QUERY, strlen(SQL_HIGHLIGHT_QUERY),
                                &errorOffset, &errorType);
        if (!tsQuery_) {
            Logger::error(
                std::format("Tree-sitter SQL query compilation failed at offset {}, error type {}",
                            errorOffset, static_cast<int>(errorType)));
        }
    }

    void TextEditor::cleanupTreeSitter() {
        if (tsQuery_) {
            ts_query_delete(tsQuery_);
            tsQuery_ = nullptr;
        }
        if (tsTree_) {
            ts_tree_delete(tsTree_);
            tsTree_ = nullptr;
        }
        if (tsParser_) {
            ts_parser_delete(tsParser_);
            tsParser_ = nullptr;
        }
    }

    void TextEditor::rehighlight() {
        if (!tsParser_ || !tsQuery_ || content_.empty()) {
            // Set all to default text color
            colors_.assign(content_.size(), palette_.text);
            return;
        }

        // Incremental parsing
        bool initialParse = tsPreviousContent_.empty();

        if (!initialParse && tsTree_) {
            // Compute edit range
            size_t start = 0;
            size_t oldEnd = tsPreviousContent_.size();
            size_t newEnd = content_.size();

            while (start < oldEnd && start < newEnd && tsPreviousContent_[start] == content_[start])
                ++start;

            while (oldEnd > start && newEnd > start &&
                   tsPreviousContent_[oldEnd - 1] == content_[newEnd - 1]) {
                --oldEnd;
                --newEnd;
            }

            TSInputEdit edit;
            edit.start_byte = static_cast<uint32_t>(start);
            edit.old_end_byte = static_cast<uint32_t>(oldEnd);
            edit.new_end_byte = static_cast<uint32_t>(newEnd);
            edit.start_point = {0, static_cast<uint32_t>(start)};
            edit.old_end_point = {0, static_cast<uint32_t>(oldEnd)};
            edit.new_end_point = {0, static_cast<uint32_t>(newEnd)};
            ts_tree_edit(tsTree_, &edit);
        }

        // Parse
        TSTree* newTree =
            ts_parser_parse_string(tsParser_, initialParse ? nullptr : tsTree_, content_.c_str(),
                                   static_cast<uint32_t>(content_.size()));

        if (tsTree_)
            ts_tree_delete(tsTree_);
        tsTree_ = newTree;
        tsPreviousContent_ = content_;

        if (!newTree) {
            colors_.assign(content_.size(), palette_.text);
            return;
        }

        // Fill all colors with default text color first
        colors_.assign(content_.size(), palette_.text);

        // Execute highlight query
        TSQueryCursor* cursor = ts_query_cursor_new();
        ts_query_cursor_exec(cursor, tsQuery_, ts_tree_root_node(newTree));

        TSQueryMatch match;
        while (ts_query_cursor_next_match(cursor, &match)) {
            for (uint32_t i = 0; i < match.capture_count; ++i) {
                TSNode node = match.captures[i].node;
                uint32_t nameLen;
                const char* name =
                    ts_query_capture_name_for_id(tsQuery_, match.captures[i].index, &nameLen);

                uint32_t startByte = ts_node_start_byte(node);
                uint32_t endByte = ts_node_end_byte(node);

                // Map capture name to palette color
                ImU32 color = palette_.text;
                if (strncmp(name, "keyword", nameLen) == 0)
                    color = palette_.keyword;
                else if (strncmp(name, "string", nameLen) == 0)
                    color = palette_.string;
                else if (strncmp(name, "number", nameLen) == 0)
                    color = palette_.number;
                else if (strncmp(name, "comment", nameLen) == 0)
                    color = palette_.comment;
                else if (strncmp(name, "function", nameLen) == 0)
                    color = palette_.function;
                else if (strncmp(name, "type", nameLen) == 0)
                    color = palette_.type;
                else if (strncmp(name, "operator", nameLen) == 0)
                    color = palette_.operator_;

                // Apply color to range
                for (uint32_t j = startByte; j < endByte && j < colors_.size(); ++j)
                    colors_[j] = color;
            }
        }

        ts_query_cursor_delete(cursor);
    }

} // namespace dearsql
