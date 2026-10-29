#include "exec/term.hpp"

namespace askmdb::exec {

namespace {

constexpr char kHex[] = "0123456789abcdef";

void append_codepoint(std::string& out, unsigned cp) {
    out += "\\u{";
    for (int shift = 12; shift >= 0; shift -= 4) out += kHex[(cp >> shift) & 0x0f];
    out += '}';
}

}  // namespace

std::string escape_control(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const auto u = static_cast<unsigned char>(c);
        if ((u < 0x20 && c != '\n' && c != '\t') || u == 0x7f) {
            out += "\\x";
            out += kHex[u >> 4];
            out += kHex[u & 0x0f];
            continue;
        }
        // C1 controls U+0080..U+009F: C2 80..C2 9F.
        if (u == 0xc2 && i + 1 < text.size()) {
            const auto v = static_cast<unsigned char>(text[i + 1]);
            if (v >= 0x80 && v <= 0x9f) {
                append_codepoint(out, v);
                ++i;
                continue;
            }
        }
        // Bidi controls U+202A..U+202E (E2 80 AA..AE) and U+2066..U+2069 (E2 81 A6..A9).
        if (u == 0xe2 && i + 2 < text.size()) {
            const auto v = static_cast<unsigned char>(text[i + 1]);
            const auto w = static_cast<unsigned char>(text[i + 2]);
            const bool bidi = (v == 0x80 && w >= 0xaa && w <= 0xae) || (v == 0x81 && w >= 0xa6 && w <= 0xa9);
            if (bidi) {
                append_codepoint(out, 0x2000u + ((v & 0x3fu) << 6) + (w & 0x3fu));
                i += 2;
                continue;
            }
        }
        out += c;
    }
    return out;
}

}  // namespace askmdb::exec
