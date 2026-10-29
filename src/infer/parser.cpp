#include "infer/parser.hpp"

#include <array>
#include <nlohmann/json.hpp>
#include <utility>

namespace askmdb::infer {

namespace {

using nlohmann::json;

constexpr std::array<std::string_view, 4> kKeys = {"type", "collection", "pipeline", "explanation"};

std::string_view trim_ascii(std::string_view s) {
    constexpr std::string_view ws = " \t\n\r\f\v";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

// C0 controls, DEL and C1 (U+0080-U+009F, C2 80-C2 9F in UTF-8). nlohmann already guarantees UTF-8.
bool has_control(std::string_view s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7F) return true;
        if (c == 0xC2 && i + 1 < s.size()) {
            const auto n = static_cast<unsigned char>(s[i + 1]);
            if (n >= 0x80 && n <= 0x9F) return true;
        }
    }
    return false;
}

ParseResult fail(std::string error, std::string detail) {
    ParseResult r;
    r.error = std::move(error);
    r.detail = std::move(detail);
    return r;
}

ParseResult bad_json(std::string detail) { return fail("invalid_json", std::move(detail)); }
ParseResult bad_schema(std::string detail) { return fail("invalid_schema", std::move(detail)); }

// What the parser callback sees of the top-level keys and of every decoded string.
struct Shape {
    std::size_t keys = 0;     // top-level keys seen so far
    std::string key_error;    // first problem with the top-level keys
    std::string control;      // first string (key or value) with a control character
};

void on_top_key(Shape& shape, const std::string& key) {
    if (!shape.key_error.empty()) return;
    for (std::size_t k = 0; k < shape.keys && k < kKeys.size(); ++k) {
        if (key == kKeys[k]) {
            shape.key_error = "duplicate key: " + key;
            return;
        }
    }
    if (shape.keys >= kKeys.size()) {
        shape.key_error = "unexpected key: " + key;
    } else if (key != kKeys[shape.keys]) {
        bool known = false;
        for (const auto k : kKeys) known = known || key == k;
        shape.key_error = known ? "key out of order: " + key + " (expected " + std::string(kKeys[shape.keys]) + ")"
                                : "unexpected key: " + key;
    }
    ++shape.keys;
}

}  // namespace

ParseResult parse_response(std::string_view raw) {
    if (raw.size() > kMaxRawBytes)
        return bad_json("output too long: " + std::to_string(raw.size()) + " bytes (max " +
                        std::to_string(kMaxRawBytes) + ")");

    // Step 1: shape, with nlohmann. The callback sees every key in document order (the DOM loses
    // order and keeps only the last of repeated keys) and every decoded string.
    Shape shape;
    const json::parser_callback_t cb = [&](int depth, json::parse_event_t event, json& parsed) {
        if (event == json::parse_event_t::key || (event == json::parse_event_t::value && parsed.is_string())) {
            const auto& s = parsed.get_ref<const std::string&>();
            if (shape.control.empty() && has_control(s)) shape.control = s;
            if (event == json::parse_event_t::key && depth == 1) on_top_key(shape, s);
        }
        return true;
    };

    json doc;
    try {
        doc = json::parse(raw.begin(), raw.end(), cb);
    } catch (const json::parse_error& e) {
        return bad_json("malformed JSON at byte " + std::to_string(e.byte));
    } catch (const json::exception& e) {
        // e.g. out_of_range 406 on number overflow (1e999): must not abort askmdb.
        return bad_json(std::string("malformed JSON: ") + e.what());
    }
    if (!doc.is_object()) return bad_json("top level is not an object");

    if (!shape.key_error.empty()) return bad_schema(shape.key_error);
    if (shape.keys < kKeys.size()) return bad_schema("missing key: " + std::string(kKeys[shape.keys]));
    if (!shape.control.empty()) return bad_schema("control character in a string");

    const json& type = doc["type"];
    const json& collection = doc["collection"];
    const json& pipeline = doc["pipeline"];
    const json& explanation = doc["explanation"];
    if (!type.is_string()) return bad_schema("type is not a string");
    if (!collection.is_string()) return bad_schema("collection is not a string");
    if (!pipeline.is_array()) return bad_schema("pipeline is not an array");
    if (!explanation.is_string()) return bad_schema("explanation is not a string");

    Suggestion s;
    const auto& type_s = type.get_ref<const std::string&>();
    const auto& collection_s = collection.get_ref<const std::string&>();
    if (type_s == "query") {
        s.kind = Suggestion::Kind::Query;
        if (collection_s.empty()) return bad_schema("empty collection");
        if (pipeline.empty()) return bad_schema("empty pipeline");
        if (pipeline.size() > kMaxStages)
            return bad_schema("too many stages: " + std::to_string(pipeline.size()) + " (max " +
                              std::to_string(kMaxStages) + ")");
    } else if (type_s == "refuse" || type_s == "unsure") {
        s.kind = type_s == "refuse" ? Suggestion::Kind::Refuse : Suggestion::Kind::Unsure;
        if (!collection_s.empty()) return bad_schema("collection must be empty for type " + type_s);
        if (!pipeline.empty()) return bad_schema("pipeline must be empty for type " + type_s);
    } else {
        return bad_schema("unknown type: " + type_s);
    }

    const auto& explanation_s = explanation.get_ref<const std::string&>();
    if (trim_ascii(explanation_s).empty()) return bad_schema("empty explanation");
    if (explanation_s.size() > kMaxExplanationBytes)
        return bad_schema("explanation too long: " + std::to_string(explanation_s.size()) + " bytes (max " +
                          std::to_string(kMaxExplanationBytes) + ")");

    // Step 2: the pipeline BSON, with libbson, from the original text of the member. Never from a
    // re-serialisation of the DOM: repeated keys inside a stage must reach the verifier.
    const auto text = verify::raw_member(raw, "pipeline");
    if (!text) return bad_json("the JSON scanner could not extract the pipeline member");
    s.pipeline = verify::load_pipeline(*text);
    if (!s.pipeline.bson) return bad_json("libbson could not read the pipeline: " + s.pipeline.parse_error);

    s.collection = collection_s;
    s.explanation = explanation_s;
    ParseResult r;
    r.suggestion = std::move(s);
    return r;
}

}  // namespace askmdb::infer
