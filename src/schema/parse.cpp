// Inverse of render(): reads a schema block (also the frozen snapshots given with --schema-from).

#include <algorithm>
#include <nlohmann/json.hpp>

#include "schema/schema.hpp"

namespace askmdb::schema {

namespace {

bool is_type(std::string_view t) {
    static constexpr std::string_view kTypes[] = {"oid",  "str",  "int", "long", "double", "decimal", "bool",
                                                  "date", "obj",  "arr", "null", "bin",    "other"};
    return std::find(std::begin(kTypes), std::end(kTypes), t) != std::end(kTypes);
}

// "int|str" -> {"int","str"}; nullopt if any part is not a known type.
std::optional<std::vector<std::string>> parse_types(std::string_view s) {
    std::vector<std::string> types;
    while (true) {
        auto bar = s.find('|');
        auto t = s.substr(0, bar);
        if (!is_type(t)) return std::nullopt;
        types.emplace_back(t);
        if (bar == std::string_view::npos) break;
        s.remove_prefix(bar + 1);
    }
    return types;
}

// "<path>[?] <types>" -> Path (no values).
std::optional<Path> parse_head(std::string_view head) {
    auto sp = head.rfind(' ');
    if (sp == std::string_view::npos || sp == 0) return std::nullopt;
    auto types = parse_types(head.substr(sp + 1));
    if (!types) return std::nullopt;
    Path p;
    std::string_view path = head.substr(0, sp);
    if (path.size() > 1 && path.back() == '?') {
        p.optional = true;
        path.remove_suffix(1);
    }
    p.path = std::string(path);
    p.types = std::move(*types);
    return p;
}

// A path line: "<path>[?] <types>" or "<path>[?] <types> [<JSON strings>]". Field names may contain
// spaces or brackets, so the values are the leftmost " [" whose rest is a JSON array of strings.
std::optional<Path> parse_path_line(std::string_view line) {
    if (!line.empty() && line.back() == ']') {
        for (auto pos = line.find(" ["); pos != std::string_view::npos; pos = line.find(" [", pos + 1)) {
            auto j = nlohmann::json::parse(line.substr(pos + 1), nullptr, false);
            if (j.is_discarded() || !j.is_array() || j.empty()) continue;
            if (!std::all_of(j.begin(), j.end(), [](const auto& v) { return v.is_string(); })) continue;
            auto p = parse_head(line.substr(0, pos));
            if (!p) continue;
            for (const auto& v : j) p->values.push_back(v.get<std::string>());
            return p;
        }
    }
    return parse_head(line);
}

}  // namespace

ParseResult parse(std::string_view block) {
    ParseResult r;
    Schema s;
    Collection* current = nullptr;
    std::size_t n = 0;
    auto fail = [&](std::string why) {
        r.error = "line " + std::to_string(n) + ": " + why;
        return r;
    };
    if (block.empty() || block.back() != '\n') {
        n = 1;
        return fail("the block must end with a newline");
    }
    while (!block.empty()) {
        ++n;
        auto nl = block.find('\n');
        std::string_view line = block.substr(0, nl);
        block.remove_prefix(nl + 1);
        if (!plain_text(line)) return fail("control, bidi or reserved (\"<|\") characters");
        if (n == 1) {
            if (line.substr(0, 3) != "db=" || line.size() == 3) return fail("expected db=<name>");
            s.db = std::string(line.substr(3));
            continue;
        }
        if (line.substr(0, 3) == "## ") {
            if (line.size() == 3) return fail("empty collection name");
            if (!s.collections.empty() && !(s.collections.back().name < line.substr(3)))
                return fail("collections must be in alphabetical order");
            s.collections.push_back(Collection{std::string(line.substr(3)), {}});
            current = &s.collections.back();
            continue;
        }
        if (!current) return fail("path before any collection");
        auto p = parse_path_line(line);
        if (!p) return fail("expected '<path>[?] <types> [values]'");
        current->paths.push_back(std::move(*p));
    }
    r.schema = std::move(s);
    return r;
}

}  // namespace askmdb::schema
