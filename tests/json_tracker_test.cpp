// JsonTracker: where the model's response ends (the top-level '}'), fed in chunks.
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "infer/json_tracker.hpp"

using askmdb::infer::JsonTracker;
using State = JsonTracker::State;

namespace {

// Feeds the chunks to the tracker and returns the accepted text.
std::string feed_all(JsonTracker& t, const std::vector<std::string_view>& chunks) {
    std::string out;
    for (std::string_view c : chunks) out.append(c.substr(0, t.feed(c)));
    return out;
}

}  // namespace

TEST_SUITE("infer") {

TEST_CASE("rastreador: objeto simple en un trozo y en varios") {
    JsonTracker a;
    CHECK(a.state() == State::Waiting);
    CHECK(feed_all(a, {"  \n{\"a\":1}"}) == "  \n{\"a\":1}");
    CHECK(a.state() == State::Done);

    JsonTracker b;
    CHECK(feed_all(b, {"{\"ty", "pe\":\"qu", "ery\"", "}"}) == "{\"type\":\"query\"}");
    CHECK(b.state() == State::Done);

    // After Done nothing more is consumed.
    CHECK(b.feed("{}") == 0);
}

TEST_CASE("rastreador: llaves dentro de cadenas no cuentan (patrones de regex)") {
    JsonTracker t;
    feed_all(t, {R"({"pipeline":[{"$match":{"sku":{"$regex":"^a{2}$"}}},{"$match":{"x":"}}}{"}}])"});
    CHECK(t.state() == State::Open);
    feed_all(t, {"}"});
    CHECK(t.state() == State::Done);

    JsonTracker u;
    CHECK(feed_all(u, {R"({"e":"{"})"}) == R"({"e":"{"})");
    CHECK(u.state() == State::Done);
}

TEST_CASE("rastreador: escapes \\\" y \\\\") {
    JsonTracker a;
    feed_all(a, {R"({"c":"say \"}\" ok"})"});
    CHECK(a.state() == State::Done);

    // \\ closes the escape: the next quote ends the string and the '}' counts.
    JsonTracker b;
    CHECK(feed_all(b, {R"({"c":"dir\\"})"}) == R"({"c":"dir\\"})");
    CHECK(b.state() == State::Done);

    // Escape split across two chunks: the quote after it is escaped.
    JsonTracker c;
    feed_all(c, {R"({"c":"a\)", R"("}")"});
    CHECK(c.state() == State::Open);
    feed_all(c, {"}"});
    CHECK(c.state() == State::Done);

    // \\ split across two chunks: the quote after it closes the string.
    JsonTracker d;
    CHECK(feed_all(d, {R"({"c":"a\)", R"(\"})", "basura"}) == R"({"c":"a\\"})");
    CHECK(d.state() == State::Done);
}

TEST_CASE("rastreador: arrays anidados con objetos (un pipeline)") {
    const std::string response =
        R"({"type":"query","collection":"pedidos","pipeline":[{"$match":{"$expr":{"$gte":["$fecha",)"
        R"({"$dateTrunc":{"date":"$$NOW","unit":"year"}}]}}},{"$group":{"_id":{"$month":"$fecha"},)"
        R"("total":{"$sum":"$importe"}}},{"$sort":{"_id":1}}],"explanation":"Sums per month."})";
    JsonTracker t;
    std::vector<std::string_view> chunks;
    const std::string_view all = response;
    for (std::size_t i = 0; i < all.size(); i += 7) chunks.push_back(all.substr(i, 7));
    CHECK(feed_all(t, chunks) == response);
    CHECK(t.state() == State::Done);
}

TEST_CASE("rastreador: texto basura tras el cierre") {
    JsonTracker t;
    CHECK(feed_all(t, {"{\"a\":{\"b\":{}},", "\"c\":[1,{}]}\n<|im_end|>", "más"}) == "{\"a\":{\"b\":{}},\"c\":[1,{}]}");
    CHECK(t.state() == State::Done);

    JsonTracker u;
    const std::string_view chunk = "{}}{\"x\":1}";
    CHECK(u.feed(chunk) == 2);
    CHECK(u.state() == State::Done);
}

TEST_CASE("rastreador: texto que no empieza por {") {
    JsonTracker a;
    feed_all(a, {"  "});
    CHECK(a.state() == State::Waiting);
    feed_all(a, {"<think>{"});
    CHECK(a.state() == State::Invalid);
    CHECK(a.feed("{}") == 0);

    JsonTracker b;
    feed_all(b, {"Sure! {\"a\":1}"});
    CHECK(b.state() == State::Invalid);

    JsonTracker c;
    feed_all(c, {"[{\"a\":1}]"});
    CHECK(c.state() == State::Invalid);
}

}  // TEST_SUITE
