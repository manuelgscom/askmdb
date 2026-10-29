// Strict parser of the model output (the model contract) and ASKMDB_FAKE_RAW.
#include <bson/bson.h>

#include <cstdlib>
#include <optional>
#include <string>

#include "doctest.h"
#include "infer/fake_engine.hpp"
#include "infer/parser.hpp"

using askmdb::infer::parse_response;
using askmdb::infer::ParseResult;
using askmdb::infer::Suggestion;

namespace {

// Saves an environment variable and restores it when going out of scope.
class EnvGuard {
public:
    explicit EnvGuard(const char* name) : name_(name) {
        if (const char* v = std::getenv(name)) old_ = v;
    }
    ~EnvGuard() {
        if (old_) setenv(name_, old_->c_str(), 1);
        else unsetenv(name_);
    }
    void set(const std::string& v) { setenv(name_, v.c_str(), 1); }
    void unset() { unsetenv(name_); }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    const char* name_;
    std::optional<std::string> old_;
};

std::string query(const std::string& pipeline, const std::string& explanation = "Cuenta.") {
    return R"({"type":"query","collection":"pedidos","pipeline":)" + pipeline + R"(,"explanation":")" +
           explanation + R"("})";
}

// The error code of a rejected output ("" if it parsed).
std::string error_of(const std::string& raw) {
    const ParseResult r = parse_response(raw);
    if (r.suggestion) {
        CHECK(r.error.empty());
        return "";
    }
    CHECK_FALSE(r.detail.empty());
    return r.error;
}

// How many times `key` appears in the first level of the sub-document `stage_key` of stage `n`.
int count_keys(const bson_t* pipeline, const char* n, const char* stage_key, const char* key) {
    bson_iter_t it, stage, body;
    if (!bson_iter_init_find(&it, pipeline, n) || !BSON_ITER_HOLDS_DOCUMENT(&it)) return -1;
    if (!bson_iter_recurse(&it, &stage) || !bson_iter_find(&stage, stage_key)) return -1;
    if (!BSON_ITER_HOLDS_DOCUMENT(&stage) || !bson_iter_recurse(&stage, &body)) return -1;
    int count = 0;
    while (bson_iter_next(&body))
        if (std::string(bson_iter_key(&body)) == key) ++count;
    return count;
}

}  // namespace

TEST_SUITE("infer") {

TEST_CASE("parser: los cuatro ejemplos del contrato") {
    const std::string q1 =
        R"({"type":"query","collection":"pedidos","pipeline":[{"$match":{"estado":"cancelado","cliente.ciudad":"Madrid"}},{"$count":"n"}],"explanation":"Cuenta los pedidos cancelados de clientes de Madrid."})";
    ParseResult r = parse_response(q1);
    REQUIRE(r.suggestion);
    CHECK(r.error.empty());
    CHECK(r.suggestion->kind == Suggestion::Kind::Query);
    CHECK(r.suggestion->collection == "pedidos");
    CHECK(r.suggestion->pipeline.text ==
          R"([{"$match":{"estado":"cancelado","cliente.ciudad":"Madrid"}},{"$count":"n"}])");
    CHECK(r.suggestion->pipeline.bson);
    CHECK(r.suggestion->explanation == "Cuenta los pedidos cancelados de clientes de Madrid.");

    const std::string q2 =
        R"({"type":"query","collection":"pedidos","pipeline":[{"$match":{"$expr":{"$gte":["$fecha",{"$dateTrunc":{"date":"$$NOW","unit":"year"}}]}}},{"$group":{"_id":{"$month":"$fecha"},"total":{"$sum":"$importe"}}},{"$sort":{"_id":1}}],"explanation":"Sums order amounts per month for the current year."})";
    r = parse_response(q2);
    REQUIRE(r.suggestion);
    CHECK(r.suggestion->kind == Suggestion::Kind::Query);
    CHECK(bson_count_keys(r.suggestion->pipeline.bson.get()) == 3);

    r = parse_response(
        R"({"type":"refuse","collection":"","pipeline":[],"explanation":"askmdb solo lee: no puede borrar documentos."})");
    REQUIRE(r.suggestion);
    CHECK(r.suggestion->kind == Suggestion::Kind::Refuse);
    CHECK(r.suggestion->collection.empty());
    CHECK(r.suggestion->pipeline.text == "[]");
    REQUIRE(r.suggestion->pipeline.bson);
    CHECK(bson_count_keys(r.suggestion->pipeline.bson.get()) == 0);
    CHECK(r.suggestion->explanation == "askmdb solo lee: no puede borrar documentos.");

    r = parse_response(
        R"({"type":"unsure","collection":"","pipeline":[],"explanation":"El esquema no tiene datos de repartidores."})");
    REQUIRE(r.suggestion);
    CHECK(r.suggestion->kind == Suggestion::Kind::Unsure);
}

TEST_CASE("parser: espacios alrededor y dentro se aceptan; el pipeline conserva su texto") {
    const ParseResult r = parse_response(
        "\n { \"type\" : \"query\" , \"collection\":\"pedidos\", \"pipeline\" : [ {\"$count\" : \"n\"} ] ,"
        " \"explanation\":\"Cuenta.\" } \n");
    REQUIRE(r.suggestion);
    CHECK(r.suggestion->pipeline.text == R"([ {"$count" : "n"} ])");
}

TEST_CASE("parser: JSON no válido") {
    CHECK(error_of("") == "invalid_json");
    CHECK(error_of("   ") == "invalid_json");
    CHECK(error_of(R"({"type":"query")") == "invalid_json");
    CHECK(error_of(query(R"([{"$count":"n"}])") + "x") == "invalid_json");
    CHECK(error_of(query(R"([{"$count":"n"}])") + "{}") == "invalid_json");
    CHECK(error_of(R"({'type':'query'})") == "invalid_json");
    CHECK(error_of(query(R"([{"$count":"n",}])")) == "invalid_json");
    // A raw control character inside a string is not valid JSON.
    CHECK(error_of(query(R"([{"$count":"n"}])", "a\tb")) == "invalid_json");
    // Invalid UTF-8.
    CHECK(error_of(query(R"([{"$count":"n"}])", "a\xFF")) == "invalid_json");
    // Number overflow: nlohmann throws out_of_range; it must not escape.
    CHECK(error_of(query(R"([{"$match":{"a":1e999}}])")) == "invalid_json");
}

TEST_CASE("parser: el nivel superior tiene que ser un objeto") {
    CHECK(error_of("[]") == "invalid_json");
    CHECK(error_of(R"("query")") == "invalid_json");
    CHECK(error_of("42") == "invalid_json");
    CHECK(error_of("null") == "invalid_json");
}

TEST_CASE("parser: más de 8 KiB") {
    // A valid object of exactly 8 KiB passes; one byte more is rejected before parsing.
    std::string ok = query(R"([{"$match":{"s":""}}])");
    ok.insert(ok.find(R"("s":"")") + 5, askmdb::infer::kMaxRawBytes - ok.size(), 'a');
    REQUIRE(ok.size() == askmdb::infer::kMaxRawBytes);
    CHECK(error_of(ok) == "");
    std::string big = ok;
    big.insert(big.find(R"("s":")") + 5, "a");
    CHECK(error_of(big) == "invalid_json");
    CHECK(parse_response(big).detail.find("too long") != std::string::npos);
}

TEST_CASE("parser: claves de nivel superior exactas y en orden") {
    // Swapped order.
    ParseResult r = parse_response(
        R"({"collection":"pedidos","type":"query","pipeline":[{"$count":"n"}],"explanation":"Cuenta."})");
    CHECK(r.error == "invalid_schema");
    CHECK(r.detail.find("order") != std::string::npos);
    CHECK(error_of(R"({"type":"query","collection":"pedidos","explanation":"Cuenta.","pipeline":[{"$count":"n"}]})") ==
          "invalid_schema");

    // Duplicate key (nlohmann alone would keep the last one).
    r = parse_response(
        R"({"type":"refuse","type":"query","collection":"pedidos","pipeline":[{"$count":"n"}],"explanation":"Cuenta."})");
    CHECK(r.error == "invalid_schema");
    CHECK(r.detail.find("duplicate") != std::string::npos);
    CHECK(error_of(R"({"type":"query","collection":"pedidos","pipeline":[{"$count":"n"}],"explanation":"Cuenta.",)"
                   R"("explanation":"Otra."})") == "invalid_schema");

    // Extra key, at the end or in the middle.
    r = parse_response(
        R"({"type":"query","collection":"pedidos","pipeline":[{"$count":"n"}],"explanation":"Cuenta.","db":"x"})");
    CHECK(r.error == "invalid_schema");
    CHECK(r.detail.find("unexpected") != std::string::npos);
    CHECK(error_of(R"({"type":"query","db":"x","collection":"pedidos","pipeline":[{"$count":"n"}],"explanation":"Cuenta."})") ==
          "invalid_schema");

    // Missing key.
    r = parse_response(R"({"type":"query","collection":"pedidos","pipeline":[{"$count":"n"}]})");
    CHECK(r.error == "invalid_schema");
    CHECK(r.detail.find("missing key: explanation") != std::string::npos);
    CHECK(error_of("{}") == "invalid_schema");

    // Keys with those names inside the pipeline do not count as top-level keys.
    CHECK(error_of(query(R"([{"$match":{"type":"a","collection":"b","explanation":"c","type":"d"}}])")) == "");
}

TEST_CASE("parser: caracteres de control decodificados") {
    CHECK(error_of(query(R"([{"$count":"n"}])", R"(a\u0001b)")) == "invalid_schema");
    CHECK(error_of(query(R"([{"$count":"n"}])", R"(a\nb)")) == "invalid_schema");
    CHECK(error_of(query(R"([{"$count":"n"}])", R"(a\u007fb)")) == "invalid_schema");
    CHECK(error_of(query(R"([{"$count":"n"}])", R"(a\u0085b)")) == "invalid_schema");
    // Raw DEL and C1 are valid JSON, but still control characters.
    CHECK(error_of(query(R"([{"$count":"n"}])", "a\x7F" "b")) == "invalid_schema");
    CHECK(error_of(query(R"([{"$count":"n"}])", "a\xC2\x9F" "b")) == "invalid_schema");
    // Anywhere: inside the pipeline, in values and in keys.
    CHECK(error_of(query(R"([{"$match":{"a":"x\u0000y"}}])")) == "invalid_schema");
    CHECK(error_of(query(R"([{"$match":{"a\tb":1}}])")) == "invalid_schema");
    CHECK(error_of(R"({"type":"query","collection":"ped\u001bidos","pipeline":[{"$count":"n"}],"explanation":"Cuenta."})") ==
          "invalid_schema");
    // Non-control Unicode is fine.
    CHECK(error_of(query(R"([{"$match":{"ciudad":"Málaga"}}])", "Cuenta los de Málaga €.")) == "");
}

TEST_CASE("parser: tipos JSON de cada clave") {
    CHECK(error_of(R"({"type":1,"collection":"pedidos","pipeline":[{"$count":"n"}],"explanation":"Cuenta."})") ==
          "invalid_schema");
    CHECK(error_of(R"({"type":"query","collection":null,"pipeline":[{"$count":"n"}],"explanation":"Cuenta."})") ==
          "invalid_schema");
    CHECK(error_of(R"({"type":"query","collection":"pedidos","pipeline":{"$count":"n"},"explanation":"Cuenta."})") ==
          "invalid_schema");
    CHECK(error_of(R"({"type":"query","collection":"pedidos","pipeline":"[]","explanation":"Cuenta."})") ==
          "invalid_schema");
    CHECK(error_of(R"({"type":"query","collection":"pedidos","pipeline":[{"$count":"n"}],"explanation":["Cuenta."]})") ==
          "invalid_schema");
}

TEST_CASE("parser: reglas de type") {
    CHECK(error_of(R"({"type":"command","collection":"pedidos","pipeline":[{"$count":"n"}],"explanation":"Cuenta."})") ==
          "invalid_schema");
    CHECK(error_of(R"({"type":"Query","collection":"pedidos","pipeline":[{"$count":"n"}],"explanation":"Cuenta."})") ==
          "invalid_schema");

    // query: collection not empty, 1..20 stages.
    CHECK(error_of(R"({"type":"query","collection":"","pipeline":[{"$count":"n"}],"explanation":"Cuenta."})") ==
          "invalid_schema");
    CHECK(error_of(query("[]")) == "invalid_schema");
    std::string stages = "[";
    for (int i = 0; i < 20; ++i) stages += std::string(i ? "," : "") + R"({"$match":{}})";
    CHECK(error_of(query(stages + "]")) == "");
    const ParseResult r = parse_response(query(stages + R"(,{"$match":{}}])"));
    CHECK(r.error == "invalid_schema");
    CHECK(r.detail.find("too many stages") != std::string::npos);
    // What a stage looks like is the verifier's job.
    CHECK(error_of(query(R"([1,"x",{"$match":{},"$limit":1}])")) == "");

    // refuse / unsure: collection "" and pipeline [].
    CHECK(error_of(R"({"type":"refuse","collection":"","pipeline":[{"$count":"n"}],"explanation":"No."})") ==
          "invalid_schema");
    CHECK(error_of(R"({"type":"unsure","collection":"pedidos","pipeline":[],"explanation":"No."})") ==
          "invalid_schema");
    CHECK(error_of(R"({"type":"unsure","collection":"","pipeline":[ ],"explanation":"No."})") == "");
}

TEST_CASE("parser: explanation no vacía y de 400 bytes como mucho") {
    CHECK(error_of(query(R"([{"$count":"n"}])", "")) == "invalid_schema");
    CHECK(error_of(query(R"([{"$count":"n"}])", "   ")) == "invalid_schema");
    CHECK(error_of(query(R"([{"$count":"n"}])", std::string(400, 'a'))) == "");
    const ParseResult r = parse_response(query(R"([{"$count":"n"}])", std::string(401, 'a')));
    CHECK(r.error == "invalid_schema");
    CHECK(r.detail.find("explanation too long") != std::string::npos);
    // Bytes, not characters: 200 "ñ" are 400 bytes; one more character is 402.
    std::string enes;
    for (int i = 0; i < 200; ++i) enes += "ñ";
    CHECK(error_of(query(R"([{"$count":"n"}])", enes)) == "");
    CHECK(error_of(query(R"([{"$count":"n"}])", enes + "a")) == "invalid_schema");
}

TEST_CASE("parser: claves repetidas dentro de una etapa llegan al BSON") {
    const ParseResult r = parse_response(query(R"([{"$match":{"a":1,"a":2}}])"));
    REQUIRE(r.suggestion);
    CHECK(r.suggestion->pipeline.text == R"([{"$match":{"a":1,"a":2}}])");
    REQUIRE(r.suggestion->pipeline.bson);
    CHECK(count_keys(r.suggestion->pipeline.bson.get(), "0", "$match", "a") == 2);
}

TEST_CASE("parser: lo que libbson no lee es invalid_json") {
    // Extended JSON that nlohmann accepts as plain JSON but libbson rejects.
    const ParseResult r = parse_response(query(R"([{"$match":{"d":{"$date":"no es fecha"}}}])"));
    CHECK_FALSE(r.suggestion);
    CHECK(r.error == "invalid_json");
    CHECK(r.detail.find("libbson") != std::string::npos);
}

TEST_CASE("parser: anidamiento muy profundo no tumba el proceso") {
    std::string deep(3000, '[');
    deep += std::string(3000, ']');
    const ParseResult r = parse_response(query("[" + deep + "]"));
    CHECK((r.suggestion.has_value() || r.error == "invalid_json"));
}

TEST_CASE("FakeEngine: ASKMDB_FAKE_RAW pasa por el parser real") {
    EnvGuard raw("ASKMDB_FAKE_RAW"), type("ASKMDB_FAKE_TYPE"), pipe("ASKMDB_FAKE_PIPELINE"), err("ASKMDB_FAKE_ERROR");
    for (EnvGuard* g : {&raw, &type, &pipe, &err}) g->unset();
    using askmdb::infer::fake_engine_from_env;
    CHECK(fake_engine_from_env() == nullptr);

    // Valid output; it takes precedence over the other variables.
    const std::string ok = query(R"([{"$count":"n"}])");
    raw.set(ok);
    err.set("timeout");
    type.set("unsure");
    auto engine = fake_engine_from_env();
    REQUIRE(engine);
    auto r = engine->suggest({});
    REQUIRE(r.suggestion);
    CHECK(r.error.empty());
    CHECK(r.raw == ok);
    CHECK(r.suggestion->kind == Suggestion::Kind::Query);
    CHECK(r.suggestion->collection == "pedidos");
    CHECK(r.suggestion->pipeline.bson);
    err.unset();
    type.unset();

    // Broken output: the parser's error and detail, and the raw text.
    raw.set(R"({"type":"query")");
    r = fake_engine_from_env()->suggest({});
    CHECK_FALSE(r.suggestion);
    CHECK(r.error == "invalid_json");
    CHECK_FALSE(r.error_detail.empty());
    CHECK(r.raw == R"({"type":"query")");

    raw.set(R"({"type":"refuse","collection":"","pipeline":[{"$count":"n"}],"explanation":"No."})");
    r = fake_engine_from_env()->suggest({});
    CHECK_FALSE(r.suggestion);
    CHECK(r.error == "invalid_schema");

    // Without FAKE_RAW, raw stays empty.
    raw.unset();
    type.set("unsure");
    r = fake_engine_from_env()->suggest({});
    REQUIRE(r.suggestion);
    CHECK(r.raw.empty());
}

}  // TEST_SUITE
