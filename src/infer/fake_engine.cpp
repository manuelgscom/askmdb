#include "infer/fake_engine.hpp"

#include <cstdlib>
#include <string>
#include <utility>

#include "infer/parser.hpp"

namespace askmdb::infer {

namespace {

std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v != nullptr ? v : fallback;
}

// Always returns the answer set by the ASKMDB_FAKE_* variables.
class FakeEngine : public Engine {
public:
    EngineResult suggest(const Request&) override {
        EngineResult r;
        // Raw text through the real parser: end-to-end JSON errors.
        if (const char* raw = std::getenv("ASKMDB_FAKE_RAW")) {
            r.raw = raw;
            ParseResult p = parse_response(r.raw);
            r.suggestion = std::move(p.suggestion);
            r.error = std::move(p.error);
            r.error_detail = std::move(p.detail);
            return r;
        }
        if (std::getenv("ASKMDB_FAKE_ERROR") != nullptr) {
            r.error = env_or("ASKMDB_FAKE_ERROR", "");
            r.error_detail = env_or("ASKMDB_FAKE_ERROR_DETAIL", "");
            return r;
        }
        const std::string type = env_or("ASKMDB_FAKE_TYPE", "query");
        Suggestion s;
        if (type == "query" || type.empty()) {
            s.kind = Suggestion::Kind::Query;
            s.collection = env_or("ASKMDB_FAKE_COLLECTION", "");
            s.pipeline = verify::load_pipeline(env_or("ASKMDB_FAKE_PIPELINE", ""));
        } else if (type == "refuse" || type == "unsure") {
            s.kind = type == "refuse" ? Suggestion::Kind::Refuse : Suggestion::Kind::Unsure;
            s.pipeline = verify::load_pipeline("[]");
        } else {
            r.error = "invalid_schema";
            r.error_detail = "ASKMDB_FAKE_TYPE: " + type;
            return r;
        }
        s.explanation = env_or("ASKMDB_FAKE_EXPLANATION", "");
        r.suggestion = std::move(s);
        return r;
    }
};

}  // namespace

std::unique_ptr<Engine> fake_engine_from_env() {
    if (std::getenv("ASKMDB_FAKE_RAW") == nullptr && std::getenv("ASKMDB_FAKE_TYPE") == nullptr &&
        std::getenv("ASKMDB_FAKE_PIPELINE") == nullptr && std::getenv("ASKMDB_FAKE_ERROR") == nullptr)
        return nullptr;
    return std::make_unique<FakeEngine>();
}

}  // namespace askmdb::infer
