#pragma once
// Helpers shared by the files of the schema module (not part of its interface).

#include <string>

#include "schema/schema.hpp"

namespace askmdb::schema::detail {

// One line of the block: "<path>[?] <types>[ <values>]\n".
std::string render_line(const Path& p);

// Number of segments of a path: "a" 1, "a.b" 2, "items[]" 1, "items[].sku" 2.
inline int path_depth(const std::string& path) {
    int d = 1;
    for (char c : path)
        if (c == '.') ++d;
    return d;
}

}  // namespace askmdb::schema::detail
