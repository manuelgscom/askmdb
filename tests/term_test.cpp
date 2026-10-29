#include "doctest.h"

#include "exec/term.hpp"

using askmdb::exec::escape_control;

TEST_CASE("escape_control keeps text, \\n and \\t, and escapes what could drive the terminal") {
    CHECK(escape_control("Madrid ñ €\n\tx") == "Madrid ñ €\n\tx");
    CHECK(escape_control("a\x1b[2Jb") == "a\\x1b[2Jb");
    CHECK(escape_control("\r\x7f") == "\\x0d\\x7f");
    CHECK(escape_control("a\xc2\x9b[2J") == "a\\u{009b}[2J");          // C1 CSI
    CHECK(escape_control("\xc2\xa0") == "\xc2\xa0");                    // NBSP is not C1
    CHECK(escape_control("a\xe2\x80\xae" "b") == "a\\u{202e}b");          // RIGHT-TO-LEFT OVERRIDE
    CHECK(escape_control("\xe2\x81\xa6x\xe2\x81\xa9") == "\\u{2066}x\\u{2069}");
    CHECK(escape_control("\xe2\x80\x94") == "\xe2\x80\x94");            // em dash untouched
    CHECK(escape_control("\xc2") == "\xc2");                            // truncated sequence: as is
}
