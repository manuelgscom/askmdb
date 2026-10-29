#pragma once
// Mode state machine: show, ask, copy or execute, depending on the mode and the verifier's level.

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "cli/i18n.hpp"
#include "config/config.hpp"
#include "infer/engine.hpp"
#include "verify/verify.hpp"

namespace askmdb::cli {

// Exit codes.
inline constexpr int kExitOk = 0;
inline constexpr int kExitError = 1;        // askmdb: config, model, invalid response, cache, disk
inline constexpr int kExitUsage = 2;        // usage: flags, question, database, schema that does not fit
inline constexpr int kExitBlocked = 3;      // blocked by the verifier, or admin/local/config
inline constexpr int kExitMongo = 4;        // MongoDB: connection, auth, maxTimeMS, server error
inline constexpr int kExitInterrupted = 130;

// Everything that touches the outside world, injectable for tests.
struct Io {
    std::function<void(std::string_view)> write;
    std::function<std::optional<std::string>()> read_line;  // without "\n"; nullopt = EOF
    std::function<verify::Assessment(const infer::Suggestion&)> verify;
    std::function<int(const infer::Suggestion&)> execute;   // runs and prints; askmdb exit code
    std::function<bool(const std::string&)> copy;           // mongosh text to the clipboard
};

struct FlowOptions {
    config::Mode mode = config::Mode::Confirm;
    UiLang lang = UiLang::Es;
    bool color = false;
    bool interactive = true;  // false (stdin or stdout not a TTY) => shows, never executes
};

enum class Answer { Yes, No, Copy, Empty, Invalid };

// Answer to the prompt (case-insensitive, trimmed): s/si/sí/y/yes, n/no, c.
Answer parse_answer(std::string_view line);

// Shows the query (mongosh syntax) of a Query suggestion and acts according to the mode and level.
// Returns askmdb's exit code.
int run_flow(const infer::Suggestion& suggestion, std::string_view mongosh, const FlowOptions& opt, const Io& io);

}  // namespace askmdb::cli
