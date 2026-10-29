#pragma once
// --json output: one object per run, always (also on errors after argument parsing).

#include <bson/bson.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cli/i18n.hpp"
#include "infer/engine.hpp"
#include "verify/verify.hpp"

namespace askmdb::cli {

// What the engine said, once suggest() was called.
struct EngineInfo {
    std::optional<std::string> model;  // basename of the GGUF file; null with FakeEngine
    std::string raw;
    infer::Timings timings;
    infer::PrefixCache prefix_cache = infer::PrefixCache::Off;
};

// What is known when askmdb finishes. Unset fields are written as null.
struct Report {
    std::optional<std::vector<std::string>> hosts;  // server: null until the URI is parsed
    std::string db;
    std::string question;                    // normalized
    std::optional<std::string> type;         // query | refuse | unsure
    std::optional<std::string> collection;
    std::shared_ptr<const bson_t> pipeline;  // array document; written as relaxed EJSON
    std::optional<std::string> mongosh;
    std::optional<std::string> explanation;
    std::optional<verify::Assessment> assessment;  // level + reasons
    bool executed = false;
    std::optional<std::string> rows_json;  // mongo::rows_json, spliced verbatim
    std::optional<bool> truncated;
    std::optional<std::string> error;         // stable code (see cli.cpp)
    std::optional<std::string> error_detail;  // human-readable, untranslated, without credentials
    std::optional<EngineInfo> engine;         // null until the engine is called
    std::optional<std::string> model_name;    // askmdb-0.8b | askmdb-4b | askmdb-9b; null for another GGUF
    std::optional<std::string> device;        // cpu | gpu, once the model is resolved
};

// The keys, in output order.
const std::vector<std::string>& report_keys();

// JSON object on one line (unless rows_json has newlines), no trailing "\n". Reasons in language
// lang. Invalid UTF-8 => U+FFFD. The model fields (model, raw, tokens, timings_ms, prefix_cache)
// are null unless report.engine is set; times in ms rounded to 0.1. model_name and device are null
// until the model is resolved (and with FakeEngine without a model).
std::string json_report(const Report& report, UiLang lang);

}  // namespace askmdb::cli
