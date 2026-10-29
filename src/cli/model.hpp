#pragma once
// Model file resolution. No network and no prompts: the download dialog lives in
// cli.cpp.

#include <cstdint>
#include <string>

#include "infer/hw.hpp"

namespace askmdb::cli {

// Data of one model of the family: fetch::model_info(), or the ASKMDB_TEST_MODEL_* hooks.
struct ModelSpec {
    std::string name;  // infer::tier_name()
    std::string filename;
    std::string url;
    std::uint64_t size = 0;
    std::string sha256;
};

// fetch::model_info(tier); with ASKMDB_TEST_HOOKS and ASKMDB_TEST_MODEL_URL set, URL, size
// (ASKMDB_TEST_MODEL_SIZE) and hash (ASKMDB_TEST_MODEL_SHA256) come from the environment, whatever
// the tier (name and file name stay the tier's).
ModelSpec model_spec(infer::ModelTier tier);

// model_spec of the CPU model (askmdb-0.8b).
ModelSpec default_model_spec();

// true if the ASKMDB_TEST_MODEL_* hooks are active (they force resolution with FakeEngine).
bool test_model_hooks();

struct ModelResolution {
    enum class Status {
        Ready,          // path exists (explicit) or is the default one with the right size
        Missing,        // explicit path that does not exist: error, never downloaded
        NeedsDownload,  // default is missing or its size does not match
        NoDir,          // no models directory (neither XDG_DATA_HOME nor HOME)
    };
    Status status = Status::NoDir;
    std::string path;
};

// explicit_path: --model or, failing that, model from the config (empty = default).
// models_dir: fetch::models_dir().
ModelResolution resolve_model(const std::string& explicit_path, const std::string& models_dir,
                              const ModelSpec& spec);

// Decimal megabytes (10^6 bytes) rounded to the nearest, for the dialog (as the model cards).
std::uint64_t size_mb(std::uint64_t bytes);

}  // namespace askmdb::cli
