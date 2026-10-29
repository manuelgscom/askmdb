#pragma once
// Data of the models askmdb downloads, one per tier of the family. The single place that holds it.

#include <cstddef>
#include <cstdint>

#include "infer/hw.hpp"

namespace askmdb::fetch {

struct ModelInfo {
    const char* name;  // infer::tier_name() of its tier
    const char* filename;
    const char* url;
    std::uint64_t size;
    const char* sha256;
};

// Version of the published model files (part of their names, so an older file with the same size
// in the cache is never taken for these). Independent of the binary's version.
#define ASKMDB_MODELS_VERSION "1.0.0"
inline constexpr const char* kModelsVersion = ASKMDB_MODELS_VERSION;
inline constexpr const char* kModelsQuant = "Q4_K_M";

// Hugging Face revision of each file: pinned to the commit of the 1.0.0 upload of its repo (only the
// middle string literal changes when a new model version is published).
#define ASKMDB_HF_FILE(repo, rev, file) "https://huggingface.co/manuelgscom/" repo "/resolve/" rev "/" file

// The fine-tuned models, Q4_K_M, indexed by infer::ModelTier (the 2B of the family is published
// for llama-server only; the CLI does not use it). -DASKMDB_MODEL_URL=... overrides the 0.8B URL
// at build time (CMakeLists.txt), e.g. for a download smoke test.
inline constexpr ModelInfo kModels[] = {
    {"askmdb-0.8b", "askmdb-0.8b-" ASKMDB_MODELS_VERSION "-Q4_K_M.gguf",
#ifdef ASKMDB_MODEL_URL
     ASKMDB_MODEL_URL,
#else
     ASKMDB_HF_FILE("askmdb-0.8b-GGUF", "87315ab706b7c967135061688be1d564faa0c798", "askmdb-0.8b-" ASKMDB_MODELS_VERSION "-Q4_K_M.gguf"),
#endif
     529298016ULL, "856c3502c6fe4061a77d9a4940a5e36670f35dded1d32cecc298af978cb3509c"},
    {"askmdb-4b", "askmdb-4b-" ASKMDB_MODELS_VERSION "-Q4_K_M.gguf",
     ASKMDB_HF_FILE("askmdb-4b-GGUF", "9f1611ef5bfcbeed9172a54a3b8e9e255fd63eb9", "askmdb-4b-" ASKMDB_MODELS_VERSION "-Q4_K_M.gguf"), 2708805088ULL,
     "2d0a5241e22a772863467530e00afa8be7e78a519559dc77b185c9606db2da14"},
    {"askmdb-9b", "askmdb-9b-" ASKMDB_MODELS_VERSION "-Q4_K_M.gguf",
     ASKMDB_HF_FILE("askmdb-9b-GGUF", "a26b3449ac44f485ccb23ab81783312d5b6ddf38", "askmdb-9b-" ASKMDB_MODELS_VERSION "-Q4_K_M.gguf"), 5629109824ULL,
     "bd2c49de8cb07b93b4b6b961c45e351901426bf6bf96e5d97c18602d2336cba7"},
};
#undef ASKMDB_HF_FILE
static_assert(sizeof(kModels) / sizeof(kModels[0]) == 3, "one model per infer::ModelTier");
static_assert(static_cast<std::size_t>(infer::ModelTier::M08B) == 0 &&
                  static_cast<std::size_t>(infer::ModelTier::M4B) == 1 &&
                  static_cast<std::size_t>(infer::ModelTier::M9B) == 2,
              "kModels is indexed by infer::ModelTier");

constexpr const ModelInfo& model_info(infer::ModelTier tier) { return kModels[static_cast<std::size_t>(tier)]; }

// The CPU model: the base mode and the safe fallback.
inline constexpr const ModelInfo& kDefaultModel = kModels[0];

}  // namespace askmdb::fetch
