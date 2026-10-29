#include "doctest.h"

#include "verify/verify.hpp"

using askmdb::verify::load_pipeline;

TEST_CASE("libbson parses a top-level JSON array as an array document") {
    auto p = load_pipeline(R"([{"$match":{"a":1}},{"$limit":2}])");
    REQUIRE(p.bson);
    CHECK(bson_count_keys(p.bson.get()) == 2);
    bson_iter_t it;
    REQUIRE(bson_iter_init_find(&it, p.bson.get(), "1"));
    CHECK(BSON_ITER_HOLDS_DOCUMENT(&it));
}
