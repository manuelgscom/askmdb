// Small JSON scanner: finds the raw text of a member without re-serializing it.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "verify/internal.hpp"

namespace askmdb::verify {

namespace {

constexpr int kMaxScanDepth = 512;

void put_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Scanner {
public:
    explicit Scanner(std::string_view s, bool lenient = false) : s_(s), lenient_(lenient) {}
    bool duplicate() const { return duplicate_; }

    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool at_end() const { return i_ == s_.size(); }
    std::size_t pos() const { return i_; }

    // A string starting at the current position; decoded into *out if given.
    bool string(std::string* out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        while (i_ < s_.size()) {
            auto c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') {
                ++i_;
                return true;
            }
            if (c < 0x20) return false;
            if (c != '\\') {
                if (out) *out += static_cast<char>(c);
                ++i_;
                continue;
            }
            if (++i_ >= s_.size()) return false;
            char e = s_[i_++];
            char simple = 0;
            switch (e) {
                case '"': simple = '"'; break;
                case '\\': simple = '\\'; break;
                case '/': simple = '/'; break;
                case 'b': simple = '\b'; break;
                case 'f': simple = '\f'; break;
                case 'n': simple = '\n'; break;
                case 'r': simple = '\r'; break;
                case 't': simple = '\t'; break;
                case 'u': break;
                default: return false;
            }
            if (e != 'u') {
                if (out) *out += simple;
                continue;
            }
            auto cp = hex4();
            if (!cp) return false;
            std::uint32_t code = *cp;
            if (code >= 0xD800 && code <= 0xDBFF) {
                if (i_ + 1 >= s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u') return false;
                i_ += 2;
                auto lo = hex4();
                if (!lo || *lo < 0xDC00 || *lo > 0xDFFF) return false;
                code = 0x10000 + ((code - 0xD800) << 10) + (*lo - 0xDC00);
            } else if (code >= 0xDC00 && code <= 0xDFFF) {
                return false;
            }
            if (out) put_utf8(*out, code);
        }
        return false;
    }

    bool value(int depth = 0) {
        if (depth > kMaxScanDepth) return false;
        ws();
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '"') return string(nullptr);
        if (c == '{') return object(depth, nullptr);
        if (c == '[') {
            ++i_;
            ws();
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return true;
            }
            while (true) {
                if (!value(depth + 1)) return false;
                ws();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ']') {
                    ++i_;
                    return true;
                }
                if (s_[i_++] != ',') return false;
            }
        }
        std::size_t b = i_;
        while (i_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' ||
                                  s_[i_] == '+' || s_[i_] == '.'))
            ++i_;
        auto tok = s_.substr(b, i_ - b);
        if (tok == "true" || tok == "false" || tok == "null") return true;
        if (lenient_) return !tok.empty();  // NaN, Infinity ... (libbson reads them)
        if (tok.empty() || !(tok[0] == '-' || (tok[0] >= '0' && tok[0] <= '9'))) return false;
        for (char d : tok) {
            if (!((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E')) return false;
        }
        return true;
    }

    // An object; for each member calls on_member(decoded key, raw value).
    template <class F>
    bool object(int depth, F* on_member) {
        if (i_ >= s_.size() || s_[i_] != '{') return false;
        ++i_;
        ws();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        std::vector<std::string> keys;
        while (true) {
            ws();
            std::string key;
            if (!string(&key)) return false;
            if (std::find(keys.begin(), keys.end(), key) != keys.end()) duplicate_ = true;
            keys.push_back(key);
            ws();
            if (i_ >= s_.size() || s_[i_++] != ':') return false;
            ws();
            std::size_t b = i_;
            if (!value(depth + 1)) return false;
            if (on_member) (*on_member)(key, s_.substr(b, i_ - b));
            ws();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == '}') {
                ++i_;
                return true;
            }
            if (s_[i_++] != ',') return false;
        }
    }

    bool object(int depth, std::nullptr_t) {
        auto none = [](const std::string&, std::string_view) {};
        return object(depth, &none);
    }

private:
    std::optional<std::uint32_t> hex4() {
        if (i_ + 4 > s_.size()) return std::nullopt;
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            char h = s_[i_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= static_cast<std::uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') v |= static_cast<std::uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= static_cast<std::uint32_t>(h - 'A' + 10);
            else return std::nullopt;
        }
        return v;
    }

    std::string_view s_;
    bool lenient_;
    std::size_t i_ = 0;
    bool duplicate_ = false;
};

}  // namespace

std::optional<std::string> raw_member(std::string_view json_object, std::string_view key) {
    Scanner sc(json_object);
    sc.ws();
    int found = 0;
    std::string raw;
    auto on_member = [&](const std::string& k, std::string_view value) {
        if (k != key) return;
        ++found;
        raw = std::string(value);
    };
    if (!sc.object(0, &on_member)) return std::nullopt;
    sc.ws();
    if (!sc.at_end() || found != 1) return std::nullopt;
    return raw;
}

bool detail::has_duplicate_keys(std::string_view json) {
    Scanner sc(json, true);
    sc.value();
    return sc.duplicate();
}

std::optional<std::string> json_string(std::string_view raw) {
    Scanner sc(raw);
    std::string out;
    if (!sc.string(&out) || !sc.at_end()) return std::nullopt;
    return out;
}

}  // namespace askmdb::verify
