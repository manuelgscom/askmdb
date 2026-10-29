// Needs a mongod with the humo seed (or one like it): ASKMDB_E2E_URI=mongodb://127.0.0.1:27018/humo
#include "doctest.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>

#include "mongo/mongo.hpp"

using namespace askmdb;
using namespace askmdb::mongo;

namespace {

std::optional<Uri> e2e_uri() {
    const char* text = std::getenv("ASKMDB_E2E_URI");
    if (!text || !*text) {
        MESSAGE("ASKMDB_E2E_URI not set: skipped");
        return std::nullopt;
    }
    std::string error;
    auto uri = Uri::parse(text, error);
    REQUIRE_MESSAGE(uri, error);
    REQUIRE_MESSAGE(!uri->db.empty(), "ASKMDB_E2E_URI needs a database");
    return uri;
}

Doc pipeline(const char* text) {
    bson_error_t err{};
    Doc doc = adopt(bson_new_from_json(reinterpret_cast<const std::uint8_t*>(text), -1, &err));
    REQUIRE_MESSAGE(doc, err.message);
    return doc;
}

std::string hosts_of(const Uri& uri) {
    std::string out;
    for (const auto& h : uri.hosts) out += (out.empty() ? "" : ",") + h;
    return out;
}

}  // namespace

TEST_SUITE("mongo") {
    TEST_CASE("list_collections: sorted, without system.*, views included") {
        auto uri = e2e_uri();
        if (!uri) return;
        Client client(*uri, uri->db);
        auto names = client.list_collections();
        REQUIRE_MESSAGE(names.value, names.error.message);
        CHECK(std::is_sorted(names.value->begin(), names.value->end()));
        for (const auto& n : *names.value) CHECK_FALSE(n.starts_with("system."));
        auto has = [&](const char* n) {
            return std::find(names.value->begin(), names.value->end(), n) != names.value->end();
        };
        CHECK(has("pedidos"));
        CHECK(has("clientes"));
        CHECK(has("pedidos_madrid"));
    }

    TEST_CASE("sample: newest documents first, at most 200") {
        auto uri = e2e_uri();
        if (!uri) return;
        Client client(*uri, uri->db);
        auto docs = client.sample("pedidos");
        REQUIRE_MESSAGE(docs.value, docs.error.message);
        REQUIRE(docs.value->size() >= 2);
        CHECK(docs.value->size() <= static_cast<std::size_t>(kSampleLimit));
        bson_iter_t a;
        bson_iter_t b;
        REQUIRE(bson_iter_init_find(&a, (*docs.value)[0].get(), "_id"));
        REQUIRE(bson_iter_init_find(&b, (*docs.value)[1].get(), "_id"));
        if (BSON_ITER_HOLDS_OID(&a) && BSON_ITER_HOLDS_OID(&b)) {
            CHECK(bson_oid_compare(bson_iter_oid(&a), bson_iter_oid(&b)) > 0);
        }

        auto view = client.sample("pedidos_madrid");
        REQUIRE_MESSAGE(view.value, view.error.message);
        CHECK_FALSE(view.value->empty());

        auto missing = client.sample("no_such_collection");
        REQUIRE(missing.value);
        CHECK(missing.value->empty());
    }

    TEST_CASE("aggregate: final $limit and truncation") {
        auto uri = e2e_uri();
        if (!uri) return;
        Client client(*uri, uri->db);
        Limits limits;
        limits.max_rows = 5;

        auto all = client.aggregate("pedidos", *pipeline(R"([{"$match":{}}])"), limits);
        REQUIRE_MESSAGE(all.value, all.error.message);
        CHECK(all.value->docs.size() == 5);
        CHECK(all.value->truncated);

        auto exact = client.aggregate("pedidos", *pipeline(R"([{"$limit":5}])"), limits);
        REQUIRE_MESSAGE(exact.value, exact.error.message);
        CHECK(exact.value->docs.size() == 5);
        CHECK_FALSE(exact.value->truncated);

        auto few = client.aggregate("pedidos", *pipeline(R"([{"$limit":3},{"$project":{"_id":1}}])"), limits);
        REQUIRE_MESSAGE(few.value, few.error.message);
        CHECK(few.value->docs.size() == 3);
        CHECK_FALSE(few.value->truncated);
        CHECK(bson_count_keys(few.value->docs[0].get()) == 1);

        auto empty = client.aggregate("pedidos", *pipeline("[]"), limits);
        REQUIRE_MESSAGE(empty.value, empty.error.message);
        CHECK(empty.value->truncated);

        auto view = client.aggregate("pedidos_madrid", *pipeline(R"([{"$count":"n"}])"), limits);
        REQUIRE_MESSAGE(view.value, view.error.message);
        CHECK(view.value->docs.size() == 1);
    }

    TEST_CASE("aggregate: errors are classified") {
        auto uri = e2e_uri();
        if (!uri) return;
        Client client(*uri, uri->db);
        Limits limits;
        limits.max_time_ms = 1;
        auto slow = client.aggregate(
            "pedidos",
            *pipeline(R"([{"$limit":1},{"$project":{"x":{"$range":[0,1000]}}},{"$unwind":"$x"},)"
                      R"({"$project":{"y":{"$range":[0,1000]}}},{"$unwind":"$y"},)"
                      R"({"$group":{"_id":null,"n":{"$sum":1}}}])"),
            limits);
        REQUIRE_FALSE(slow.value);
        CHECK(slow.error.kind == Error::Kind::Timeout);

        auto bad = client.aggregate("pedidos", *pipeline(R"([{"$nonsense":1}])"), Limits{});
        REQUIRE_FALSE(bad.value);
        CHECK(bad.error.kind == Error::Kind::Server);
        CHECK_FALSE(bad.error.message.empty());
    }

    TEST_CASE("unreachable server: Connect error in under 6 s") {
        if (!e2e_uri()) return;
        std::string error;
        auto uri = Uri::parse("mongodb://127.0.0.1:1/humo", error);
        REQUIRE(uri);
        auto start = std::chrono::steady_clock::now();
        Client client(*uri, "humo");
        auto names = client.list_collections();
        auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE_FALSE(names.value);
        CHECK(names.error.kind == Error::Kind::Connect);
        CHECK(elapsed < std::chrono::seconds(6));
    }

    TEST_CASE("wrong credentials: Auth error without the password") {
        auto base = e2e_uri();
        if (!base) return;
        std::string text = "mongodb://nobody:s3cr3t-pw@" + hosts_of(*base) + "/" + base->db + "?authSource=admin";
        std::string error;
        auto uri = Uri::parse(text, error);
        REQUIRE(uri);
        Client client(*uri, uri->db);
        auto names = client.list_collections();
        REQUIRE_FALSE(names.value);
        CHECK(names.error.kind == Error::Kind::Auth);
        CHECK(names.error.message.find("s3cr3t") == std::string::npos);
        CHECK(names.error.message.find("nobody") == std::string::npos);
        MESSAGE(names.error.message);
    }
}
