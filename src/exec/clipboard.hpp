#pragma once
// Copy text to the system clipboard: wl-copy, xclip, xsel or OSC 52 as a fallback.

#include <string>

namespace askmdb::exec {

// true if some method accepted the text.
bool copy_to_clipboard(const std::string& text);

}  // namespace askmdb::exec
