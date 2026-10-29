#include "cli/flow.hpp"

#include "cli/ui.hpp"

namespace askmdb::cli {

namespace {

constexpr int kMaxInvalid = 3;

std::string_view trim(std::string_view s) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

// ASCII lowercase; also maps "Í" (UTF-8) to "í" so "SÍ" is accepted.
std::string lower(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
        } else if (c == '\xC3' && i + 1 < s.size() && s[i + 1] == '\x8D') {
            out += "\xC3\xAD";
            ++i;
        } else {
            out += c;
        }
    }
    return out;
}

int cancel(const Io& io, const FlowOptions& opt) {
    io.write(render_status(false, tr(opt.lang, Msg::Cancelled), opt.color));
    return kExitOk;
}

}  // namespace

Answer parse_answer(std::string_view line) {
    const std::string a = lower(trim(line));
    if (a.empty()) return Answer::Empty;
    if (a == "s" || a == "si" || a == "sí" || a == "y" || a == "yes") return Answer::Yes;
    if (a == "n" || a == "no") return Answer::No;
    if (a == "c") return Answer::Copy;
    return Answer::Invalid;
}

int run_flow(const infer::Suggestion& suggestion, std::string_view mongosh, const FlowOptions& opt, const Io& io) {
    const UiLang lang = opt.lang;
    const verify::Assessment a = io.verify(suggestion);
    io.write(render_suggestion(mongosh, suggestion.explanation, opt.color));

    // Blocked: never runs, never asks, in any mode.
    if (a.level == verify::Level::Blocked) {
        io.write(render_level(a, lang, opt.color, false));
        return kExitBlocked;
    }
    const config::Mode mode = opt.interactive ? opt.mode : config::Mode::Suggest;
    if (mode == config::Mode::Suggest) {
        io.write(render_level(a, lang, opt.color, false));
        return kExitOk;
    }
    if (mode == config::Mode::Auto && a.level == verify::Level::Read) {
        io.write(render_level(a, lang, opt.color, false));
        return io.execute(suggestion);
    }

    // Confirm (read or warn) and auto (warn): explicit answer; Enter = yes only for read.
    io.write(render_level(a, lang, opt.color, true));
    Answer answer = Answer::Invalid;
    for (int invalid = 0;;) {
        const auto line = io.read_line();
        if (!line) {
            io.write("\n");
            return cancel(io, opt);
        }
        answer = parse_answer(*line);
        if (answer == Answer::Empty) answer = a.level == verify::Level::Read ? Answer::Yes : Answer::No;
        if (answer != Answer::Invalid) break;
        if (++invalid >= kMaxInvalid) return cancel(io, opt);
        io.write(render_prompt(a.level, lang));
    }

    switch (answer) {
        case Answer::Yes:
            return io.execute(suggestion);
        case Answer::Copy:
            if (!io.copy(std::string(mongosh))) {
                io.write(render_status(false, tr(lang, Msg::CopyFailed), opt.color));
                return kExitError;
            }
            io.write(render_status(true, tr(lang, Msg::Copied), opt.color));
            return kExitOk;
        default:
            return cancel(io, opt);
    }
}

}  // namespace askmdb::cli
