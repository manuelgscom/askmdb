// The block within a byte budget, with a deterministic trimming.
//
// Order across collections: always from the last collection to the first and, inside each one,
// from its last path to its first. Step 1 removes value lists in that order; step 2 removes paths
// of depth 4 (or the deepest there is) in that order, then of depth 3, then of depth 2. Each step
// stops as soon as the block fits. Top-level paths are never removed.

#include <algorithm>

#include "schema/internal.hpp"
#include "schema/schema.hpp"

namespace askmdb::schema {

std::size_t cost(const Collection& collection) {
    std::size_t bytes = 3 + collection.name.size() + 1;  // "## <name>\n"
    for (const auto& p : collection.paths) bytes += detail::render_line(p).size();
    return (bytes + kBytesPerToken - 1) / kBytesPerToken;
}

Fit render(const Schema& schema, std::size_t budget_bytes) {
    Fit fit;
    for (const auto& c : schema.collections) fit.costs.push_back({c.name, cost(c)});

    std::string full = render(schema);
    if (full.size() <= budget_bytes) {
        fit.ok = true;
        fit.block = std::move(full);
        return fit;
    }

    Schema s = schema;
    std::size_t total = full.size();
    auto done = [&] {
        if (total > budget_bytes) return false;
        fit.ok = true;
        fit.block = render(s);
        return true;
    };

    for (auto c = s.collections.rbegin(); c != s.collections.rend(); ++c) {
        for (auto p = c->paths.rbegin(); p != c->paths.rend(); ++p) {
            if (p->values.empty()) continue;
            std::size_t before = detail::render_line(*p).size();
            p->values.clear();
            total -= before - detail::render_line(*p).size();
            if (done()) return fit;
        }
    }

    int deepest = 1;  // kMaxDepth for inferred schemas; a block from a file may go deeper
    for (const auto& c : s.collections)
        for (const auto& p : c.paths) deepest = std::max(deepest, detail::path_depth(p.path));
    for (int depth = deepest; depth >= 2; --depth) {
        for (auto c = s.collections.rbegin(); c != s.collections.rend(); ++c) {
            for (std::size_t i = c->paths.size(); i-- > 0;) {
                if (detail::path_depth(c->paths[i].path) != depth) continue;
                total -= detail::render_line(c->paths[i]).size();
                c->paths.erase(c->paths.begin() + static_cast<std::ptrdiff_t>(i));
                if (done()) return fit;
            }
        }
    }
    return fit;
}

}  // namespace askmdb::schema
