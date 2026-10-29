// llama.cpp tokens against the transformers tokenizer ones (tests/golden/*.tokens), and the token
// count of the schema blocks that calibrates kSchemaBudgetBytes.
// Needs ASKMDB_TEST_MODEL (a GGUF); skipped when the variable is unset.
#include "doctest.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "infer/prompt.hpp"
#include "llama.h"

namespace {

// Token limit of the trimmed schema block (contract: ~1,500 tokens).
constexpr std::size_t kSchemaTokenLimit = 1500;

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot read " << path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void quiet_log(ggml_log_level, const char*, void*) {}

std::vector<llama_token> tokenize(const llama_vocab* vocab, const std::string& text) {
    const auto len = static_cast<int32_t>(text.size());
    int32_t n = -llama_tokenize(vocab, text.data(), len, nullptr, 0, false, true);
    std::vector<llama_token> out(static_cast<std::size_t>(n));
    n = llama_tokenize(vocab, text.data(), len, out.data(), n, false, true);
    REQUIRE(n == static_cast<int32_t>(out.size()));
    return out;
}

}  // namespace

TEST_SUITE("model") {

TEST_CASE("golden: llama.cpp tokens = transformers tokens, and schema blocks within the token limit") {
    const char* model_path = std::getenv("ASKMDB_TEST_MODEL");
    if (model_path == nullptr || *model_path == '\0') {
        MESSAGE("ASKMDB_TEST_MODEL unset: skipped");
        return;
    }
    if (std::getenv("ASKMDB_DEBUG") == nullptr) llama_log_set(quiet_log, nullptr);
    llama_backend_init();
    llama_model_params params = llama_model_default_params();
    params.vocab_only = true;
    llama_model* model = llama_model_load_from_file(model_path, params);
    REQUIRE_MESSAGE(model != nullptr, "cannot load " << model_path);
    const llama_vocab* vocab = llama_model_get_vocab(model);

    MESSAGE("kSystemPrompt: " << tokenize(vocab, std::string(askmdb::infer::kSystemPrompt)).size() << " tokens");
    const auto counts = nlohmann::json::parse(read_file(ASKMDB_GOLDEN_DIR "/schema_tokens.json"));
    for (const auto& c : nlohmann::json::parse(read_file(ASKMDB_GOLDEN_DIR "/cases.json"))) {
        const std::string id = c.at("id").get<std::string>();
        CAPTURE(id);
        const std::string text = read_file(ASKMDB_GOLDEN_DIR "/prompt_" + id + ".txt");
        std::vector<llama_token> expected;
        std::istringstream in(read_file(ASKMDB_GOLDEN_DIR "/prompt_" + id + ".tokens"));
        for (llama_token t; in >> t;) expected.push_back(t);
        REQUIRE(!expected.empty());
        CHECK(tokenize(vocab, text) == expected);

        const std::string block = c.at("schema_block").get<std::string>();
        const std::size_t block_tokens = tokenize(vocab, block).size();
        CHECK(block_tokens == counts.at(id).at("tokens").get<std::size_t>());
        MESSAGE(id << ": schema block " << block.size() << " bytes, " << block_tokens << " tokens; prompt "
                   << text.size() << " bytes, " << expected.size() << " tokens");
        if (id == "presupuesto") CHECK(block_tokens <= kSchemaTokenLimit);
    }
    llama_model_free(model);
    llama_backend_free();
}

}  // TEST_SUITE
