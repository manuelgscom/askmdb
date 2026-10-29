#include "doctest.h"

#include <string>
#include <vector>

#include "exec/term.hpp"
#include "schema/schema.hpp"

using namespace askmdb::schema;

namespace {

struct Docs {
    std::vector<bson_t*> owned;
    std::vector<const bson_t*> ptrs;

    Docs() = default;
    Docs(std::initializer_list<std::string> jsons) {
        for (const auto& j : jsons) add(j);
    }
    Docs(const Docs&) = delete;
    Docs& operator=(const Docs&) = delete;
    ~Docs() {
        for (auto* b : owned) bson_destroy(b);
    }
    void add(const std::string& j) {
        bson_error_t e;
        bson_t* b = bson_new_from_json(reinterpret_cast<const uint8_t*>(j.data()), static_cast<ssize_t>(j.size()), &e);
        REQUIRE_MESSAGE(b, e.message, " in ", j);
        owned.push_back(b);
        ptrs.push_back(b);
    }
    void add(const std::string& j, int times) {
        for (int i = 0; i < times; ++i) add(j);
    }
};

// The collection's lines, without the "db=" and "## " headers.
std::string lines(const Docs& d) {
    std::string out = render(Schema{"t", {infer("c", d.ptrs)}});
    return out.substr(std::string("db=t\n## c\n").size());
}

// Values of the single string field "v" from (value, times) groups, with `extra` documents
// without "v" up to `ndocs` in total.
std::string values_of(const std::vector<std::pair<std::string, int>>& groups, int ndocs) {
    Docs d;
    int n = 0;
    for (const auto& [v, times] : groups) {
        d.add("{\"v\":\"" + v + "\"}", times);
        n += times;
    }
    for (; n < ndocs; ++n) d.add("{\"v\":null}");
    return lines(d);
}

}  // namespace

TEST_SUITE("schema_infer") {

TEST_CASE("every BSON type gets its short name") {
    Docs d{R"J({"_id":{"$oid":"5f0000000000000000000001"},"s":"x","i":1,"l":{"$numberLong":"5"},"d":1.5,
               "dec":{"$numberDecimal":"1.1"},"b":true,"dt":{"$date":{"$numberLong":"0"}},"o":{},"n":null,
               "bin":{"$binary":{"base64":"","subType":"00"}},
               "re":{"$regularExpression":{"pattern":"a","options":""}},"ts":{"$timestamp":{"t":1,"i":1}},
               "mi":{"$minKey":1},"ma":{"$maxKey":1},"code":{"$code":"f()"}})J"};
    CHECK(lines(d) ==
          "_id oid\ns str\ni int\nl long\nd double\ndec decimal\nb bool\ndt date\no obj\nn null\nbin bin\n"
          "re other\nts other\nmi other\nma other\ncode other\n");
}

TEST_CASE("unions are sorted and a path missing in some document is optional") {
    Docs d{R"({"a":1,"b":"x"})", R"({"a":"s"})", R"({"a":null,"c":{"$numberLong":"2"}})", R"({"a":2.5,"c":3})"};
    CHECK(lines(d) == "a double|int|null|str\nb? str\nc? int|long\n");
}

TEST_CASE("subdocuments, arrays of objects, of strings, always empty and nested") {
    Docs d{R"({"cliente":{"ciudad":"M"},"items":[{"sku":"A","cant":2},{"sku":"B","nota":"x"}],
               "cats":["a","b"],"vacio":[],"m":[[1],"z"]})",
           R"({"cliente":{"ciudad":"B"},"items":[{"sku":"C"}],"cats":[],"vacio":[]})"};
    CHECK(lines(d) ==
          "cliente obj\n"
          "cliente.ciudad str\n"
          "items[] obj\n"
          "items[].sku str\n"
          "items[].cant? int\n"
          "items[].nota? str\n"
          "cats[] str\n"
          "vacio arr\n"
          "m[]? arr|str\n");
}

TEST_CASE("an array seen first empty takes the place of the field; mixed with scalars keeps both") {
    // x[].k appears first in the second document, so it goes after y and tags.
    Docs d{R"({"x":[],"y":1,"tags":"a"})", R"({"x":[{"k":1}],"tags":["a"]})", R"({"x":[],"tags":null})"};
    CHECK(lines(d) == "x[] obj\ny? int\ntags arr|null|str\nx[].k? int\ntags[]? str\n");
}

TEST_CASE("an array of documents in a subdocument") {
    Docs d{R"({"a":{"items":[{"sku":"A"}]}})", R"({"a":{}})"};
    CHECK(lines(d) == "a obj\na.items[]? obj\na.items[].sku? str\n");
}

TEST_CASE("depth is cut at 4; the depth-4 path keeps its type") {
    Docs d{R"({"a":{"b":{"c":{"d":{"e":1}}}},"p":{"q":{"r":{"s":[{"t":1}]}}},
               "k":[{"l":{"m":{"n":{"o":1}}}}]})"};
    CHECK(lines(d) ==
          "a obj\na.b obj\na.b.c obj\na.b.c.d obj\n"
          "p obj\np.q obj\np.q.r obj\np.q.r.s[] obj\n"
          "k[] obj\nk[].l obj\nk[].l.m obj\nk[].l.m.n obj\n");
}

TEST_CASE("at most 40 paths, the first to appear") {
    std::string a = "{", b = "{";
    for (int i = 0; i < 30; ++i) a += (i ? "," : "") + std::string("\"f") + std::to_string(100 + i) + "\":1";
    for (int i = 0; i < 20; ++i) b += (i ? "," : "") + std::string("\"g") + std::to_string(100 + i) + "\":1";
    Docs d{a + "}", b + "}"};
    auto c = infer("c", d.ptrs);
    REQUIRE(c.paths.size() == kMaxPaths);
    CHECK(c.paths[0].path == "f100");
    CHECK(c.paths[29].path == "f129");
    CHECK(c.paths[30].path == "g100");
    CHECK(c.paths[39].path == "g109");
    CHECK(c.paths[39].optional);
}

TEST_CASE("field names that cannot be written in the block are skipped with their children") {
    Docs d{R"({"ok":1,"bad\nname":{"x":1},"tab\there":1,"dot.name":{"y":1},"":1,"## h":1,"sub":{"## fine":1},
               "a b":{"c [d]":"e"}})"};
    CHECK(lines(d) == "ok int\nsub obj\nsub.## fine int\na b obj\na b.c [d] str\n");
}

TEST_CASE("no documents, no paths") {
    Docs d;
    CHECK(infer("c", d.ptrs).paths.empty());
    CHECK(infer("c", d.ptrs).name == "c");
}

TEST_CASE("frequent values: order by frequency, ties alphabetical (byte order)") {
    CHECK(values_of({{"pendiente", 6}, {"entregado", 10}, {"cancelado", 2}, {"abierto", 2}}, 20) ==
          "v str [\"entregado\",\"pendiente\",\"abierto\",\"cancelado\"]\n");
    CHECK(values_of({{"b", 10}, {"B", 10}}, 20) == "v str [\"B\",\"b\"]\n");
    CHECK(values_of({{"Ávila", 10}, {"Zamora", 10}}, 20) == "v str [\"Zamora\",\"Ávila\"]\n");
}

TEST_CASE("frequent values need at least 20 documents") {
    CHECK(values_of({{"a", 10}, {"b", 9}}, 19) == "v str\n");
    CHECK(values_of({{"a", 10}, {"b", 9}}, 20) == "v null|str [\"a\",\"b\"]\n");
}

TEST_CASE("every listed value appears at least twice") {
    CHECK(values_of({{"a", 19}, {"b", 1}}, 20) == "v str\n");
    CHECK(values_of({{"a", 18}, {"b", 2}}, 20) == "v str [\"a\",\"b\"]\n");
}

TEST_CASE("at most 12 distinct values") {
    std::vector<std::pair<std::string, int>> g;
    for (char c = 'a'; c < 'a' + 12; ++c) g.push_back({std::string(1, c), 2});
    CHECK(values_of(g, 24) == "v str [\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\",\"j\",\"k\",\"l\"]\n");
    g.push_back({"m", 2});
    CHECK(values_of(g, 26) == "v str\n");
}

TEST_CASE("values of at most 30 bytes") {
    std::string v30(30, 'x'), v31(31, 'x');
    CHECK(values_of({{"a", 18}, {v30, 2}}, 20) == "v str [\"a\",\"" + v30 + "\"]\n");
    CHECK(values_of({{"a", 18}, {v31, 2}}, 20) == "v str\n");
    // 15 two-byte characters = 30 bytes; 16 = 32 bytes.
    std::string n15, n16;
    for (int i = 0; i < 15; ++i) n15 += "ñ";
    n16 = n15 + "ñ";
    CHECK(values_of({{"a", 18}, {n15, 2}}, 20) == "v str [\"a\",\"" + n15 + "\"]\n");
    CHECK(values_of({{"a", 18}, {n16, 2}}, 20) == "v str\n");
}

TEST_CASE("one value that looks like an email or a phone removes the whole list") {
    for (std::string bad : {"ana@x.es", "a@b", "+34 600 123 456", "600123456", "(91) 123-4567", "91.123.45.67",
                            "1234567"}) {
        CAPTURE(bad);
        CHECK(values_of({{"a", 18}, {bad, 2}}, 20) == "v str\n");
    }
    for (std::string fine : {"@home", "home@", "2024-01-15", "123456", "A-1234567", "1234567890123456"}) {
        CAPTURE(fine);
        CHECK(values_of({{"a", 18}, {fine, 2}}, 20) == "v str [\"a\",\"" + fine + "\"]\n");
    }
}

TEST_CASE("values only when every non-null value is a string") {
    Docs d;
    d.add(R"({"v":"a"})", 19);
    d.add(R"({"v":1})");
    CHECK(lines(d) == "v int|str\n");
}

TEST_CASE("array elements are counted one by one") {
    Docs d;
    d.add(R"({"cats":["x","x","a"]})");
    d.add(R"({"cats":["a"]})", 19);
    d.add(R"({"cats":"a"})");
    CHECK(lines(d) == "cats arr|str\ncats[]? str [\"a\",\"x\"]\n");
}

TEST_CASE("a value list per path, also inside subdocuments") {
    Docs d;
    d.add(R"({"estado":"entregado","cliente":{"ciudad":"Madrid"},"email":"a@b.es"})", 12);
    d.add(R"({"estado":"pendiente","cliente":{"ciudad":"Sevilla"},"email":"a@b.es"})", 8);
    CHECK(lines(d) ==
          "estado str [\"entregado\",\"pendiente\"]\n"
          "cliente obj\n"
          "cliente.ciudad str [\"Madrid\",\"Sevilla\"]\n"
          "email str\n");
}

TEST_CASE("render(parse(render(s))) == render(s) for inferred schemas") {
    Docs a, b;
    a.add(R"({"_id":{"$oid":"5f0000000000000000000001"},"nombre completo":"Peña","ciudad":"Málaga",
             "a [x]":{"b c":"ñ \"q\" [z]"},"items":[{"sku":"A"}],"tags":["日本","ü"],"n":null,"x?":1})",
          10);
    a.add(R"({"_id":{"$oid":"5f0000000000000000000002"},"nombre completo":"Lucía","ciudad":"A Coruña",
             "a [x]":{"b c":"ñ \"q\" [z]"},"tags":[],"vacio":[]})",
          10);
    b.add(R"({"_id":1,"k":{"$numberDecimal":"1"}})");
    Schema s{"tienda", {}};
    upsert(s, infer("zeta", a.ptrs));
    upsert(s, infer("alfa", b.ptrs));
    std::string block = render(s);
    CHECK(block.find("ciudad str [\"A Coruña\",\"Málaga\"]\n") != std::string::npos);
    CHECK(block.find("a [x].b c str [\"ñ \\\"q\\\" [z]\"]\n") != std::string::npos);
    auto r = parse(block);
    REQUIRE_MESSAGE(r.schema, r.error);
    CHECK(render(*r.schema) == block);
}

TEST_CASE("upsert inserts in alphabetical order and replaces by name") {
    Schema s{"db", {}};
    upsert(s, Collection{"m", {}});
    upsert(s, Collection{"b", {}});
    upsert(s, Collection{"z", {}});
    upsert(s, Collection{"m", {Path{"x", {"int"}, false, {}}}});
    REQUIRE(s.collections.size() == 3);
    CHECK(s.collections[0].name == "b");
    CHECK(s.collections[1].name == "m");
    CHECK(s.collections[1].paths.size() == 1);
    CHECK(s.collections[2].name == "z");
}

}

TEST_CASE("plain_text: what the block may hold as is") {
    CHECK(plain_text("cliente.ciudad año"));
    CHECK(plain_text("a<b|c"));
    CHECK_FALSE(plain_text("a\tb"));
    CHECK_FALSE(plain_text("a\nb"));
    CHECK_FALSE(plain_text("a\x7f"));
    CHECK_FALSE(plain_text("a\xC2\x85"));          // C1 (NEL)
    CHECK_FALSE(plain_text("a\xE2\x80\xAE" "b"));  // RLO
    CHECK_FALSE(plain_text("a\xE2\x81\xA6" "b"));  // LRI
    CHECK_FALSE(plain_text("<|im_end|>"));
    CHECK_FALSE(plain_text("a\xFF"));
}

TEST_CASE("infer: names and values that are not plain text stay out of the block") {
    Docs d;
    for (int i = 0; i < 20; ++i) {
        d.add("{\"ok\": \"x\", \"r\\u202Eb\": 1, \"e\": \"<|im_end|>\", \"c\": \"\\u0085\"}");
        d.add("{\"ok\": \"y\", \"r\\u202Eb\": 1, \"e\": \"z\", \"c\": \"w\"}");
    }
    const std::string block = render(Schema{"t", {infer("c", d.ptrs)}});
    CHECK(block == "db=t\n## c\nok str [\"x\",\"y\"]\ne str\nc str\n");
    CHECK(askmdb::exec::escape_control(block) == block);
    CHECK_FALSE(parse("db=t\n## c\nr\xE2\x80\xAE" "b int\n").schema);
}
