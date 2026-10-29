#pragma once
// Inference engine interface: LlamaEngine (llama_engine.hpp) and FakeEngine (tests).

#include <optional>
#include <string>

#include "verify/verify.hpp"

namespace askmdb::infer {

inline constexpr int kContractVersion = 1;

struct Suggestion {
    enum class Kind { Query, Refuse, Unsure };
    Kind kind = Kind::Query;
    std::string collection;     // empty if kind != Query
    verify::Pipeline pipeline;  // built by verify::load_pipeline from the model's text
    std::string explanation;    // in the language of the question
};

struct Timings {
    double load_ms = 0, prefill_ms = 0, generate_ms = 0, total_ms = 0;
    int prompt_tokens = 0, prefix_tokens = 0, gen_tokens = 0;
};

// What happened with the saved state of the prompt prefix (--json "prefix_cache").
enum class PrefixCache { Off, Miss, Hit };

// Empty suggestion => error. raw: raw model output.
// error is a code: invalid_json | invalid_schema | truncated | timeout | too_long | load_failed |
// gpu_unavailable | decode_failed.
// error_detail: human-readable reason (for --json and ASKMDB_DEBUG), untranslated.
struct EngineResult {
    std::optional<Suggestion> suggestion;
    std::string error;
    std::string error_detail;
    std::string raw;
    Timings timings;
    PrefixCache prefix_cache = PrefixCache::Off;
};

struct Request {
    std::string schema_block;  // schema::render(schema, budget).block
    std::string question;      // normalized, <= 1000 bytes
};

class Engine {
public:
    virtual ~Engine() = default;
    virtual EngineResult suggest(const Request& request) = 0;
};

}  // namespace askmdb::infer
