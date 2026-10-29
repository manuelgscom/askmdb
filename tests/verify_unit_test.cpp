// Unit tests of the verifier: JSON scanner, loading, text checks, limits, reasons.
#include <string>

#include "doctest.h"
#include "verify/internal.hpp"
#include "verify/verify.hpp"

using namespace askmdb;
using verify::Level;

namespace {

const schema::Schema& tienda() {
    static const schema::Schema s = [] {
        auto r = schema::parse(
            "db=tienda\n## pedidos\n_id oid\nfecha date\nimporte double\nestado str\ncliente.ciudad str\n"
            "items[] obj\nitems[].sku str\n");
        REQUIRE(r.schema);
        return *r.schema;
    }();
    return s;
}

verify::Assessment run(std::string_view text, std::string_view coll = "pedidos", std::string_view db = "tienda") {
    return verify::verify(verify::load_pipeline(text), coll, db, tienda());
}

bool has(const verify::Assessment& a, std::string_view id) {
    for (const auto& r : a.reasons)
        if (r.id == id) return true;
    return false;
}

std::string nested(int depth) {  // a pipeline whose deepest container is at `depth` (top array = 1)
    bool even = depth % 2 == 0;
    std::string mid = even ? R"({"estado":{"$eq":"x"}})" : R"({"estado":"x"})";
    for (int d = even ? 6 : 5; d < depth; d += 2) mid = "{\"$and\":[" + mid + "]}";
    return "[{\"$match\":{\"$and\":[" + mid + "]}}]";
}

}  // namespace

TEST_SUITE("verify_unit") {

TEST_CASE("raw_member returns the original text of the member") {
    auto r = verify::raw_member(R"({"db":"humo", "pipeline" : [ {"$match":{"a":1.50}} ] ,"x":null})", "pipeline");
    REQUIRE(r);
    CHECK(*r == R"([ {"$match":{"a":1.50}} ])");
    CHECK(verify::raw_member(R"({"db":"humo"})", "db") == std::optional<std::string>("\"humo\""));
    CHECK(verify::raw_member(R"(  {"a":{"b":[1,2,{"c":"}"}]}}  )", "a") ==
          std::optional<std::string>(R"({"b":[1,2,{"c":"}"}]})"));
}

TEST_CASE("raw_member rejects repeated, missing or escaped-duplicate keys and malformed input") {
    CHECK_FALSE(verify::raw_member(R"({"pipeline":[],"pipeline":[{"$out":"x"}]})", "pipeline"));
    CHECK_FALSE(verify::raw_member(R"({"pipeline":[],"pipeline":[{"$out":"x"}]})", "pipeline"));
    CHECK_FALSE(verify::raw_member(R"({"db":"humo"})", "pipeline"));
    CHECK_FALSE(verify::raw_member(R"({"pipeline":[})", "pipeline"));
    CHECK_FALSE(verify::raw_member(R"({"pipeline":[]} x)", "pipeline"));
    CHECK_FALSE(verify::raw_member(R"([{"pipeline":[]}])", "pipeline"));
    CHECK_FALSE(verify::raw_member(R"({"pipeline":[],})", "pipeline"));
    CHECK_FALSE(verify::raw_member(R"({"pipeline":tru})", "pipeline"));
    CHECK_FALSE(verify::raw_member("{\"pipeline\":\"a\tb\"}", "pipeline"));
    CHECK_FALSE(verify::raw_member("", "pipeline"));
    CHECK_FALSE(verify::raw_member(std::string(2000, '[') + std::string(2000, ']'), "pipeline"));
    CHECK(verify::raw_member(R"({"pipeline":[1]})", "pipeline") == std::optional<std::string>("[1]"));
}

TEST_CASE("json_string decodes escapes") {
    CHECK(verify::json_string(R"("año\n\"")") == std::optional<std::string>("año\n\""));
    CHECK(verify::json_string(R"("😀")") == std::optional<std::string>("\xF0\x9F\x98\x80"));
    CHECK_FALSE(verify::json_string(R"("\ud83d")"));
    CHECK_FALSE(verify::json_string(R"("abc)"));
    CHECK_FALSE(verify::json_string("12"));
}

TEST_CASE("load_pipeline keeps the text and reports libbson errors") {
    auto p = verify::load_pipeline(R"([{"$limit":1}])");
    CHECK(p.text == R"([{"$limit":1}])");
    CHECK(p.bson);
    auto bad = verify::load_pipeline(R"([{"$limit":1])");
    CHECK_FALSE(bad.bson);
    CHECK_FALSE(bad.parse_error.empty());
    auto a = verify::verify(bad, "pedidos", "tienda", tienda());
    CHECK(a.level == Level::Blocked);
    CHECK(has(a, "B-PARSE"));
}

TEST_CASE("a JSON object that libbson reads like an array is blocked") {
    auto a = run(R"({"0":{"$match":{"estado":"x"}}})");
    CHECK(a.level == Level::Blocked);
    CHECK(has(a, "B-NOT-ARRAY"));
    CHECK(run(R"(  [{"$match":{"estado":"x"}}])").level == Level::Read);
}

TEST_CASE("text checks: size, control characters, UTF-8") {
    std::string big = R"([{"$match":{"estado":")" + std::string(verify::kMaxPipelineBytes, 'a') + R"("}}])";
    CHECK(has(run(big), "B-TOO-LONG"));
    CHECK(has(run("[{\"$match\":{\"estado\":\"a\x01\"}}]"), "B-CONTROL-CHAR"));
    CHECK(has(run("[{\"$match\":{\"estado\":\"a\x7f\"}}]"), "B-CONTROL-CHAR"));
    CHECK(has(run(R"([{"$match":{"estado":"a\u0000b"}}])"), "B-CONTROL-CHAR"));
    CHECK(has(run(R"([{"$match":{"estado":"a\u001bb"}}])"), "B-CONTROL-CHAR"));
    CHECK(has(run("[{\"$match\":{\"estado\":\"\xc3\x28\"}}]"), "B-UTF8"));
    CHECK(run("[{\"$match\":\r\n\t{\"estado\":\"a\\nb\"}}]").level == Level::Read);
    CHECK(run("[{\"$match\":{\"estado\":\"España ñ\"}}]").level == Level::Read);
}

TEST_CASE("utf-8 validator") {
    using verify::detail::valid_utf8;
    CHECK(valid_utf8("año €"));
    CHECK(valid_utf8("\xF0\x9F\x98\x80"));
    CHECK_FALSE(valid_utf8("\xC0\xAF"));          // overlong
    CHECK_FALSE(valid_utf8("\xED\xA0\x80"));      // surrogate
    CHECK_FALSE(valid_utf8("\xF4\x90\x80\x80"));  // > U+10FFFF
    CHECK_FALSE(valid_utf8("\xE2\x82"));          // truncated
}

TEST_CASE("nested quantifiers") {
    using verify::detail::has_nested_quantifier;
    CHECK(has_nested_quantifier("(a+)+"));
    CHECK(has_nested_quantifier("(.*)*"));
    CHECK(has_nested_quantifier("^(x(ab*)c)+$"));
    CHECK(has_nested_quantifier("(a{2,})*"));
    CHECK(has_nested_quantifier("((ab)+)+"));
    CHECK_FALSE(has_nested_quantifier("^Mad"));
    CHECK_FALSE(has_nested_quantifier("(ab)+"));
    CHECK(has_nested_quantifier("(a|b)*c+"));  // quantified alternation
    CHECK(has_nested_quantifier("(a+)\\1*"));
    CHECK(has_nested_quantifier("(?<n>a)\\k<n>+"));
    CHECK_FALSE(has_nested_quantifier("^(Madrid|Bilbao)$"));
    CHECK_FALSE(has_nested_quantifier("[(a+)]+"));
    CHECK_FALSE(has_nested_quantifier("\\(a+\\)+"));
    CHECK_FALSE(has_nested_quantifier("(?:abc)?"));
}

TEST_CASE("regexes that cannot be analysed, and inline flags") {
    using verify::detail::analyze_regex;
    CHECK(analyze_regex("(?x)a").complex);
    CHECK(analyze_regex("(?i)a").complex);
    CHECK_FALSE(analyze_regex("(?i)a").bad_flags);
    CHECK(analyze_regex("(?U)a").bad_flags);
    CHECK(analyze_regex("(?-J:a)").bad_flags);
    CHECK(analyze_regex("(*LIMIT_HEAP=1)a").bad_flags);
    CHECK(analyze_regex("a\\Q)+\\E").complex);
    CHECK(analyze_regex("a\\c)").complex);
    CHECK(analyze_regex("a(?#c)").complex);
    auto plain = analyze_regex("(?:ab)(?=c)(?<n>d)[(?x)]");
    CHECK_FALSE(plain.complex);
    CHECK_FALSE(plain.bad_flags);
    CHECK_FALSE(plain.nested);
}

TEST_CASE("C1 and bidi controls count as control characters") {
    using verify::detail::has_control;
    CHECK(has_control("a\xC2\x9B"));
    CHECK(has_control("a\xE2\x80\xAE"));
    CHECK(has_control("a\xE2\x81\xA6"));
    CHECK_FALSE(has_control("a\xC2\xA0 \xE2\x80\x94"));  // nbsp, em dash
    CHECK(has(run(R"([{"$match":{"estado":"a\u009bb"}}])"), "B-CONTROL-CHAR"));
    CHECK(has(run(R"([{"$match":{"estado\u202e":"a"}}])"), "B-CONTROL-CHAR"));
}

TEST_CASE("duplicate keys are found in the text, also inside Extended JSON wrappers") {
    using verify::detail::has_duplicate_keys;
    CHECK(has_duplicate_keys(R"([{"a":{"$oid":"x","$oid":"y"}}])"));
    CHECK(has_duplicate_keys(R"([{"a":{"b":1,"\u0062":2}}])"));
    CHECK(has_duplicate_keys(R"([{"a":NaN,"a":1}])"));
    CHECK_FALSE(has_duplicate_keys(R"([{"a":{"a":1}},{"a":2}])"));
    CHECK(has(run(R"([{"$match":{"estado":{"$regex":"a","$options":"","$regex":"(a+)+"}}}])"), "B-DUP-KEY"));
}

TEST_CASE("variables: names, $$ROOT/$$CURRENT paths and $getField") {
    CHECK(run(R"([{"$project":{"x":{"$map":{"input":"$items","as":"it_2","in":"$$it_2.sku"}}}}])").level == Level::Read);
    CHECK(has(run(R"([{"$project":{"x":{"$map":{"input":"$items","as":"2it","in":1}}}}])"), "B-BAD-VARIABLE"));
    CHECK(has(run(R"([{"$project":{"x":{"$let":{"vars":{"NOW":1},"in":1}}}}])"), "B-BAD-VARIABLE"));
    CHECK(run(R"([{"$project":{"x":"$$ROOT.cliente.ciudad","y":"$$CURRENT.estado"}}])").level == Level::Read);
    CHECK(has(run(R"([{"$project":{"x":"$$ROOT.zz"}}])"), "W-UNKNOWN-FIELD"));
    CHECK(run(R"([{"$project":{"x":{"$getField":{"field":"zz","input":"$cliente"}}}}])").level == Level::Read);
    CHECK(has(run(R"([{"$project":{"x":{"$getField":{"field":"zz","input":"$$ROOT"}}}}])"), "W-UNKNOWN-FIELD"));
    CHECK(run(R"([{"$project":{"x":{"$getField":"estado"}}}])").level == Level::Read);
}

TEST_CASE("nesting: 16 levels is fine, 17 is blocked") {
    CHECK(run(nested(16)).level == Level::Read);
    auto a = run(nested(17));
    CHECK(a.level == Level::Blocked);
    CHECK(has(a, "B-TOO-DEEP"));
}

TEST_CASE("20 stages per level is fine, 21 is blocked") {
    std::string p20 = "[", p21;
    for (int i = 0; i < 20; ++i) p20 += std::string(i ? "," : "") + R"({"$limit":5})";
    p21 = p20 + R"(,{"$limit":5}])";
    p20 += "]";
    CHECK(run(p20).level == Level::Read);
    CHECK(has(run(p21), "B-TOO-MANY-STAGES"));
    std::string facet = R"([{"$facet":{"a":)" + p21 + "}}]";
    CHECK(has(run(facet), "B-TOO-MANY-STAGES"));
}

TEST_CASE("reasons: only the final level, discovery order, no repeated id") {
    auto a = run(R"([{"$match":{"zz":1}},{"$out":"a"},{"$sample":{"size":1}},{"$merge":"b"},{"$documents":[]}])");
    CHECK(a.level == Level::Blocked);
    REQUIRE(a.reasons.size() == 2);
    CHECK(a.reasons[0].id == "B-WRITE-STAGE");
    CHECK(a.reasons[1].id == "B-SERVER-STAGE");
    auto w = run(R"([{"$match":{"zz":1}},{"$sample":{"size":1}},{"$match":{"yy":1}}])");
    CHECK(w.level == Level::Warn);
    REQUIRE(w.reasons.size() == 2);
    CHECK(w.reasons[0].id == "W-UNKNOWN-FIELD");
    CHECK(w.reasons[1].id == "W-SAMPLE");
    CHECK(run(R"([{"$match":{"estado":"x"}}])").reasons.empty());
}

TEST_CASE("every reason has both texts") {
    for (const char* id : {"B-PARSE", "B-JS", "W-JOIN", "W-UNKNOWN-FIELD", "B-INPUT", "B-SYSTEM-DB"}) {
        auto r = verify::detail::make_reason(id, "x");
        CHECK(r.es != r.en);
        CHECK(r.es.ends_with(": x"));
    }
    auto r = verify::detail::make_reason("B-UNKNOWN-OPERATOR", std::string(100, 'a') + "\x01");
    CHECK(r.en.ends_with("..."));
}

TEST_CASE("check_db blocks the system databases, in any case") {
    CHECK(verify::check_db("admin"));
    CHECK(verify::check_db("local"));
    CHECK(verify::check_db("config"));
    CHECK(verify::check_db("Admin"));
    CHECK(verify::check_db("admin")->id == "B-SYSTEM-DB");
    CHECK_FALSE(verify::check_db("humo"));
    CHECK_FALSE(verify::check_db("administracion"));
    for (const char* bad : {"", "a/b", "a\\b", "a.b", "a b", "a\"b", "$external"})
        CHECK(verify::check_db(bad)->id == "B-BAD-DB");
    CHECK(verify::check_db(std::string_view("admin\0", 6))->id == "B-BAD-DB");
    CHECK(verify::check_db(std::string(64, 'a'))->id == "B-BAD-DB");
    CHECK_FALSE(verify::check_db(std::string(63, 'a')));
    CHECK(run(R"([{"$limit":1}])", "pedidos", "admin").level == Level::Blocked);
}

TEST_CASE("a schema of another database means unknown collections") {
    auto a = run(R"([{"$limit":1}])", "pedidos", "otra");
    CHECK(a.level == Level::Warn);
    CHECK(has(a, "W-UNKNOWN-COLLECTION"));
}

TEST_CASE("the same pipeline always gets the same assessment") {
    const char* p = R"([{"$lookup":{"from":"x","pipeline":[{"$match":{"$expr":{"$eq":["$$v",1]}}}],"as":"y"}}])";
    auto a = run(p), b = run(p);
    CHECK(a.level == b.level);
    REQUIRE(a.reasons.size() == b.reasons.size());
    for (std::size_t i = 0; i < a.reasons.size(); ++i) CHECK(a.reasons[i].id == b.reasons[i].id);
}

}  // TEST_SUITE
