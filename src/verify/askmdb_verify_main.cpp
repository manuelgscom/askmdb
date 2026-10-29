// askmdb_verify: the verifier from the command line, for tests, the evaluation harness and the
// dataset. Reads one JSON per line {"db","collection","pipeline"} and writes, for each input line,
// {"level":"read|warn|blocked","reasons":[{"id","text"}]}.
//
//   askmdb_verify --schema-from FILE [--lang es|en]

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#include "verify/internal.hpp"

namespace {

using namespace askmdb;

int usage() {
    std::cerr << "usage: askmdb_verify --schema-from FILE [--lang es|en]\n";
    return 2;
}

verify::Assessment assess(const std::string& line, const schema::Schema& schema) {
    auto input = [](std::string_view why) {
        return verify::Assessment{verify::Level::Blocked, {verify::detail::make_reason("B-INPUT", why)}};
    };
    auto db_raw = verify::raw_member(line, "db");
    auto coll_raw = verify::raw_member(line, "collection");
    auto pipeline = verify::raw_member(line, "pipeline");
    if (!db_raw || !coll_raw || !pipeline) return input("{\"db\",\"collection\",\"pipeline\"}");
    auto db = verify::json_string(*db_raw);
    auto coll = verify::json_string(*coll_raw);
    if (!db || !coll) return input("db/collection");
    return verify::verify(verify::load_pipeline(*pipeline), *coll, *db, schema);
}

}  // namespace

int main(int argc, char** argv) {
    std::string schema_path, lang = "en";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--schema-from" && i + 1 < argc) {
            schema_path = argv[++i];
        } else if (a == "--lang" && i + 1 < argc) {
            lang = argv[++i];
            if (lang != "es" && lang != "en") return usage();
        } else {
            return usage();
        }
    }
    if (schema_path.empty()) return usage();

    std::ifstream f(schema_path, std::ios::binary);
    if (!f) {
        std::cerr << "askmdb_verify: cannot read " << schema_path << "\n";
        return 2;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    auto parsed = schema::parse(ss.str());
    if (!parsed.schema) {
        std::cerr << "askmdb_verify: " << schema_path << ": " << parsed.error << "\n";
        return 2;
    }

    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto a = assess(line, *parsed.schema);
        nlohmann::json out;
        out["level"] = std::string(verify::to_string(a.level));
        out["reasons"] = nlohmann::json::array();
        for (const auto& r : a.reasons) out["reasons"].push_back({{"id", r.id}, {"text", lang == "es" ? r.es : r.en}});
        std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    }
    return 0;
}
