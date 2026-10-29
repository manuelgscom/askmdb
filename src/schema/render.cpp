#include <nlohmann/json.hpp>

#include "schema/internal.hpp"
#include "schema/schema.hpp"

namespace askmdb::schema {

namespace detail {

std::string render_line(const Path& p) {
    std::string out = p.path;
    if (p.optional) out += '?';
    out += ' ';
    for (std::size_t i = 0; i < p.types.size(); ++i) {
        if (i) out += '|';
        out += p.types[i];
    }
    if (!p.values.empty()) {
        out += ' ';
        out += nlohmann::json(p.values).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    out += '\n';
    return out;
}

}  // namespace detail

bool plain_text(std::string_view text) {
    if (!bson_utf8_validate(text.data(), text.size(), false)) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto u = static_cast<unsigned char>(text[i]);
        const auto next = [&](std::size_t k) {
            return i + k < text.size() ? static_cast<unsigned char>(text[i + k]) : 0u;
        };
        if (u < 0x20 || u == 0x7f) return false;
        if (u == 0xc2 && next(1) >= 0x80 && next(1) <= 0x9f) return false;  // C1: U+0080..U+009F
        if (u == 0xe2 && ((next(1) == 0x80 && next(2) >= 0xaa && next(2) <= 0xae) ||
                          (next(1) == 0x81 && next(2) >= 0xa6 && next(2) <= 0xa9)))
            return false;  // bidi: U+202A..U+202E, U+2066..U+2069
        if (u == '<' && next(1) == '|') return false;
    }
    return true;
}

std::string render(const Schema& schema) {
    std::string out = "db=" + schema.db + "\n";
    for (const auto& c : schema.collections) {
        out += "## " + c.name + "\n";
        for (const auto& p : c.paths) out += detail::render_line(p);
    }
    return out;
}

}  // namespace askmdb::schema
