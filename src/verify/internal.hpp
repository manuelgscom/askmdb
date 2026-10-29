#pragma once
// Internals of the verifier shared by its .cpp files: tables, reasons and text checks.

#include <string>
#include <string_view>

#include "verify/verify.hpp"

namespace askmdb::verify::detail {

// --- tables.cpp --------------------------------------------------------------------------------

enum class StageKind { Read, Warn, Write, Server, Unsupported, Unknown };

StageKind stage_kind(std::string_view name);
bool is_js_operator(std::string_view name);
bool is_query_logical(std::string_view name);   // top level of a query document: $and $or ...
bool is_query_operator(std::string_view name);  // on a field: $eq $in $elemMatch ...
bool is_expression_operator(std::string_view name);
bool is_accumulator(std::string_view name);     // $group, $bucket, $bucketAuto
bool is_window_operator(std::string_view name); // $setWindowFields (accumulators included)
bool is_system_variable(std::string_view name); // without "$$": NOW ROOT ...

// --- reasons.cpp -------------------------------------------------------------------------------

// Reason with the texts of `id`, followed by ": <detail>" when detail is not empty.
// The level of a reason is given by its prefix: B- blocked, W- warning.
Reason make_reason(std::string_view id, std::string_view detail = {});
Level level_of(std::string_view id);

// --- text.cpp ----------------------------------------------------------------------------------

// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF.
bool valid_utf8(std::string_view s);
// Bytes below 0x20 other than \t \n \r, 0x7F, C1 controls (U+0080..U+009F) and bidi controls
// (U+202A..U+202E, U+2066..U+2069).
bool has_control(std::string_view s);

struct RegexInfo {
    bool nested = false;     // quantified group with a quantifier or alternation inside, quantified backref
    bool complex = false;    // cannot be analysed reliably: inline flags, \Q...\E, \c, comments ...
    bool bad_flags = false;  // inline flags outside i m s x, PCRE verbs (*...)
};
RegexInfo analyze_regex(std::string_view pattern);
bool has_nested_quantifier(std::string_view pattern);

// Duplicate keys in any object of a JSON text (keys compared after unescaping). libbson collapses
// some of them inside Extended JSON wrappers before the BSON can be walked.
bool has_duplicate_keys(std::string_view json);

}  // namespace askmdb::verify::detail
