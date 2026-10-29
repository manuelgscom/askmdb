#pragma once
// Strict parser of the model output (the model contract).

#include <optional>
#include <string>
#include <string_view>

#include "infer/engine.hpp"

namespace askmdb::infer {

inline constexpr std::size_t kMaxRawBytes = 8 * 1024;
inline constexpr std::size_t kMaxExplanationBytes = 400;
inline constexpr std::size_t kMaxStages = 20;

struct ParseResult {
    std::optional<Suggestion> suggestion;
    std::string error;   // invalid_json | invalid_schema (empty if ok)
    std::string detail;  // untranslated reason
};

// 1) nlohmann checks the shape: one object with exactly "type", "collection", "pipeline",
//    "explanation", in that order, no repeats; <= kMaxRawBytes; no control characters; the rules
//    per type. 2) The pipeline BSON comes from libbson reading the original text of the
//    "pipeline" member (verify::raw_member + verify::load_pipeline), never from a re-serialisation,
//    so the verifier sees exactly what was written (repeated keys included).
ParseResult parse_response(std::string_view raw);

}  // namespace askmdb::infer
