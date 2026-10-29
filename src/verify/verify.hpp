#pragma once
// Read-only verifier: a pure function over the BSON that will be sent to the server.
//
// The pipeline text is turned into BSON once, by libbson (load_pipeline). That same document is
// verified and executed; it is never re-serialized from another parser.

#include <bson/bson.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "schema/schema.hpp"

namespace askmdb::verify {

inline constexpr std::size_t kMaxPipelineBytes = 8 * 1024;
inline constexpr int kMaxStages = 20;  // per pipeline level
inline constexpr int kMaxNesting = 16;

enum class Level { Read, Warn, Blocked };  // ordered: the highest wins

// "read" | "warn" | "blocked"
std::string_view to_string(Level level);

struct Reason {
    std::string id;  // stable id: B-* (blocked) or W-* (warning)
    std::string es;  // text in Spanish
    std::string en;  // text in English
};

struct Assessment {
    Level level = Level::Read;
    std::vector<Reason> reasons;  // the reasons at the final level, in discovery order, no repeats
};

struct Pipeline {
    std::string text;                    // exactly as received
    std::shared_ptr<const bson_t> bson;  // array document {"0": stage, "1": ...}; null if no parse
    std::string parse_error;             // libbson's message when bson is null
};

// Parses the text with libbson (bson_new_from_json) into the array document that will be executed.
// Never fails on content: limits, duplicate keys, etc. are verify()'s job.
Pipeline load_pipeline(std::string_view text);

// Level of the pipeline over `collection` in `db`, against the full cached schema. Pure.
// A pipeline that did not parse is Blocked.
Assessment verify(const Pipeline& pipeline, std::string_view collection, std::string_view db,
                  const schema::Schema& schema);

// admin, local and config are blocked before connecting.
std::optional<Reason> check_db(std::string_view db);

// The reason ids and their texts are listed in one place: src/verify/reasons.cpp.

// --- Tables, for tests and tools ---------------------------------------------------------------

// Stages that are known and always blocked (they write, read from the server or are out of scope).
std::vector<std::string_view> blocked_stage_names();
// Operators that run JavaScript on the server ($where, $function, $accumulator).
std::vector<std::string_view> js_operator_names();

// --- Small JSON scanner, so a pipeline reaches load_pipeline as its original text ---------------

// Raw text of the member `key` of a JSON object (e.g. the pipeline of {"db":…,"pipeline":[…]}).
// nullopt if the text is not exactly one well-formed JSON object, or `key` is missing or repeated
// (keys are compared after decoding their escapes).
std::optional<std::string> raw_member(std::string_view json_object, std::string_view key);

// Value of a raw JSON string token, with its escapes decoded; nullopt if it is not a valid string.
std::optional<std::string> json_string(std::string_view raw);

}  // namespace askmdb::verify
