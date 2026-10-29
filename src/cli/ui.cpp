#include "cli/ui.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "exec/term.hpp"

namespace askmdb::cli {

namespace {

constexpr std::string_view kGreen = "\x1b[32m";
constexpr std::string_view kYellow = "\x1b[33m";
constexpr std::string_view kMagenta = "\x1b[35m";
constexpr std::string_view kCyan = "\x1b[36m";
constexpr std::string_view kCyanBold = "\x1b[1;36m";
constexpr std::string_view kRedBold = "\x1b[1;31m";
constexpr std::string_view kBold = "\x1b[1m";
constexpr std::string_view kDim = "\x1b[2m";
constexpr std::string_view kReset = "\x1b[0m";

// Empty text is not painted, to avoid stray escape codes.
std::string paint(std::string_view text, std::string_view color, bool enabled) {
    if (!enabled || text.empty()) return std::string(text);
    std::string out(color);
    out += text;
    out += kReset;
    return out;
}

// Escapes control characters and paints each line separately, so the indentation stays
// uncolored. The first line gets prefix first and the rest get rest.
std::string indent(std::string_view text, std::string_view first, std::string_view rest, std::string_view style,
                   bool color) {
    const std::string safe = exec::escape_control(text);
    const std::string_view view(safe);
    std::string out(first);
    for (std::size_t start = 0;;) {
        const std::size_t end = view.find('\n', start);
        out += paint(view.substr(start, end == std::string_view::npos ? end : end - start), style, color);
        if (end == std::string_view::npos) break;
        out += '\n';
        out += rest;
        start = end + 1;
    }
    return out;
}

// Puts first before the first line and rest after every newline.
std::string prefix_lines(std::string_view text, std::string_view first, std::string_view rest) {
    std::string out(first);
    for (char c : text) {
        out += c;
        if (c == '\n') out += rest;
    }
    return out;
}

bool is_word_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_word_char(char c) { return is_word_start(c) || is_digit(c); }

// End of the string or regex literal that starts at i (one past the closing quote). It stops at
// a newline, so no token spans two lines.
std::size_t literal_end(std::string_view s, std::size_t i, char close) {
    std::size_t j = i + 1;
    while (j < s.size() && s[j] != close && s[j] != '\n') j += (s[j] == '\\' && j + 1 < s.size() && s[j + 1] != '\n') ? 2 : 1;
    return j < s.size() && s[j] == close ? j + 1 : j;
}

// Whether the next non-blank character after i is ':' (the token before it is a key).
bool before_colon(std::string_view s, std::size_t i) {
    while (i < s.size() && s[i] == ' ') ++i;
    return i < s.size() && s[i] == ':';
}

// Syntax highlighting of mongosh text (as produced by mongo::to_mongosh, control characters
// already escaped). Each token is painted on its own and closed with a reset, and no token spans
// a newline, so every line ends uncolored and removing the escape codes gives back the input.
std::string highlight_mongosh(std::string_view s) {
    std::string out;
    std::size_t i = 0;
    auto emit = [&](std::size_t end, std::string_view style) {
        out += paint(s.substr(i, end - i), style, !style.empty());
        i = end;
    };
    // Head: db.<collection>.aggregate or db.getCollection("<collection>").aggregate.
    if (s.starts_with("db.")) {
        emit(3, kDim);
        std::size_t j = i;
        while (j < s.size() && is_word_char(s[j])) ++j;
        if (s.substr(i, j - i) == "getCollection" && j < s.size() && s[j] == '(' && j + 1 < s.size() &&
            s[j + 1] == '"') {
            emit(j + 1, {});
            emit(literal_end(s, i, '"'), kBold);
            if (i < s.size() && s[i] == ')') emit(i + 1, {});
        } else {
            emit(j, kBold);
        }
        if (s.substr(i).starts_with(".aggregate")) emit(i + 10, kDim);
    }
    std::vector<char> nest;  // open '[' and '{'; a stage key sits right inside "[{"
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"') {
            const std::size_t j = literal_end(s, i, '"');
            if (before_colon(s, j)) {
                emit(j, {});
            } else {
                emit(j, j > i + 1 && s[i + 1] == '$' ? kMagenta : kGreen);
            }
        } else if (c == '/') {
            std::size_t j = literal_end(s, i, '/');
            while (j < s.size() && s[j] >= 'a' && s[j] <= 'z') ++j;
            emit(j, kGreen);
        } else if (is_digit(c) || (c == '-' && i + 1 < s.size() && is_digit(s[i + 1]))) {
            std::size_t j = i + 1;
            while (j < s.size() && (is_word_char(s[j]) || s[j] == '.' ||
                                    ((s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E'))))
                ++j;
            emit(j, kYellow);
        } else if (is_word_start(c) || (c == '-' && s.substr(i + 1).starts_with("Infinity"))) {
            std::size_t j = i + 1;
            while (j < s.size() && is_word_char(s[j])) ++j;
            const std::string_view word = s.substr(i, j - i);
            if (before_colon(s, j)) {
                const bool stage = nest.size() == 2 && nest[0] == '[' && nest[1] == '{';
                emit(j, c != '$' ? std::string_view{} : stage ? kCyanBold : kCyan);
            } else if (word == "true" || word == "false" || word == "null" || word == "undefined" || word == "NaN" ||
                       word == "Infinity" || word == "-Infinity") {
                emit(j, kYellow);
            } else {
                emit(j, {});
            }
        } else {
            if (c == '[' || c == '{') nest.push_back(c);
            if ((c == ']' || c == '}') && !nest.empty()) nest.pop_back();
            out += c;
            ++i;
        }
    }
    return out;
}

std::string reasons(const verify::Assessment& a, UiLang lang) {
    std::string out;
    for (const auto& r : a.reasons) {
        out += "    · ";
        out += exec::escape_control(reason_text(r, lang));
        out += '\n';
    }
    return out;
}

}  // namespace

bool use_color(bool stdout_tty, const char* no_color, const char* term) {
    if (!stdout_tty) return false;
    if (no_color != nullptr && *no_color != '\0') return false;
    if (term != nullptr && std::string_view(term) == "dumb") return false;
    return true;
}

std::string_view reason_text(const verify::Reason& reason, UiLang lang) {
    return lang == UiLang::Es ? reason.es : reason.en;
}

std::string render_suggestion(std::string_view mongosh, std::string_view explanation, bool color) {
    const std::string safe = exec::escape_control(mongosh);
    std::string out = "\n" + prefix_lines(color ? highlight_mongosh(safe) : safe, "  ❯ ", "    ") + "\n";
    if (!explanation.empty()) out += indent(explanation, "    ", "    ", kDim, color) + "\n";
    out += '\n';
    return out;
}

std::string render_level(const verify::Assessment& a, UiLang lang, bool color, bool ask) {
    std::string out = "  ";
    switch (a.level) {
        case verify::Level::Read:
            out += paint("● " + std::string(tr(lang, Msg::LevelRead)), kGreen, color);
            if (ask) {
                out += "  ·  ";
                out += tr(lang, Msg::PromptRead);
            } else {
                out += '\n';
            }
            break;
        case verify::Level::Warn:
            out += paint("▲ " + std::string(tr(lang, Msg::LevelWarn)), kYellow, color);
            out += '\n';
            out += reasons(a, lang);
            if (ask) out += render_prompt(a.level, lang);
            break;
        case verify::Level::Blocked:
            out += paint("■ " + std::string(tr(lang, Msg::LevelBlocked)), kRedBold, color);
            out += "  ";
            out += tr(lang, Msg::NotExecuted);
            out += '\n';
            out += reasons(a, lang);
            break;
    }
    return out;
}

std::string render_prompt(verify::Level level, UiLang lang) {
    switch (level) {
        case verify::Level::Read: return "  " + std::string(tr(lang, Msg::PromptRead));
        case verify::Level::Warn: return "  " + std::string(tr(lang, Msg::PromptWarn));
        case verify::Level::Blocked: break;
    }
    return {};
}

std::string render_refusal(bool unsure, std::string_view explanation, UiLang lang, bool color) {
    std::string text(tr(lang, unsure ? Msg::Unsure : Msg::Refuse));
    text += explanation;
    const std::string mark = paint(unsure ? "?" : "✗", kDim, color);
    return "\n" + indent(text, "  " + mark + " ", "    ", {}, false) + "\n\n";
}

std::string render_status(bool ok, std::string_view text, bool color) {
    std::string out = "  ";
    out += ok ? paint("✓", kGreen, color) : paint("✗", kDim, color);
    out += ' ';
    out += text;
    out += '\n';
    return out;
}

std::string render_results(std::string_view table, bool truncated, int max_rows, UiLang lang, bool color) {
    std::string out = "\n";
    if (table.empty()) {
        out += "  " + paint("(" + std::string(tr(lang, Msg::NoRows)) + ")", kDim, color) + "\n";
        return out;
    }
    const std::string safe = exec::escape_control(table);
    if (color) {
        // Bold header (the first line), leaving its indentation as is.
        const std::size_t end = std::min(safe.find('\n'), safe.size());
        const std::size_t start = std::min(safe.find_first_not_of(' '), end);
        out += safe.substr(0, start);
        out += paint(std::string_view(safe).substr(start, end - start), kBold, true);
        out += safe.substr(end);
    } else {
        out += safe;
    }
    if (!out.ends_with('\n')) out += '\n';
    if (truncated)
        out += "  " + paint("(" + fill(tr(lang, Msg::MoreRows), std::to_string(max_rows)) + ")", kDim, color) + "\n";
    return out;
}

}  // namespace askmdb::cli
