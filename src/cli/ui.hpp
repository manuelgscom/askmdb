#pragma once
// Screen rendering. Pure functions: they return the text to write. Text coming from the model or
// the server has its control characters escaped.

#include <string>
#include <string_view>

#include "cli/i18n.hpp"
#include "verify/verify.hpp"

namespace askmdb::cli {

// Color only if stdout is a TTY, NO_COLOR is unset or empty, and TERM is not "dumb".
bool use_color(bool stdout_tty, const char* no_color, const char* term);

// The reason's text in the UI language.
std::string_view reason_text(const verify::Reason& reason, UiLang lang);

// "\n  ❯ <mongosh>\n    <explanation>\n\n". With color, the query gets syntax highlighting (collection
// bold, $operators cyan, stages bold cyan, strings and regexes green, "$field" paths and "$$VARS"
// magenta, numbers and true/false/null yellow) and the explanation is dimmed; without color it
// is plain. Continuation lines are indented by 4 spaces.
std::string render_suggestion(std::string_view mongosh, std::string_view explanation, bool color);

// Level line ("● solo lectura", "▲ aviso" or "■ bloqueada") with its reasons. Blocked also says
// that it will not be run. With ask (never for blocked), it ends with the prompt (no newline).
std::string render_level(const verify::Assessment& a, UiLang lang, bool color, bool ask);

// The prompt alone, to ask again after an invalid answer. Read: [S/n/c]; Warn: [s/N/c].
std::string render_prompt(verify::Level level, UiLang lang);

// "\n  ✗ No puedo: …\n\n" / "\n  ? No lo sé: …\n\n".
std::string render_refusal(bool unsure, std::string_view explanation, UiLang lang, bool color);

// "  ✓ <text>\n" (green ✓) if ok; otherwise "  ✗ <text>\n" (dimmed ✗).
std::string render_status(bool ok, std::string_view text, bool color);

// "\n" + the table (mongo::render_table, its first line bold with color) + "  (hay más filas
// (tope N))" when truncated, or "  (sin resultados)" when there are no rows.
std::string render_results(std::string_view table, bool truncated, int max_rows, UiLang lang, bool color);

}  // namespace askmdb::cli
