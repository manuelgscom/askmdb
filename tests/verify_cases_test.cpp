// Verifier case lists (tests/verify/{lectura,aviso,bloqueada}.jsonl): exact level and expected
// reason, at 100 %. Plus coverage: every known-blocked stage and JavaScript operator has a case at
// the top level, inside $facet and inside a $lookup pipeline.
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "schema/schema.hpp"
#include "verify/verify.hpp"

using namespace askmdb;

namespace {

struct Case {
    std::string where;  // file:line
    std::string db, collection, schema, pipeline, level, reason;
};

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string str_member(const std::string& line, const char* key) {
    auto raw = verify::raw_member(line, key);
    if (!raw) return "\x01missing";
    auto s = verify::json_string(*raw);
    return s ? *s : "\x01not a string";
}

std::vector<Case> load_cases(const char* name) {
    std::vector<Case> cases;
    std::string path = std::string(ASKMDB_VERIFY_CASES_DIR) + "/" + name;
    std::ifstream f(path, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), ("missing " + path));
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        auto b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos || line[b] == '#') continue;
        Case c;
        c.where = std::string(name) + ":" + std::to_string(lineno);
        auto pipeline = verify::raw_member(line, "pipeline");
        if (!pipeline) {
            FAIL_CHECK((c.where + ": the line has no single \"pipeline\" member or is not valid JSON"));
            continue;
        }
        c.pipeline = *pipeline;
        c.db = str_member(line, "db");
        c.collection = str_member(line, "collection");
        c.schema = str_member(line, "schema");
        c.level = str_member(line, "level");
        c.reason = str_member(line, "reason");
        cases.push_back(std::move(c));
    }
    return cases;
}

const schema::Schema& schema_named(const std::string& name) {
    static std::map<std::string, schema::Schema> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    std::string path = std::string(ASKMDB_VERIFY_CASES_DIR) + "/schemas/" + name + ".txt";
    auto r = schema::parse(read_file(path));
    REQUIRE_MESSAGE(r.schema, (path + ": " + r.error));
    return cache.emplace(name, *r.schema).first->second;
}

void check_file(const char* name, const char* expected_level, std::size_t minimum) {
    auto cases = load_cases(name);
    for (const auto& c : cases) {
        if (c.level != expected_level) {
            FAIL_CHECK((c.where + ": level \"" + c.level + "\" in a file of " + expected_level));
            continue;
        }
        auto a = verify::verify(verify::load_pipeline(c.pipeline), c.collection, c.db, schema_named(c.schema));
        std::string got = std::string(verify::to_string(a.level));
        bool has_reason = c.reason.empty() ? a.reasons.empty() : false;
        for (const auto& r : a.reasons) has_reason = has_reason || r.id == c.reason;
        if (got == c.level && has_reason) continue;
        std::string reasons;
        for (const auto& r : a.reasons) reasons += "\n    " + r.id + ": " + r.en;
        FAIL_CHECK((c.where + ": " + c.pipeline.substr(0, 300) + "\n  expected " + c.level + " " + c.reason +
                    ", got " + got + reasons));
    }
    CHECK_MESSAGE(cases.size() >= minimum, (std::string(name) + " has " + std::to_string(cases.size()) +
                                            " cases (minimum " + std::to_string(minimum) + ")"));
}

// Where each key appears: "top", "facet" or "lookup" (innermost $facet / $lookup around it).
void contexts(bson_iter_t it, const std::string& ctx, std::set<std::pair<std::string, std::string>>& out) {
    while (bson_iter_next(&it)) {
        std::string k = bson_iter_key(&it);
        out.insert({k, ctx});
        bson_iter_t c;
        if ((BSON_ITER_HOLDS_DOCUMENT(&it) || BSON_ITER_HOLDS_ARRAY(&it)) && bson_iter_recurse(&it, &c))
            contexts(c, k == "$facet" ? "facet" : k == "$lookup" ? "lookup" : ctx, out);
    }
}

}  // namespace

TEST_SUITE("verify_cases") {

TEST_CASE("lectura.jsonl: read") { check_file("lectura.jsonl", "read", 150); }
TEST_CASE("aviso.jsonl: warn") { check_file("aviso.jsonl", "warn", 60); }
TEST_CASE("bloqueada.jsonl: blocked") { check_file("bloqueada.jsonl", "blocked", 150); }

TEST_CASE("every blocked stage and JavaScript operator has a case at top level, in $facet and in $lookup") {
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& c : load_cases("bloqueada.jsonl")) {
        auto p = verify::load_pipeline(c.pipeline);
        if (!p.bson) continue;
        bson_iter_t it;
        if (bson_iter_init(&it, p.bson.get())) contexts(it, "top", seen);
    }
    std::vector<std::string_view> names = verify::blocked_stage_names();
    for (auto js : verify::js_operator_names()) names.push_back(js);
    for (auto name : names) {
        for (const char* ctx : {"top", "facet", "lookup"}) {
            CHECK_MESSAGE(seen.count({std::string(name), ctx}) == 1,
                          ("bloqueada.jsonl has no case with " + std::string(name) + " in context " + ctx));
        }
    }
}

}  // TEST_SUITE
