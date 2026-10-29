#include "cli/model.hpp"

#include <sys/stat.h>

#include <cstdlib>

#include "fetch/model_info.hpp"

namespace askmdb::cli {

namespace {

#if ASKMDB_TEST_HOOKS
const char* env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' ? v : nullptr;
}
#endif

}  // namespace

bool test_model_hooks() {
#if ASKMDB_TEST_HOOKS
    return env("ASKMDB_TEST_MODEL_URL") != nullptr;
#else
    return false;
#endif
}

ModelSpec model_spec(infer::ModelTier tier) {
    const fetch::ModelInfo& m = fetch::model_info(tier);
    ModelSpec spec{m.name, m.filename, m.url, m.size, m.sha256};
#if ASKMDB_TEST_HOOKS
    if (test_model_hooks()) {
        spec.url = env("ASKMDB_TEST_MODEL_URL");
        const char* size = env("ASKMDB_TEST_MODEL_SIZE");
        spec.size = size != nullptr ? std::strtoull(size, nullptr, 10) : 0;
        const char* sha = env("ASKMDB_TEST_MODEL_SHA256");
        spec.sha256 = sha != nullptr ? sha : "";
    }
#endif
    return spec;
}

ModelSpec default_model_spec() { return model_spec(infer::ModelTier::M08B); }

ModelResolution resolve_model(const std::string& explicit_path, const std::string& models_dir,
                              const ModelSpec& spec) {
    ModelResolution r;
    struct stat st{};
    if (!explicit_path.empty()) {
        r.path = explicit_path;
        r.status = ::stat(explicit_path.c_str(), &st) == 0 ? ModelResolution::Status::Ready
                                                             : ModelResolution::Status::Missing;
        return r;
    }
    if (models_dir.empty()) return r;  // NoDir
    r.path = models_dir + "/" + spec.filename;
    const bool ok = ::stat(r.path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                    static_cast<std::uint64_t>(st.st_size) == spec.size;
    r.status = ok ? ModelResolution::Status::Ready : ModelResolution::Status::NeedsDownload;
    return r;
}

std::uint64_t size_mb(std::uint64_t bytes) {
    constexpr std::uint64_t kMB = 1000 * 1000;
    const std::uint64_t mb = (bytes + kMB / 2) / kMB;
    return mb == 0 && bytes > 0 ? 1 : mb;  // a non-empty file never shows as 0 MB
}

}  // namespace askmdb::cli
