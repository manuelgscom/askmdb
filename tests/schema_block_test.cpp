#include "doctest.h"

#include "schema/schema.hpp"

using namespace askmdb::schema;

TEST_SUITE("schema_block") {

TEST_CASE("render(parse(b)) == b for the contract example") {
    const std::string b =
        "db=tienda\n"
        "## clientes\n"
        "_id str\n"
        "nombre str\n"
        "alta date\n"
        "vip bool\n"
        "## pedidos\n"
        "_id oid\n"
        "fecha date\n"
        "importe double\n"
        "estado str [\"entregado\",\"pendiente\",\"cancelado\"]\n"
        "cliente.ciudad str [\"Madrid\",\"Barcelona\",\"Valencia\",\"Sevilla\"]\n"
        "items[] obj\n"
        "items[].sku str\n"
        "codigo int|str\n"
        "cupon? str\n"
        "a field [x] str [\"y [z]\",\"ñ \\\"q\\\"\"]\n";
    auto r = parse(b);
    REQUIRE_MESSAGE(r.schema, r.error);
    CHECK(r.schema->db == "tienda");
    REQUIRE(r.schema->collections.size() == 2);
    const auto& p = r.schema->collections[1].paths;
    CHECK(p[8].path == "cupon");
    CHECK(p[8].optional);
    CHECK(p[7].types == std::vector<std::string>{"int", "str"});
    CHECK(p[9].path == "a field [x]");
    CHECK(p[9].values == std::vector<std::string>{"y [z]", "ñ \"q\""});
    CHECK(render(*r.schema) == b);
}

TEST_CASE("parse rejects malformed blocks with the line number") {
    CHECK(parse("db=x\n_id oid\n").error == "line 2: path before any collection");
    CHECK(parse("db=x\n## b\n## a\n").error == "line 3: collections must be in alphabetical order");
    CHECK(parse("db=x\n## a\n_id objectid\n").error == "line 3: expected '<path>[?] <types> [values]'");
    CHECK(parse("x\n").error == "line 1: expected db=<name>");
    CHECK(parse("db=x").error == "line 1: the block must end with a newline");
}

}
