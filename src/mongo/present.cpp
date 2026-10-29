// Results as a terminal table or as JSON.
//
// Cells: strings as they are (terminal-safe, \n and \t shown escaped); integers with thousands
// separators and doubles with two decimals in the interface language; decimal128 as its string;
// dates ISO UTC; ObjectId hex; booleans true/false; null as "null" (a missing key is an empty
// cell, so both stay distinguishable); subdocuments, arrays and any other type as compact relaxed
// JSON cut at 40 characters with "…". Numeric columns are right-aligned.
#include <cmath>
#include <cstdio>
#include <ctime>
#include <unordered_set>

#include "exec/term.hpp"
#include "mongo/format.hpp"
#include "mongo/mongo.hpp"

namespace askmdb::mongo {

namespace {

constexpr std::size_t kCellMaxChars = 40;
constexpr std::string_view kIndent = "   ";
constexpr std::string_view kGap = "   ";

// Decodes one UTF-8 code point at `pos` and advances it; invalid bytes yield themselves.
char32_t next_code_point(std::string_view s, std::size_t& pos) {
    auto c = static_cast<unsigned char>(s[pos]);
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (c < 0x80 || extra < 0 || pos + static_cast<std::size_t>(extra) >= s.size()) {
        ++pos;
        return c;
    }
    char32_t cp = c & (0x3Fu >> extra);
    for (int i = 1; i <= extra; ++i) {
        auto cc = static_cast<unsigned char>(s[pos + static_cast<std::size_t>(i)]);
        if ((cc & 0xC0) != 0x80) {
            ++pos;
            return c;
        }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    pos += static_cast<std::size_t>(extra) + 1;
    return cp;
}

int code_point_width(char32_t cp) {
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0xFE00 && cp <= 0xFE0F)) {
        return 0;
    }
    bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
                (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
                (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD);
    return wide ? 2 : 1;
}

// At most `max` code points; longer text keeps max - 1 and ends with "…".
std::string cut(std::string_view text, std::size_t max) {
    std::size_t pos = 0;
    std::size_t count = 0;
    std::size_t keep = 0;  // byte length of the first max - 1 code points
    while (pos < text.size()) {
        next_code_point(text, pos);
        ++count;
        if (count == max - 1) keep = pos;
        if (count > max) return std::string(text.substr(0, keep)) + "…";
    }
    return std::string(text);
}

std::string safe_text(std::string_view text) {
    std::string flat;
    for (char c : text) {
        if (c == '\n') {
            flat += "\\n";
        } else if (c == '\t') {
            flat += "\\t";
        } else {
            flat += c;
        }
    }
    return exec::escape_control(flat);
}

std::string group_digits(std::string_view digits, char separator) {
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += separator;
        out += digits[i];
    }
    return out;
}

std::string format_int(std::int64_t value, NumberStyle style) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(value));
    std::string_view text(buf);
    std::string sign;
    if (text.front() == '-') {
        sign = "-";
        text.remove_prefix(1);
    }
    return sign + group_digits(text, style == NumberStyle::Es ? '.' : ',');
}

std::string format_double(double value, NumberStyle style) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    char thousands = style == NumberStyle::Es ? '.' : ',';
    char decimal = style == NumberStyle::Es ? ',' : '.';
    char buf[64];
    if (std::fabs(value) >= 1e15) {
        std::snprintf(buf, sizeof buf, "%.6g", value);
        std::string out(buf);
        for (char& c : out) {
            if (c == '.') c = decimal;
        }
        return out;
    }
    std::snprintf(buf, sizeof buf, "%.2f", value);
    std::string_view text(buf);
    std::string sign;
    if (text.front() == '-') {
        text.remove_prefix(1);
        if (text != "0.00") sign = "-";
    }
    auto dot = text.find('.');
    return sign + group_digits(text.substr(0, dot), thousands) + decimal + std::string(text.substr(dot + 1));
}

bool is_number(bson_type_t type) {
    return type == BSON_TYPE_INT32 || type == BSON_TYPE_INT64 || type == BSON_TYPE_DOUBLE ||
           type == BSON_TYPE_DECIMAL128;
}

std::string minify(std::string_view json) {
    std::string out;
    bool in_string = false;
    bool escaped = false;
    for (char c : json) {
        if (in_string) {
            out += c;
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
        } else if (c == '"') {
            in_string = true;
            out += c;
        } else if (c != ' ' && c != '\n' && c != '\t' && c != '\r') {
            out += c;
        }
    }
    return out;
}

}  // namespace

std::string iso_date(std::int64_t millis) {
    std::int64_t secs = millis / 1000;
    std::int64_t ms = millis % 1000;
    if (ms < 0) {
        ms += 1000;
        secs -= 1;
    }
    std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
    if (!gmtime_r(&t, &tm)) return std::to_string(millis);
    char buf[64];
    if (ms) {
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                      tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    } else {
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                      tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    return buf;
}

std::string compact_json(const bson_iter_t& value) {
    bson_t wrap;
    bson_init(&wrap);
    bson_append_iter(&wrap, "v", 1, &value);
    std::size_t len = 0;
    char* json = bson_as_relaxed_extended_json(&wrap, &len);
    bson_destroy(&wrap);
    if (!json) return "?";
    std::string out = minify(std::string_view(json, len));
    bson_free(json);
    constexpr std::string_view prefix = "{\"v\":";
    if (out.starts_with(prefix) && out.ends_with('}')) out = out.substr(prefix.size(), out.size() - prefix.size() - 1);
    return out;
}

namespace detail {

std::size_t display_width(std::string_view text) {
    std::size_t width = 0;
    std::size_t pos = 0;
    while (pos < text.size()) width += static_cast<std::size_t>(code_point_width(next_code_point(text, pos)));
    return width;
}

std::string format_cell(const bson_iter_t& value, NumberStyle style) {
    switch (bson_iter_type(&value)) {
        case BSON_TYPE_UTF8: {
            std::uint32_t len = 0;
            const char* s = bson_iter_utf8(&value, &len);
            return safe_text(std::string_view(s, len));
        }
        case BSON_TYPE_INT32:
            return format_int(bson_iter_int32(&value), style);
        case BSON_TYPE_INT64:
            return format_int(bson_iter_int64(&value), style);
        case BSON_TYPE_DOUBLE:
            return format_double(bson_iter_double(&value), style);
        case BSON_TYPE_DECIMAL128: {
            bson_decimal128_t dec;
            bson_iter_decimal128(&value, &dec);
            char buf[BSON_DECIMAL128_STRING];
            bson_decimal128_to_string(&dec, buf);
            return buf;
        }
        case BSON_TYPE_DATE_TIME:
            return iso_date(bson_iter_date_time(&value));
        case BSON_TYPE_OID: {
            char buf[25];
            bson_oid_to_string(bson_iter_oid(&value), buf);
            return buf;
        }
        case BSON_TYPE_BOOL:
            return bson_iter_bool(&value) ? "true" : "false";
        case BSON_TYPE_NULL:
            return "null";
        default:
            return exec::escape_control(cut(compact_json(value), kCellMaxChars));
    }
}

}  // namespace detail

std::string render_table(const std::vector<Doc>& rows, NumberStyle style) {
    if (rows.empty()) return {};

    std::vector<std::string> columns;
    std::unordered_set<std::string> known;
    for (const auto& row : rows) {
        bson_iter_t it;
        if (!bson_iter_init(&it, row.get())) continue;
        while (bson_iter_next(&it)) {
            std::string key(bson_iter_key(&it), bson_iter_key_len(&it));
            if (known.insert(key).second) columns.push_back(std::move(key));
        }
    }

    std::vector<std::vector<std::string>> cells(rows.size(), std::vector<std::string>(columns.size()));
    std::vector<bool> numeric(columns.size(), true);
    std::vector<bool> any_number(columns.size(), false);
    for (std::size_t r = 0; r < rows.size(); ++r) {
        for (std::size_t c = 0; c < columns.size(); ++c) {
            bson_iter_t it;
            if (!bson_iter_init_find_w_len(&it, rows[r].get(), columns[c].data(), static_cast<int>(columns[c].size()))) {
                continue;
            }
            bson_type_t type = bson_iter_type(&it);
            if (is_number(type)) {
                any_number[c] = true;
            } else if (type != BSON_TYPE_NULL) {
                numeric[c] = false;
            }
            cells[r][c] = detail::format_cell(it, style);
        }
    }

    std::vector<std::string> headers;
    std::vector<std::size_t> widths;
    for (std::size_t c = 0; c < columns.size(); ++c) {
        headers.push_back(safe_text(columns[c]));
        std::size_t w = detail::display_width(headers.back());
        for (const auto& row : cells) w = std::max(w, detail::display_width(row[c]));
        widths.push_back(w);
        numeric[c] = numeric[c] && any_number[c];
    }

    std::string out;
    auto emit = [&](const std::vector<std::string>& line) {
        std::string text(kIndent);
        for (std::size_t c = 0; c < line.size(); ++c) {
            if (c) text += kGap;
            std::string pad(widths[c] - detail::display_width(line[c]), ' ');
            text += numeric[c] ? pad + line[c] : line[c] + pad;
        }
        while (!text.empty() && text.back() == ' ') text.pop_back();
        out += text;
        out += '\n';
    };
    emit(headers);
    for (const auto& row : cells) emit(row);
    return out;
}

std::string rows_json(const std::vector<Doc>& rows) {
    std::string out = "[";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        std::size_t len = 0;
        char* json = bson_as_relaxed_extended_json(rows[i].get(), &len);
        if (i) out += ", ";
        if (json) {
            out.append(json, len);
            bson_free(json);
        } else {
            out += "null";
        }
    }
    out += "]";
    return out;
}

}  // namespace askmdb::mongo
