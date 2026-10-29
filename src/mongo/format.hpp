#pragma once
// Private to src/mongo: value formatting shared by present.cpp and shell.cpp.

#include <bson/bson.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace askmdb::mongo {

// "2026-01-01T00:00:00Z", with ".123" only when the milliseconds are not zero. UTC.
std::string iso_date(std::int64_t millis);

// A single value as compact relaxed Extended JSON ({"a":1,"b":[2,3]}).
std::string compact_json(const bson_iter_t& value);

// JSON string literal; control characters, DEL, C1 controls and U+2028/U+2029 escaped as \uXXXX.
std::string json_quote(std::string_view text);

}  // namespace askmdb::mongo
