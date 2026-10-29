// Byte-level checks: UTF-8, control characters and regex patterns.

#include <cstdint>
#include <vector>

#include "verify/internal.hpp"

namespace askmdb::verify::detail {

bool valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            ++i;
            continue;
        }
        std::size_t len;
        std::uint32_t cp;
        if (b >= 0xC2 && b <= 0xDF) {
            len = 2;
            cp = b & 0x1Fu;
        } else if (b >= 0xE0 && b <= 0xEF) {
            len = 3;
            cp = b & 0x0Fu;
        } else if (b >= 0xF0 && b <= 0xF4) {
            len = 4;
            cp = b & 0x07u;
        } else {
            return false;
        }
        if (i + len > s.size()) return false;
        for (std::size_t k = 1; k < len; ++k) {
            auto c = static_cast<unsigned char>(s[i + k]);
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3Fu);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += len;
    }
    return true;
}

bool has_control(std::string_view s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto u = static_cast<unsigned char>(s[i]);
        if ((u < 0x20 && u != '\t' && u != '\n' && u != '\r') || u == 0x7F) return true;
        if (i + 1 < s.size() && u == 0xC2) {  // C1 controls U+0080..U+009F
            auto n = static_cast<unsigned char>(s[i + 1]);
            if (n >= 0x80 && n <= 0x9F) return true;
        }
        if (i + 2 < s.size() && u == 0xE2) {  // bidi controls U+202A..U+202E, U+2066..U+2069
            auto a = static_cast<unsigned char>(s[i + 1]), b = static_cast<unsigned char>(s[i + 2]);
            if (a == 0x80 && b >= 0xAA && b <= 0xAE) return true;
            if (a == 0x81 && b >= 0xA6 && b <= 0xA9) return true;
        }
    }
    return false;
}

namespace {

bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

RegexInfo analyze_regex(std::string_view p) {
    RegexInfo info;
    auto quantifier_at = [&](std::size_t i) {
        return i < p.size() && (p[i] == '*' || p[i] == '+' || p[i] == '?' || p[i] == '{');
    };
    struct Group {
        bool quant = false;  // contains a quantifier
        bool alt = false;    // contains an alternation
    };
    std::vector<Group> groups{Group{}};
    std::size_t i = 0;
    while (i < p.size()) {
        char c = p[i];
        if (c == '\\') {
            char e = i + 1 < p.size() ? p[i + 1] : '\0';
            if (e == 'Q') {  // \Q...\E: quoted text, not analysed
                info.complex = true;
                auto end = p.find("\\E", i + 2);
                i = end == std::string_view::npos ? p.size() : end + 2;
            } else if (e == 'c') {  // \cX: takes the next character, whatever it is
                info.complex = true;
                i += 3;
            } else if (is_digit(e) && e != '0') {  // backreference \1
                i += 2;
                while (i < p.size() && is_digit(p[i])) ++i;
                if (quantifier_at(i)) info.nested = true;
            } else if (e == 'k' || e == 'g') {  // \k<n> \k'n' \k{n} \g{n} \g1
                i += 2;
                if (i < p.size() && (p[i] == '<' || p[i] == '\'' || p[i] == '{')) {
                    char close = p[i] == '<' ? '>' : p[i] == '{' ? '}' : '\'';
                    auto end = p.find(close, i + 1);
                    i = end == std::string_view::npos ? p.size() : end + 1;
                } else {
                    if (i < p.size() && p[i] == '-') ++i;
                    while (i < p.size() && is_digit(p[i])) ++i;
                }
                if (quantifier_at(i)) info.nested = true;
            } else {
                i += 2;
            }
        } else if (c == '[') {  // character class: its content is never a quantifier
            ++i;
            if (i < p.size() && p[i] == '^') ++i;
            if (i < p.size() && p[i] == ']') ++i;
            while (i < p.size() && p[i] != ']') i += p[i] == '\\' ? 2 : 1;
            ++i;
        } else if (c == '(') {
            groups.push_back(Group{});
            ++i;
            if (i < p.size() && p[i] == '*') {  // PCRE verb (*LIMIT_...), (*UTF) ...
                info.bad_flags = true;
            } else if (i < p.size() && p[i] == '?') {
                ++i;
                std::size_t b = i;
                while (i < p.size() && (is_letter(p[i]) || p[i] == '-' || p[i] == '^')) ++i;
                bool letters = i > b;
                if (letters && i < p.size() && (p[i] == ')' || p[i] == ':')) {  // inline flags
                    info.complex = true;
                    for (std::size_t k = b; k < i; ++k) {
                        char f = p[k];
                        if (f != 'i' && f != 'm' && f != 's' && f != 'x' && f != '-') info.bad_flags = true;
                    }
                } else if (!letters && i < p.size() &&
                           (p[i] == '#' || p[i] == '(' || p[i] == '&' || p[i] == '+' || is_digit(p[i]))) {
                    info.complex = true;  // comments, conditionals, recursion
                } else {
                    i = b;  // (?:  (?=  (?!  (?<name> ...: an ordinary group
                }
            }
        } else if (c == ')') {
            ++i;
            if (groups.size() < 2) continue;
            Group g = groups.back();
            groups.pop_back();
            bool quantified = quantifier_at(i);
            if (quantified && (g.quant || g.alt)) info.nested = true;
            groups.back().quant = groups.back().quant || g.quant || quantified;
            groups.back().alt = groups.back().alt || g.alt;
        } else {
            if (c == '|') groups.back().alt = true;
            if (quantifier_at(i)) groups.back().quant = true;
            ++i;
        }
    }
    return info;
}

bool has_nested_quantifier(std::string_view pattern) { return analyze_regex(pattern).nested; }

}  // namespace askmdb::verify::detail
