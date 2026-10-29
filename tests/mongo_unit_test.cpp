#include "doctest.h"

#include <mongoc/mongoc.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "mongo/mongo.hpp"

using namespace askmdb;
using namespace askmdb::mongo;

namespace {

Doc json(const char* text) {
    bson_error_t err{};
    Doc doc = adopt(bson_new_from_json(reinterpret_cast<const std::uint8_t*>(text), -1, &err));
    REQUIRE_MESSAGE(doc, err.message);
    return doc;
}

std::vector<Doc> rows(std::initializer_list<const char*> texts) {
    std::vector<Doc> out;
    for (const char* t : texts) out.push_back(json(t));
    return out;
}

std::string cell(const char* doc_text, NumberStyle style = NumberStyle::Es) {
    Doc doc = json(doc_text);
    bson_iter_t it;
    REQUIRE(bson_iter_init_find(&it, doc.get(), "v"));
    return detail::format_cell(it, style);
}

std::string shell(std::string_view coll, const char* pipeline) {
    Doc doc = json(pipeline);
    return to_mongosh(coll, *doc);
}

Uri parse_ok(const char* text) {
    std::string error;
    auto uri = Uri::parse(text, error);
    REQUIRE_MESSAGE(uri, error);
    return *uri;
}

}  // namespace

TEST_CASE("Uri::parse fills hosts, db, credentials and flags") {
    // libmongoc lowercases host names.
    Uri uri = parse_ok("mongodb://u%40x:p%2Fw@Host1:27017,host2/humo?tls=false");
    CHECK(uri.hosts == std::vector<std::string>{"host1:27017", "host2:27017"});
    CHECK(uri.db == "humo");
    CHECK(uri.user == "u@x");
    CHECK(uri.password == "p/w");
    CHECK(uri.has_password());
    CHECK_FALSE(uri.srv);
    CHECK_FALSE(uri.tls);
    CHECK(uri.redacted() == "mongodb://host1:27017,host2:27017/humo");
    CHECK(uri.redacted("otra") == "mongodb://host1:27017,host2:27017/otra");
    CHECK(uri.host_key() == "host1:27017,host2:27017");

    Uri plain = parse_ok("mongodb://localhost");
    CHECK(plain.db.empty());
    CHECK(plain.user.empty());
    CHECK_FALSE(plain.has_password());
    CHECK(plain.redacted() == "mongodb://localhost:27017");
    CHECK(plain.redacted("humo") == "mongodb://localhost:27017/humo");

    CHECK(parse_ok("mongodb://h/x?tls=true").tls);
}

TEST_CASE("Uri host_key is order and case insensitive and ignores the user") {
    CHECK(parse_ok("mongodb://B:1,a:2/x").host_key() == parse_ok("mongodb://ana:pw@a:2,b:1/y").host_key());
}

TEST_CASE("Uri::parse of mongodb+srv does not resolve and implies TLS") {
    Uri uri = parse_ok("mongodb+srv://user:pw@Cluster0.Example.net/ventas");
    CHECK(uri.srv);
    CHECK(uri.tls);
    CHECK(uri.hosts == std::vector<std::string>{"Cluster0.Example.net"});
    CHECK(uri.redacted() == "mongodb+srv://Cluster0.Example.net/ventas");
    CHECK(uri.host_key() == "srv:cluster0.example.net");
}

TEST_CASE("Uri::scrub hides user and password, raw and percent-encoded") {
    Uri uri = parse_ok("mongodb://ana%2Eg:s3cr%2Ft%40pw@h:27017/humo");
    CHECK(uri.user == "ana.g");
    CHECK(uri.password == "s3cr/t@pw");
    std::string msg = uri.scrub("auth for ana.g with s3cr/t@pw, s3cr%2Ft%40pw, s3cr%2ft%40pw, ana%2Eg in " + uri.text);
    CHECK(msg.find("s3cr") == std::string::npos);
    CHECK(msg.find("ana") == std::string::npos);
    CHECK(msg.find("***") != std::string::npos);
    CHECK(uri.scrub("no secrets here") == "no secrets here");
}

TEST_CASE("Uri::scrub replaces a short user name only as a whole word") {
    Uri uri = parse_ok("mongodb://u:pw@h/humo");
    CHECK(uri.scrub("No suitable servers found for u") == "No suitable servers found for ***");
    CHECK(uri.scrub("user u: Authentication failed") == "user ***: Authentication failed");
    CHECK(uri.scrub("bad u:pw@h and u%3A") == "bad ***h and ***%3A");
    CHECK(uri.scrub("password pw, pwd") == "password ***, ***d");  // the password goes anywhere

    Uri two = parse_ok("mongodb://al:x9@h/humo");
    CHECK(two.scrub("all calls by al") == "all calls by ***");
}

TEST_CASE("Uri::scrub replaces a user name of 4+ characters anywhere") {
    Uri uri = parse_ok("mongodb://lector:pw1@h/humo");
    CHECK(uri.scrub("lectores and lector") == "***es and ***");
    CHECK(uri.scrub("connecting to h:27017") == "connecting to h:27017");
}

TEST_CASE("Uri::parse errors never echo credentials") {
    for (const char* text : {"mongodb://bob:hunter2@", "mongodb://bob:hunter2@host:notaport/db",
                             "mongodb://bob:hunter2@h/db?foo=bar&tls=maybe", "mongodb://bob:hun/ter2@h/db",
                             "mongodb://bob:hunter2@h/db?authMechanism=NOPE", "notmongo://bob:hunter2@h"}) {
        std::string error;
        CHECK_FALSE(Uri::parse(text, error));
        CHECK_FALSE(error.empty());
        CHECK_MESSAGE(error.find("hunter2") == std::string::npos, error);
        CHECK_MESSAGE(error.find("ter2") == std::string::npos, error);
        CHECK_MESSAGE(error.find("bob") == std::string::npos, error);
    }
}

TEST_CASE("detail::classify maps libmongoc errors to kinds and scrubs them") {
    Uri uri = parse_ok("mongodb://ana:pw-123@h/humo");
    auto e = detail::classify(MONGOC_ERROR_SERVER_SELECTION, MONGOC_ERROR_SERVER_SELECTION_FAILURE,
                              "No suitable servers found for ana", uri);
    CHECK(e.kind == Error::Kind::Connect);
    CHECK(e.message == "No suitable servers found for ***");
    CHECK(detail::classify(MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_CONNECT, "x", uri).kind == Error::Kind::Connect);
    CHECK(detail::classify(MONGOC_ERROR_CLIENT, MONGOC_ERROR_CLIENT_AUTHENTICATE, "pw-123", uri).kind ==
          Error::Kind::Auth);
    CHECK(detail::classify(MONGOC_ERROR_CLIENT, MONGOC_ERROR_CLIENT_AUTHENTICATE, "pw-123", uri).message == "***");
    CHECK(detail::classify(MONGOC_ERROR_SCRAM, MONGOC_ERROR_SCRAM_PROTOCOL_ERROR, "x", uri).kind == Error::Kind::Auth);
    CHECK(detail::classify(MONGOC_ERROR_SERVER, 18, "x", uri).kind == Error::Kind::Auth);
    CHECK(detail::classify(MONGOC_ERROR_SERVER, 13, "x", uri).kind == Error::Kind::Auth);
    CHECK(detail::classify(MONGOC_ERROR_SERVER, 50, "x", uri).kind == Error::Kind::Timeout);
    CHECK(detail::classify(MONGOC_ERROR_QUERY, 50, "x", uri).kind == Error::Kind::Timeout);
    CHECK(detail::classify(MONGOC_ERROR_SERVER, 40324, "x", uri).kind == Error::Kind::Server);
    CHECK(detail::classify(MONGOC_ERROR_CLIENT, 50, "x", uri).kind == Error::Kind::Server);
}

TEST_CASE("detail::find_ca follows the fixed order") {
    std::map<std::string, std::string> env;
    std::set<std::string> files;
    std::set<std::string> dirs;
    auto getenv = [&](const char* name) -> std::optional<std::string> {
        auto it = env.find(name);
        if (it == env.end()) return std::nullopt;
        return it->second;
    };
    auto exists = [&](const std::string& path, bool is_dir) {
        return is_dir ? dirs.contains(path) : files.contains(path);
    };
    auto find = [&] { return detail::find_ca(getenv, exists); };

    CHECK_FALSE(find());
    dirs.insert("/etc/ssl/certs/");
    CHECK(find()->dir == "/etc/ssl/certs/");
    files.insert("/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem");
    CHECK(find()->file == "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem");
    files.insert("/etc/ssl/ca-bundle.pem");
    CHECK(find()->file == "/etc/ssl/ca-bundle.pem");
    files.insert("/etc/pki/tls/certs/ca-bundle.crt");
    CHECK(find()->file == "/etc/pki/tls/certs/ca-bundle.crt");
    files.insert("/etc/ssl/certs/ca-certificates.crt");
    CHECK(find()->file == "/etc/ssl/certs/ca-certificates.crt");
    CHECK(find()->dir.empty());

    env["SSL_CERT_DIR"] = "/nope:/my/certs";
    CHECK(find()->file == "/etc/ssl/certs/ca-certificates.crt");  // no entry exists yet
    dirs.insert("/my/certs");
    CHECK(find()->dir == "/nope:/my/certs");
    CHECK(find()->file.empty());

    env["SSL_CERT_FILE"] = "";
    CHECK(find()->dir == "/nope:/my/certs");
    env["SSL_CERT_FILE"] = "/my/ca.pem";
    CHECK(find()->dir == "/nope:/my/certs");  // missing file: skipped
    files.insert("/my/ca.pem");
    CHECK(find()->file == "/my/ca.pem");
}

TEST_CASE("render_table: columns, alignment and number styles") {
    auto data = rows({R"({"_id":"C-1042","total":18430.5})", R"({"_id":"C-0007","total":15112.0})"});
    CHECK(render_table(data, NumberStyle::Es) ==
          "   _id          total\n"
          "   C-1042   18.430,50\n"
          "   C-0007   15.112,00\n");
    CHECK(render_table(data, NumberStyle::En) ==
          "   _id          total\n"
          "   C-1042   18,430.50\n"
          "   C-0007   15,112.00\n");
    CHECK(render_table({}, NumberStyle::Es).empty());

    // Columns in order of first appearance; a missing key is an empty cell; null is "null".
    auto mixed = rows({R"({"a":1,"b":"x"})", R"({"c":true,"a":null})", R"({"b":"yy"})"});
    CHECK(render_table(mixed, NumberStyle::En) ==
          "      a   b    c\n"
          "      1   x\n"
          "   null        true\n"
          "          yy\n");
}

TEST_CASE("format_cell: numbers") {
    CHECK(cell(R"({"v":18430})") == "18.430");
    CHECK(cell(R"({"v":18430})", NumberStyle::En) == "18,430");
    CHECK(cell(R"({"v":999})") == "999");
    CHECK(cell(R"({"v":0})") == "0");
    CHECK(cell(R"({"v":-1234567})") == "-1.234.567");
    CHECK(cell(R"({"v":{"$numberLong":"9000000000"}})", NumberStyle::En) == "9,000,000,000");
    CHECK(cell(R"({"v":{"$numberLong":"-9223372036854775808"}})", NumberStyle::En) ==
          "-9,223,372,036,854,775,808");
    CHECK(cell(R"({"v":1234.5})", NumberStyle::En) == "1,234.50");
    CHECK(cell(R"({"v":1234.5})") == "1.234,50");
    CHECK(cell(R"({"v":0.25})") == "0,25");
    CHECK(cell(R"({"v":-0.001})") == "0,00");
    CHECK(cell(R"({"v":-12.5})") == "-12,50");
    CHECK(cell(R"({"v":{"$numberDouble":"NaN"}})") == "NaN");
    CHECK(cell(R"({"v":{"$numberDouble":"-Infinity"}})") == "-Infinity");
    CHECK(cell(R"({"v":{"$numberDecimal":"12.50"}})") == "12.50");
}

TEST_CASE("format_cell: dates, ids, booleans, null") {
    CHECK(cell(R"({"v":{"$date":"2026-01-01T00:00:00Z"}})") == "2026-01-01T00:00:00Z");
    CHECK(cell(R"({"v":{"$date":"2026-01-01T10:20:30.123Z"}})") == "2026-01-01T10:20:30.123Z");
    CHECK(cell(R"({"v":{"$date":{"$numberLong":"-1"}}})") == "1969-12-31T23:59:59.999Z");
    CHECK(cell(R"({"v":{"$oid":"65a1b2c3d4e5f60718293a4b"}})") == "65a1b2c3d4e5f60718293a4b");
    CHECK(cell(R"({"v":true})") == "true");
    CHECK(cell(R"({"v":false})") == "false");
    CHECK(cell(R"({"v":null})") == "null");
}

TEST_CASE("format_cell: subdocuments and arrays are compact JSON cut at 40 characters") {
    CHECK(cell(R"({"v":{"a":1,"b":[2,"x y"]}})") == R"({"a":1,"b":[2,"x y"]})");
    CHECK(cell(R"({"v":[]})") == "[]");
    CHECK(cell(R"({"v":{"d":{"$date":"2026-01-01T00:00:00Z"}}})") == R"({"d":{"$date":"2026-01-01T00:00:00Z"}})");

    std::string long_cell = cell(R"({"v":{"a":"0123456789012345678901234567890123456789"}})");
    CHECK(detail::display_width(long_cell) == 40);
    CHECK(long_cell == R"({"a":"012345678901234567890123456789012…)");

    // Never cuts inside a multibyte character.
    std::string utf = cell(R"({"v":["ñññññññññññññññññññññññññññññññññññññññññññññ"]})");
    CHECK(detail::display_width(utf) == 40);
    CHECK(utf.ends_with("ñ…"));
    CHECK(cell(R"({"v":[1,2]})") == "[1,2]");
}

TEST_CASE("format_cell: strings are terminal safe") {
    std::string s = cell(R"({"v":"a\u001b[31mb\nc\td"})");
    CHECK(s.find('\x1b') == std::string::npos);
    CHECK(s.find('\n') == std::string::npos);
    CHECK(s.find('\t') == std::string::npos);
    CHECK(s.find("\\n") != std::string::npos);
    CHECK(cell(R"({"v":"Málaga"})") == "Málaga");
}

TEST_CASE("display_width counts code points and wide characters") {
    CHECK(detail::display_width("abc") == 3);
    CHECK(detail::display_width("Málaga") == 6);
    CHECK(detail::display_width("日本") == 4);
    CHECK(detail::display_width("…") == 1);

    auto data = rows({R"({"c":"日本","n":1})", R"({"c":"abcde","n":22})"});
    CHECK(render_table(data, NumberStyle::En) ==
          "   c        n\n"
          "   日本     1\n"
          "   abcde   22\n");
}

TEST_CASE("rows_json is a JSON array of relaxed Extended JSON") {
    auto data = rows({R"({"a":1,"d":{"$date":"2026-01-01T00:00:00Z"}})", R"({"b":"x"})"});
    std::string out = rows_json(data);
    CHECK(out.starts_with("[{"));
    CHECK(out.ends_with("}]"));
    CHECK(out.find(R"("$date" : "2026-01-01T00:00:00Z")") != std::string::npos);
    CHECK(out.find(R"("b" : "x")") != std::string::npos);
    CHECK(rows_json({}) == "[]");
    bson_error_t err{};
    bson_t parsed;
    CHECK(bson_init_from_json(&parsed, out.c_str(), -1, &err));
    bson_destroy(&parsed);
}

TEST_CASE("to_mongosh: one stage per line, unquoted identifiers, shell helpers") {
    CHECK(shell("pedidos",
                R"([{"$match":{"estado":"cancelado","cliente.ciudad":"Madrid",)"
                R"("fecha":{"$gte":{"$date":"2026-01-01T00:00:00Z"}}}},)"
                R"({"$group":{"_id":"$estado","n":{"$sum":1}}},{"$limit":{"$numberLong":"5"}}])") ==
          "db.pedidos.aggregate([\n"
          "  { $match: { estado: \"cancelado\", \"cliente.ciudad\": \"Madrid\", "
          "fecha: { $gte: ISODate(\"2026-01-01T00:00:00Z\") } } },\n"
          "  { $group: { _id: \"$estado\", n: { $sum: 1 } } },\n"
          "  { $limit: NumberLong(\"5\") }\n"
          "])");
    CHECK(shell("pedidos", "[]") == "db.pedidos.aggregate([])");
}

TEST_CASE("to_mongosh: values") {
    auto stage = [](const char* value) {
        std::string pipeline = std::string(R"([{"$match":{"v":)") + value + "}}]";
        std::string out = shell("c", pipeline.c_str());
        std::string prefix = "db.c.aggregate([\n  { $match: { v: ";
        std::string suffix = " } }\n])";
        REQUIRE(out.starts_with(prefix));
        REQUIRE(out.ends_with(suffix));
        return out.substr(prefix.size(), out.size() - prefix.size() - suffix.size());
    };
    CHECK(stage("1") == "1");
    CHECK(stage("1.0") == "1.0");
    CHECK(stage("2.5") == "2.5");
    CHECK(stage("0.1") == "0.1");
    CHECK(stage("-3e+300") == "-3e+300");
    CHECK(stage(R"({"$numberDecimal":"12.50"})") == R"(NumberDecimal("12.50"))");
    CHECK(stage(R"({"$oid":"65a1b2c3d4e5f60718293a4b"})") == R"(ObjectId("65a1b2c3d4e5f60718293a4b"))");
    CHECK(stage(R"({"$date":"2026-03-01T12:00:00.500Z"})") == R"(ISODate("2026-03-01T12:00:00.500Z"))");
    CHECK(stage(R"({"$regularExpression":{"pattern":"^a/b\\/c","options":"i"}})") == R"(/^a\/b\/c/i)");
    CHECK(stage(R"({"$regex":"^Mad","$options":""})") == "/^Mad/");
    CHECK(stage(R"({"$regularExpression":{"pattern":"a b","options":"ix"}})") == R"(BSONRegExp("a b", "ix"))");
    CHECK(stage("true") == "true");
    CHECK(stage("null") == "null");
    CHECK(stage("{}") == "{}");
    CHECK(stage("[]") == "[]");
    CHECK(stage(R"([1,"a",[2]])") == R"([1, "a", [2]])");
    CHECK(stage(R"({"":1,"a-b":2,"$x":3,"_y9":4,"9z":5})") == R"({ "": 1, "a-b": 2, $x: 3, _y9: 4, "9z": 5 })");
    CHECK(stage(R"("say \"hi\"\\ \u001b")") == R"("say \"hi\"\\ \u001B")");
    CHECK(stage(R"({"$timestamp":{"t":5,"i":1}})") == "Timestamp({ t: 5, i: 1 })");
    CHECK(stage(R"({"$minKey":1})") == "MinKey()");
    CHECK(stage(R"({"$binary":{"base64":"AQI=","subType":"00"}})") == R"(BinData(0, "AQI="))");
}

TEST_CASE("to_mongosh: odd collection names use getCollection") {
    CHECK(shell("ventas_2026", "[]").starts_with("db.ventas_2026.aggregate("));
    CHECK(shell("stats", "[]").starts_with(R"(db.getCollection("stats").aggregate()"));
    CHECK(shell("aggregate", "[]").starts_with(R"(db.getCollection("aggregate").)"));
    CHECK(shell("mis-pedidos", "[]").starts_with(R"(db.getCollection("mis-pedidos").)"));
    CHECK(shell("a.b", "[]").starts_with(R"(db.getCollection("a.b").)"));
    CHECK(shell("_tmp", "[]").starts_with(R"(db.getCollection("_tmp").)"));
    CHECK(shell("$x", "[]").starts_with(R"(db.getCollection("$x").)"));
    CHECK(shell("2026", "[]").starts_with(R"(db.getCollection("2026").)"));
    CHECK(shell("pedidos_madrid", "[]").starts_with("db.pedidos_madrid."));
}
