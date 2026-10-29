#include "cli/report.hpp"

#include <cmath>
#include <nlohmann/json.hpp>

#include "cli/ui.hpp"
#include "infer/engine.hpp"

namespace askmdb::cli {

namespace {

using Json = nlohmann::ordered_json;

template <class T>
Json or_null(const std::optional<T>& v) {
    return v ? Json(*v) : Json(nullptr);
}

// Relaxed Extended JSON of the array document, straight from libbson (never re-serialized).
std::optional<std::string> pipeline_ejson(const bson_t* pipeline) {
    if (pipeline == nullptr) return std::nullopt;
    std::size_t len = 0;
    char* text = bson_array_as_relaxed_extended_json(pipeline, &len);
    if (text == nullptr) return std::nullopt;
    std::string out(text, len);
    bson_free(text);
    return out;
}

Json ms(double v) { return std::round(v * 10.0) / 10.0; }

const char* prefix_cache_name(infer::PrefixCache p) {
    switch (p) {
        case infer::PrefixCache::Hit: return "hit";
        case infer::PrefixCache::Miss: return "miss";
        case infer::PrefixCache::Off: return "off";
    }
    return "off";
}

}  // namespace

const std::vector<std::string>& report_keys() {
    static const std::vector<std::string> keys = {
        "contract_version", "askmdb_version", "server",    "question",   "type",         "collection",
        "pipeline",         "mongosh",        "explanation", "level",    "reasons",      "executed",
        "rows",             "truncated",      "error",     "error_detail", "model",      "model_name",
        "device",           "raw",            "tokens",    "timings_ms",   "prefix_cache"};
    return keys;
}

std::string json_report(const Report& r, UiLang lang) {
    Json j;
    j["contract_version"] = infer::kContractVersion;
    j["askmdb_version"] = ASKMDB_VERSION;
    j["server"] = r.hosts ? Json{{"hosts", *r.hosts}, {"db", r.db}} : Json(nullptr);
    j["question"] = r.question;
    j["type"] = or_null(r.type);
    j["collection"] = or_null(r.collection);
    j["pipeline"] = nullptr;  // spliced below
    j["mongosh"] = or_null(r.mongosh);
    j["explanation"] = or_null(r.explanation);
    if (r.assessment) {
        j["level"] = std::string(verify::to_string(r.assessment->level));
        Json reasons = Json::array();
        for (const auto& reason : r.assessment->reasons)
            reasons.push_back({{"id", reason.id}, {"text", std::string(reason_text(reason, lang))}});
        j["reasons"] = reasons;
    } else {
        j["level"] = nullptr;
        j["reasons"] = nullptr;
    }
    j["executed"] = r.executed;
    j["rows"] = nullptr;  // spliced below
    j["truncated"] = or_null(r.truncated);
    j["error"] = or_null(r.error);
    j["error_detail"] = or_null(r.error_detail);
    if (r.engine) {
        const infer::Timings& t = r.engine->timings;
        j["model"] = or_null(r.engine->model);
        j["model_name"] = or_null(r.model_name);
        j["device"] = or_null(r.device);
        j["raw"] = r.engine->raw;
        j["tokens"] = {{"prompt", t.prompt_tokens}, {"prefix", t.prefix_tokens}, {"generated", t.gen_tokens}};
        j["timings_ms"] = {{"load", ms(t.load_ms)},
                           {"prefill", ms(t.prefill_ms)},
                           {"generate", ms(t.generate_ms)},
                           {"total", ms(t.total_ms)}};
        j["prefix_cache"] = prefix_cache_name(r.engine->prefix_cache);
    } else {
        j["model"] = nullptr;
        j["model_name"] = or_null(r.model_name);
        j["device"] = or_null(r.device);
        for (const char* k : {"raw", "tokens", "timings_ms", "prefix_cache"}) j[k] = nullptr;
    }

    const std::optional<std::string> pipeline = pipeline_ejson(r.pipeline.get());
    std::string out = "{";
    bool first = true;
    for (const auto& [key, value] : j.items()) {
        if (!first) out += ',';
        first = false;
        out += Json(key).dump();
        out += ':';
        if (key == "pipeline" && pipeline) out += *pipeline;
        else if (key == "rows" && r.rows_json) out += *r.rows_json;
        else out += value.dump(-1, ' ', false, Json::error_handler_t::replace);
    }
    out += '}';
    return out;
}

}  // namespace askmdb::cli
