#include "doctest.h"

#include <string>
#include <vector>

#include "schema/schema.hpp"

using namespace askmdb::schema;

namespace {

Schema from_block(const std::string& b) {
    auto r = parse(b);
    REQUIRE_MESSAGE(r.schema, r.error);
    return *r.schema;
}

const std::string kFull =
    "db=t\n"
    "## a\n"
    "_id oid\n"
    "e str [\"x\",\"y\"]\n"
    "p obj\n"
    "p.q obj\n"
    "p.q.r obj\n"
    "p.q.r.s int\n"
    "p.q.t str [\"m\",\"n\"]\n"
    "## b\n"
    "_id oid\n"
    "f str [\"u\",\"v\"]\n"
    "k obj\n"
    "k.l obj\n"
    "k.l.m obj\n"
    "k.l.m.n int\n";

}  // namespace

TEST_SUITE("schema_budget") {

TEST_CASE("cost: bytes of the collection's block / 3, rounded up") {
    CHECK(cost(Collection{"c", {Path{"a", {"int"}, false, {}}}}) == 4);  // "## c\n" + "a int\n" = 11 B
    CHECK(cost(Collection{"cc", {Path{"a", {"int"}, false, {}}}}) == 4);  // 12 B
    CHECK(cost(Collection{"ccc", {Path{"a", {"int"}, false, {}}}}) == 5); // 13 B
    CHECK(cost(Collection{"c", {}}) == 2);
}

TEST_CASE("a block that fits is the plain render, with the costs of every collection") {
    Schema s = from_block(kFull);
    auto f = render(s, kFull.size());
    CHECK(f.ok);
    CHECK(f.block == kFull);
    REQUIRE(f.costs.size() == 2);
    CHECK(f.costs[0].name == "a");
    CHECK(f.costs[0].tokens == cost(s.collections[0]));
    CHECK(f.costs[1].name == "b");
    CHECK(f.costs[1].tokens == cost(s.collections[1]));
}

TEST_CASE("trimming order: values last to first, then depth 4, 3, 2 last to first; never top level") {
    Schema s = from_block(kFull);
    std::vector<std::string> steps = {
        // 1. Value lists: last collection's last path first.
        "db=t\n## a\n_id oid\ne str [\"x\",\"y\"]\np obj\np.q obj\np.q.r obj\np.q.r.s int\np.q.t str [\"m\",\"n\"]\n"
        "## b\n_id oid\nf str\nk obj\nk.l obj\nk.l.m obj\nk.l.m.n int\n",
        "db=t\n## a\n_id oid\ne str [\"x\",\"y\"]\np obj\np.q obj\np.q.r obj\np.q.r.s int\np.q.t str\n"
        "## b\n_id oid\nf str\nk obj\nk.l obj\nk.l.m obj\nk.l.m.n int\n",
        "db=t\n## a\n_id oid\ne str\np obj\np.q obj\np.q.r obj\np.q.r.s int\np.q.t str\n"
        "## b\n_id oid\nf str\nk obj\nk.l obj\nk.l.m obj\nk.l.m.n int\n",
        // 2. Depth 4.
        "db=t\n## a\n_id oid\ne str\np obj\np.q obj\np.q.r obj\np.q.r.s int\np.q.t str\n"
        "## b\n_id oid\nf str\nk obj\nk.l obj\nk.l.m obj\n",
        "db=t\n## a\n_id oid\ne str\np obj\np.q obj\np.q.r obj\np.q.t str\n"
        "## b\n_id oid\nf str\nk obj\nk.l obj\nk.l.m obj\n",
        // Depth 3.
        "db=t\n## a\n_id oid\ne str\np obj\np.q obj\np.q.r obj\np.q.t str\n"
        "## b\n_id oid\nf str\nk obj\nk.l obj\n",
        "db=t\n## a\n_id oid\ne str\np obj\np.q obj\np.q.r obj\n"
        "## b\n_id oid\nf str\nk obj\nk.l obj\n",
        "db=t\n## a\n_id oid\ne str\np obj\np.q obj\n"
        "## b\n_id oid\nf str\nk obj\nk.l obj\n",
        // Depth 2.
        "db=t\n## a\n_id oid\ne str\np obj\np.q obj\n"
        "## b\n_id oid\nf str\nk obj\n",
        "db=t\n## a\n_id oid\ne str\np obj\n"
        "## b\n_id oid\nf str\nk obj\n",
    };
    for (std::size_t i = 0; i < steps.size(); ++i) {
        CAPTURE(i);
        auto f = render(s, steps[i].size());
        CHECK(f.ok);
        CHECK(f.block == steps[i]);
        CHECK(f.block.size() <= steps[i].size());
        // One byte less goes to the next step (or fails after the last one).
        auto g = render(s, steps[i].size() - 1);
        if (i + 1 < steps.size()) CHECK(g.block == steps[i + 1]);
    }
    CHECK(render(s, kFull.size() - 1).block == steps[0]);
}

TEST_CASE("if even the top-level paths do not fit: not ok, no block, costs of the untrimmed schema") {
    Schema s = from_block(kFull);
    std::string minimal = "db=t\n## a\n_id oid\ne str\np obj\n## b\n_id oid\nf str\nk obj\n";
    auto f = render(s, minimal.size() - 1);
    CHECK_FALSE(f.ok);
    CHECK(f.block.empty());
    REQUIRE(f.costs.size() == 2);
    CHECK(f.costs[0].tokens == cost(s.collections[0]));
    CHECK(f.costs[1].tokens == cost(s.collections[1]));
    CHECK(f.costs[0].tokens == (kFull.find("## b") - 5 + 2) / 3);
    CHECK_FALSE(render(s, 0).ok);
}

TEST_CASE("paths deeper than 4 (from a block file) go first") {
    Schema s = from_block("db=t\n## a\na obj\na.b.c.d.e int\na.b obj\n");
    auto f = render(s, std::string("db=t\n## a\na obj\na.b obj\n").size());
    CHECK(f.ok);
    CHECK(f.block == "db=t\n## a\na obj\na.b obj\n");
}

TEST_CASE("arrays: items[] is top level and items[].sku is depth 2") {
    Schema s = from_block("db=t\n## a\nitems[] obj\nitems[].sku str\n");
    auto f = render(s, std::string("db=t\n## a\nitems[] obj\n").size());
    CHECK(f.ok);
    CHECK(f.block == "db=t\n## a\nitems[] obj\n");
    CHECK_FALSE(render(s, f.block.size() - 1).ok);
}

}
