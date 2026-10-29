// Golden prompt files (tests/golden): the C++ prompt against the transformers chat template.
//
// cases.json is derived from the code. After changing kSystemPrompt, kSchemaBudgetBytes, the block
// format or a question, rewrite it and regenerate the prompt files:
//   ASKMDB_GOLDEN_UPDATE=1 build/askmdb_tests -tc='golden update*'
//   then the generator script that renders the same cases with the transformers chat template.
#include "doctest.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "infer/prompt.hpp"
#include "schema/schema.hpp"

using askmdb::infer::build_messages;
using askmdb::infer::Message;
using askmdb::infer::normalize_request;
using askmdb::infer::prefix_bytes;
using askmdb::infer::render_prompt;
using askmdb::infer::render_qwen35;
using askmdb::infer::Request;
namespace schema = askmdb::schema;

namespace {

using Json = nlohmann::ordered_json;

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot read " << path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Json load_cases() { return Json::parse(read_file(ASKMDB_GOLDEN_DIR "/cases.json")); }

std::vector<Message> to_messages(const Json& arr) {
    std::vector<Message> out;
    for (const auto& m : arr) out.push_back({m.at("role").get<std::string>(), m.at("content").get<std::string>()});
    return out;
}

Request request_of(const Json& c) {
    return {c.at("schema_block").get<std::string>(), c.at("question").get<std::string>()};
}

schema::Schema parse_ok(const std::string& block) {
    auto r = schema::parse(block);
    REQUIRE_MESSAGE(r.schema, r.error);
    return *r.schema;
}

std::string grande() { return read_file(ASKMDB_GOLDEN_DIR "/schemas/presupuesto-grande.txt"); }

// The presupuesto block: the large schema trimmed to the budget, as the binary renders it.
std::string presupuesto_block() {
    const schema::Fit fit = schema::render(parse_ok(grande()), schema::kSchemaBudgetBytes);
    REQUIRE(fit.ok);
    return fit.block;
}

std::string humo_block() { return read_file(ASKMDB_TESTS_DIR "/verify/schemas/humo.txt"); }

}  // namespace

TEST_SUITE("infer") {

TEST_CASE("golden update: rewrite cases.json from the code (only with ASKMDB_GOLDEN_UPDATE=1)") {
    const char* update = std::getenv("ASKMDB_GOLDEN_UPDATE");
    if (update == nullptr || std::string(update) != "1") return;
    Json cases = load_cases();
    for (auto& c : cases) {
        const std::string id = c.at("id").get<std::string>();
        if (c.contains("question_raw")) c["question"] = normalize_request(c["question_raw"].get<std::string>());
        if (id == "presupuesto") c["schema_block"] = presupuesto_block();
        if (id == "humo") c["schema_block"] = humo_block();
        Json messages = Json::array();
        for (const auto& m : build_messages(request_of(c))) messages.push_back({{"role", m.role}, {"content", m.content}});
        c["messages"] = std::move(messages);
    }
    std::ofstream(ASKMDB_GOLDEN_DIR "/cases.json", std::ios::binary) << cases.dump(2) << '\n';
    MESSAGE("cases.json rewritten: regenerate the prompt files with gen_golden.py");
}

TEST_CASE("golden: render_qwen35 matches apply_chat_template byte for byte") {
    const auto cases = load_cases();
    REQUIRE(cases.size() >= 6);
    for (const auto& c : cases) {
        const std::string id = c.at("id").get<std::string>();
        CAPTURE(id);
        CHECK(render_qwen35(to_messages(c.at("messages"))) == read_file(ASKMDB_GOLDEN_DIR "/prompt_" + id + ".txt"));
    }
}

TEST_CASE("golden: build_messages, render_prompt and normalize_request reproduce every case") {
    for (const auto& c : load_cases()) {
        const std::string id = c.at("id").get<std::string>();
        CAPTURE(id);
        const Request r = request_of(c);
        CHECK(normalize_request(r.question) == r.question);
        CHECK(r.question.size() <= askmdb::infer::kMaxQuestionBytes);
        if (c.contains("question_raw")) {
            const std::string raw = c.at("question_raw").get<std::string>();
            CHECK(raw != r.question);
            CHECK(normalize_request(raw) == r.question);
        }
        CHECK(build_messages(r) == to_messages(c.at("messages")));
        CHECK(render_prompt(r) == read_file(ASKMDB_GOLDEN_DIR "/prompt_" + id + ".txt"));
    }
}

TEST_CASE("golden: prefix_bytes ends at the user header, right before the question") {
    constexpr std::string_view kUserHeader = "<|im_start|>user\n";
    for (const auto& c : load_cases()) {
        const std::string id = c.at("id").get<std::string>();
        CAPTURE(id);
        const Request r = request_of(c);
        const std::string text = read_file(ASKMDB_GOLDEN_DIR "/prompt_" + id + ".txt");
        const std::size_t n = prefix_bytes(r);
        REQUIRE(n <= text.size());
        CHECK(std::string_view(text).substr(0, n).ends_with(kUserHeader));
        CHECK(std::string_view(text).substr(n).starts_with(r.question + "<|im_end|>\n"));
    }
}

TEST_CASE("golden: schema blocks round-trip and are plain text") {
    for (const auto& c : load_cases()) {
        const std::string id = c.at("id").get<std::string>();
        CAPTURE(id);
        const std::string block = c.at("schema_block").get<std::string>();
        CHECK(schema::render(parse_ok(block)) == block);
        std::istringstream lines(block);
        for (std::string line; std::getline(lines, line);) {
            CAPTURE(line);
            CHECK(schema::plain_text(line));
        }
    }
}

TEST_CASE("golden: presupuesto is the large schema trimmed close to the budget") {
    const std::string big = grande();
    const schema::Schema s = parse_ok(big);
    CHECK(schema::render(s) == big);
    CHECK(big.size() > schema::kSchemaBudgetBytes);  // the trimming kicks in

    // Top-level paths alone fit, so the trimming never runs out of paths to remove.
    schema::Schema top = s;
    for (auto& col : top.collections) {
        std::erase_if(col.paths, [](const schema::Path& p) {
            return p.path.find('.') != std::string::npos;
        });
        for (auto& p : col.paths) p.values.clear();
    }
    CHECK(schema::render(top).size() < schema::kSchemaBudgetBytes);

    const std::string block = presupuesto_block();
    CHECK(block.size() <= schema::kSchemaBudgetBytes);
    CHECK(block.size() * 100 >= schema::kSchemaBudgetBytes * 95);
    bool found = false;
    for (const auto& c : load_cases()) {
        if (c.at("id") != "presupuesto") continue;
        found = true;
        CHECK_MESSAGE(c.at("schema_block").get<std::string>() == block,
                      "kSchemaBudgetBytes or the trimming changed: run the golden update");
    }
    CHECK(found);
}

TEST_CASE("golden: required cases and the snapshot blocks") {
    std::vector<std::string> ids;
    for (const auto& c : load_cases()) {
        ids.push_back(c.at("id").get<std::string>());
        if (ids.back() == "humo") CHECK(c.at("schema_block").get<std::string>() == humo_block());
        if (ids.back() == "unicode") CHECK(c.contains("question_raw"));
    }
    for (const char* need : {"tienda_es", "tienda_en", "presupuesto", "unicode", "mezcla", "humo"}) {
        CAPTURE(need);
        CHECK(std::count(ids.begin(), ids.end(), need) == 1);
    }
}

}  // TEST_SUITE
