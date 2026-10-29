// verify(): text checks, then the two passes of the walker; the level is the highest finding.

#include "verify/verify.hpp"

#include "verify/walk.hpp"

namespace askmdb::verify {

std::string_view to_string(Level level) {
    return level == Level::Read ? "read" : level == Level::Warn ? "warn" : "blocked";
}

std::optional<Reason> check_db(std::string_view db) {
    // MongoDB's rules for database names
    constexpr std::string_view kForbidden("/\\. \"$\0", 7);
    if (db.empty() || db.size() > 63 || db.find_first_of(kForbidden) != std::string_view::npos)
        return detail::make_reason("B-BAD-DB", db);
    std::string lower;
    for (char c : db) lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    if (lower == "admin" || lower == "local" || lower == "config") return detail::make_reason("B-SYSTEM-DB", db);
    return std::nullopt;
}

Assessment verify(const Pipeline& pipeline, std::string_view collection, std::string_view db,
                  const schema::Schema& schema) {
    detail::Walker w(schema, db);
    if (auto r = check_db(db)) w.add(*r);

    const std::string& text = pipeline.text;
    if (text.size() > kMaxPipelineBytes) w.add("B-TOO-LONG");
    if (detail::has_control(text)) w.add("B-CONTROL-CHAR");
    if (!detail::valid_utf8(text)) w.add("B-UTF8");
    if (!pipeline.bson) {
        w.add("B-PARSE", pipeline.parse_error);
        return w.result();
    }
    // A JSON object {"0": ...} gives the same BSON as an array: only the text tells them apart.
    auto first = text.find_first_not_of(" \t\n\r");
    if (first == std::string::npos || text[first] != '[') {
        w.add("B-NOT-ARRAY");
        return w.result();
    }

    if (detail::has_duplicate_keys(text)) w.add("B-DUP-KEY");

    detail::Scope sc = w.scope_for(collection);
    w.scan(pipeline.bson.get());
    w.pipeline(pipeline.bson.get(), sc);
    return w.result();
}

}  // namespace askmdb::verify
