#pragma once
// Terminal-safe text: control characters (except \n and \t) as \xNN, and C1 controls and bidi
// overrides/isolates as \u{XXXX}, so that text coming from the model or the server cannot move
// the cursor, change the terminal or reorder what is shown.

#include <string>
#include <string_view>

namespace askmdb::exec {

std::string escape_control(std::string_view text);

}  // namespace askmdb::exec
