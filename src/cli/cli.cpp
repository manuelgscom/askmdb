#include "cli/cli.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "cli/args.hpp"
#include "cli/flow.hpp"
#include "cli/i18n.hpp"
#include "cli/model.hpp"
#include "cli/report.hpp"
#include "cli/ui.hpp"
#include "config/config.hpp"
#include "config/state.hpp"
#include "exec/clipboard.hpp"
#include "exec/term.hpp"
#include "fetch/fetch.hpp"
#include "fetch/model_info.hpp"
#include "infer/cpu.hpp"
#include "infer/engine.hpp"
#include "infer/fake_engine.hpp"
#include "infer/hw.hpp"
#include "infer/llama_engine.hpp"
#include "infer/prompt.hpp"
#include "infer/vk_loader.hpp"
#include "mongo/mongo.hpp"
#include "schema/schema.hpp"
#include "verify/verify.hpp"

// Error codes of the --json "error" field:
//   usage, question_too_long, config, no_uri, invalid_uri, no_db, db_blocked,
//   too_many_collections, schema_too_large, cache_write,
//   mongo_connect, mongo_auth, mongo_timeout, mongo_server,
//   schema_from (--schema-from: unreadable, too large, not a block, or not written as render() does),
//   model_missing, no_models_dir, model_not_downloaded,
//   gpu_unavailable (--device gpu or device = gpu without a usable GPU; also the engine's code),
//   the engine's own codes (invalid_json, invalid_schema, truncated, timeout, too_long,
//   load_failed, decode_failed) and invalid_pipeline (libbson could not read the pipeline).

namespace askmdb::cli {

namespace {

// --- The real process ----------------------------------------------------------------------

void write_fd(int fd, std::string_view text) {
    while (!text.empty()) {
        const ssize_t n = ::write(fd, text.data(), text.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        text.remove_prefix(static_cast<std::size_t>(n));
    }
}

// Canonical read of one stdin line. nullopt = EOF with nothing read.
std::optional<std::string> read_stdin_line() {
    std::string line;
    for (;;) {
        char c = 0;
        const ssize_t n = ::read(0, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::nullopt;
        }
        if (n == 0) {
            if (line.empty()) return std::nullopt;
            return line;
        }
        if (c == '\n') return line;
        line += c;
    }
}

// Ctrl-C at the prompt, during inference or execution: newline and death by SIGINT, so the
// parent shell sees it (130). Async-signal-safe functions only.
extern "C" void on_sigint(int) {
    if (::write(1, "\n", 1) < 0) {
    }
    std::signal(SIGINT, SIG_DFL);
    std::raise(SIGINT);
}

// --- Helpers --------------------------------------------------------------------------------

bool debug() {
    const char* v = std::getenv("ASKMDB_DEBUG");
    return v != nullptr && std::string_view(v) == "1";
}

const char* env_nonempty(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' ? v : nullptr;
}

// FakeEngine (tests only) if ASKMDB_FAKE_* is set; otherwise nullptr.
std::unique_ptr<infer::Engine> fake_engine() {
#if ASKMDB_TEST_HOOKS
    return infer::fake_engine_from_env();
#else
    return nullptr;
#endif
}

const char* kind_name(infer::Suggestion::Kind kind) {
    switch (kind) {
        case infer::Suggestion::Kind::Query: return "query";
        case infer::Suggestion::Kind::Refuse: return "refuse";
        case infer::Suggestion::Kind::Unsure: return "unsure";
    }
    return "unsure";
}

// State of one run: the report for --json and the way out.
struct Run {
    const Host& host;
    UiLang lang;
    bool json;
    Report rep;
    std::optional<mongo::Uri> uri;  // once parsed: everything outgoing is scrubbed with it

    std::string safe(std::string_view text) const {
        return exec::escape_control(uri ? uri->scrub(text) : std::string(text));
    }

    // Writes the --json object (always exactly one) and returns the code.
    int finish(int code) const {
        if (json) host.out(json_report(rep, lang) + "\n");
        return code;
    }

    // Message to stderr (already translated), error code and detail for --json, exit.
    int fail(int code, std::string_view message, std::string error, std::string detail = {}) {
        host.err(std::string(message) + "\n");
        rep.error = std::move(error);
        if (!detail.empty()) rep.error_detail = uri ? uri->scrub(detail) : detail;
        return finish(code);
    }

    int mongo_fail(const mongo::Error& e) {
        Msg msg = Msg::MongoServer;
        const char* code = "mongo_server";
        switch (e.kind) {
            case mongo::Error::Kind::Connect: msg = Msg::MongoConnect; code = "mongo_connect"; break;
            case mongo::Error::Kind::Auth: msg = Msg::MongoAuth; code = "mongo_auth"; break;
            case mongo::Error::Kind::Timeout: msg = Msg::MongoTimeout; code = "mongo_timeout"; break;
            case mongo::Error::Kind::Server: break;
        }
        return fail(kExitMongo, std::string(tr(lang, msg)) + safe(e.message), code, e.message);
    }
};

// Honest message for an engine error (nothing is ever run).
std::string engine_error_message(const infer::EngineResult& r, const std::string& model_path, UiLang lang) {
    const std::string& e = r.error;
    if (e == "invalid_json" || e == "invalid_schema") return std::string(tr(lang, Msg::InvalidResponse));
    if (e == "truncated") return std::string(tr(lang, Msg::InvalidResponse)) + std::string(tr(lang, Msg::Incomplete));
    if (e == "timeout") return std::string(tr(lang, Msg::Timeout));
    if (e == "too_long") return fill(tr(lang, Msg::QuestionTooLong), std::to_string(infer::kMaxQuestionBytes));
    if (e == "load_failed") return std::string(tr(lang, Msg::LoadFailed)) + exec::escape_control(model_path);
    if (e == "gpu_unavailable") return std::string(tr(lang, Msg::GpuUnavailable));
    return std::string(tr(lang, Msg::EngineError)) + exec::escape_control(e);
}

// Model download and resolution. Dialogs need stdin and stderr on a TTY and no --json; Enter = yes.
// Returns the path, or the exit code after r.fail().
struct ModelPick {
    std::string path;
    int exit_code = -1;
};

std::string file_name(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool interactive(const Run& r) { return !r.json && r.host.stdin_tty && r.host.stderr_tty; }

// Asks a [S/n] question on stderr: Enter = yes, EOF = no.
bool ask_yes(const Run& r, std::string_view prompt) {
    r.host.err(prompt);
    const auto line = r.host.read_line();
    if (!line) r.host.err("\n");
    const Answer a = line ? parse_answer(*line) : Answer::No;
    return a == Answer::Yes || a == Answer::Empty;
}

// With memory for both GPU models, the user picks one: 1, "4b" or Enter = askmdb-4b; 2 or "9b" =
// askmdb-9b; n/no (or end of input, or three invalid answers) = none.
std::optional<infer::ModelTier> ask_gpu_model(const Run& r) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        r.host.err(tr(r.lang, Msg::GpuPickPrompt));
        const auto line = r.host.read_line();
        if (!line) {
            r.host.err("\n");
            return std::nullopt;
        }
        const Answer answer = parse_answer(*line);
        if (answer == Answer::Empty) return infer::ModelTier::M4B;
        if (answer == Answer::No) return std::nullopt;
        std::string a;
        for (const char c : *line)
            if (c != ' ' && c != '\t' && c != '\r') a += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (a == "1" || a == "4b" || a == "askmdb-4b") return infer::ModelTier::M4B;
        if (a == "2" || a == "9b" || a == "askmdb-9b") return infer::ModelTier::M9B;
    }
    return std::nullopt;
}

// Downloads spec into dir (the user has already said yes).
ModelPick download_model(const ModelSpec& spec, const std::string& dir, Run& r) {
    const fetch::DownloadResult d = fetch::download(spec.url, dir, spec.filename, spec.size, spec.sha256);
    switch (d.status) {
        case fetch::DownloadResult::Status::Ok:
            r.host.err(std::string(tr(r.lang, Msg::DownloadDone)) + exec::escape_control(d.path) + "\n");
            return {d.path, -1};
        case fetch::DownloadResult::Status::Interrupted:
            r.host.err("\n");
            return {{}, kExitInterrupted};
        case fetch::DownloadResult::Status::Failed:
            break;
    }
    return {{}, r.fail(kExitError, "askmdb: " + (r.lang == UiLang::Es ? d.error : d.error_en), "model_not_downloaded",
                       d.error_en)};
}

// explicit_path (--model or the config's model) or else the file of spec, downloading it if needed
// after asking.
ModelPick pick_model(const std::string& explicit_path, const ModelSpec& spec, Run& r) {
    const UiLang lang = r.lang;
    const std::string dir = fetch::models_dir();
    const ModelResolution res = resolve_model(explicit_path, dir, spec);
    switch (res.status) {
        case ModelResolution::Status::Ready:
            return {res.path, -1};
        case ModelResolution::Status::Missing:
            return {{}, r.fail(kExitError, std::string(tr(lang, Msg::ModelMissing)) + exec::escape_control(res.path),
                               "model_missing")};
        case ModelResolution::Status::NoDir:
            return {{}, r.fail(kExitError, tr(lang, Msg::NoModelsDir), "no_models_dir")};
        case ModelResolution::Status::NeedsDownload:
            break;
    }
    const std::string mb = std::to_string(size_mb(spec.size));
    if (!interactive(r))
        return {{}, r.fail(kExitError, fill_all(tr(lang, Msg::DownloadNoTty), {spec.filename, mb, spec.url}),
                           "model_not_downloaded")};
    r.host.err(fill_all(tr(lang, Msg::DownloadOffer), {spec.filename, mb, spec.url}));
    if (!ask_yes(r, tr(lang, Msg::DownloadPrompt)))
        return {{}, r.fail(kExitError, tr(lang, Msg::DownloadDeclined), "model_not_downloaded")};
    return download_model(spec, dir, r);
}

// --- Device and model of the family --------------------------------------------------------
//
//   device: --device > config device > saved state > detection (first start)
//   model:  --model > config model > saved state > the chosen tier's file
//
// With device auto and a saved CPU choice nothing is detected; a saved GPU is looked up on the
// current hardware (only discrete GPUs are ever used). Vulkan is enabled only when the device is
// (or may be) the GPU, before anything touches ggml.

struct Target {
    infer::Device device = infer::Device::Cpu;
    std::string gpu_id;  // ggml device name (LlamaOptions::gpu_id)
    std::string gpu_name;
    std::uint64_t free_bytes = 0;
    infer::ModelTier tier = infer::ModelTier::M08B;
};

Target target_of(const infer::HwInfo& hw, const infer::Choice& c) {
    Target t;
    t.tier = c.model;
    if (c.device == infer::Device::Gpu && c.gpu >= 0 && static_cast<std::size_t>(c.gpu) < hw.gpus.size()) {
        const infer::GpuInfo& g = hw.gpus[static_cast<std::size_t>(c.gpu)];
        t.device = infer::Device::Gpu;
        t.gpu_id = g.id;
        t.gpu_name = g.name;
        t.free_bytes = g.free_bytes;
    } else if (c.device == infer::Device::Gpu) {
        t.tier = infer::ModelTier::M08B;  // a GPU choice without its GPU: the safe CPU one
    }
    return t;
}

Target target_of(const config::State& s) {
    Target t;
    t.device = s.device;
    t.tier = s.model;
    t.gpu_id = s.gpu_id;
    t.gpu_name = s.gpu_name;
    t.free_bytes = s.vram_free_mib << 20;
    return t;
}

Target cpu_target() { return {}; }

// --device gpu without a usable saved GPU: choose()'s GPU, or else the discrete GPU with the most
// free memory and askmdb-9b or askmdb-4b by that memory. Integrated and software devices are never
// used. nullopt = no discrete GPU.
std::optional<Target> forced_gpu(const infer::HwInfo& hw) {
    const infer::Choice c = infer::choose(hw);
    if (c.device == infer::Device::Gpu) {
        const Target t = target_of(hw, c);
        if (t.device == infer::Device::Gpu) return t;
    }
    int best = -1;
    for (std::size_t i = 0; i < hw.gpus.size(); ++i) {
        if (hw.gpus[i].kind != infer::GpuKind::Discrete) continue;
        if (best < 0 || hw.gpus[i].free_bytes > hw.gpus[static_cast<std::size_t>(best)].free_bytes)
            best = static_cast<int>(i);
    }
    if (best < 0) return std::nullopt;
    const std::uint64_t free = hw.gpus[static_cast<std::size_t>(best)].free_bytes;
    const infer::ModelTier tier = free >= infer::kMin9bFreeBytes ? infer::ModelTier::M9B : infer::ModelTier::M4B;
    return target_of(hw, infer::Choice{infer::Device::Gpu, best, tier});
}

// The saved GPU on the current hardware: the discrete GPU with the saved id and name; else, if
// the id moved, the only discrete GPU with that name. The saved tier is kept. nullopt = the GPU
// is not there (or is ambiguous): treated as gpu_unavailable.
std::optional<Target> saved_gpu(const infer::HwInfo& hw, const config::State& s) {
    int by_name = -1;
    int named = 0;
    for (std::size_t i = 0; i < hw.gpus.size(); ++i) {
        const infer::GpuInfo& g = hw.gpus[i];
        if (g.kind != infer::GpuKind::Discrete || config::saved_name(g.name) != s.gpu_name) continue;
        if (g.id == s.gpu_id) return target_of(hw, infer::Choice{infer::Device::Gpu, static_cast<int>(i), s.model});
        by_name = static_cast<int>(i);
        ++named;
    }
    if (named != 1) return std::nullopt;
    return target_of(hw, infer::Choice{infer::Device::Gpu, by_name, s.model});
}

std::string mib(std::uint64_t bytes) { return std::to_string(bytes >> 20); }

// "la CPU" / "la GPU <name>".
std::string where(const Target& t, UiLang lang) {
    if (t.device == infer::Device::Cpu) return std::string(tr(lang, Msg::OnCpu));
    return fill(tr(lang, Msg::OnGpu), exec::escape_control(t.gpu_name.empty() ? t.gpu_id : t.gpu_name));
}

const char* kind_id(infer::GpuKind kind) {
    switch (kind) {
        case infer::GpuKind::Discrete: return "discrete";
        case infer::GpuKind::Integrated: return "integrated";
        case infer::GpuKind::Software: return "software";
    }
    return "software";
}

Msg kind_msg(infer::GpuKind kind) {
    switch (kind) {
        case infer::GpuKind::Discrete: return Msg::KindDiscrete;
        case infer::GpuKind::Integrated: return Msg::KindIntegrated;
        case infer::GpuKind::Software: return Msg::KindSoftware;
    }
    return Msg::KindSoftware;
}

std::string hw_summary(const infer::HwInfo& hw, UiLang lang) {
    std::string out(tr(lang, Msg::HwHeader));
    // Detection always enables Vulkan, so a Vulkan build without it means there is no loader.
    out += tr(lang, hw.vulkan            ? Msg::HwVulkanYes
                    : infer::vulkan_built() ? Msg::HwVulkanMissing
                                            : Msg::HwVulkanNo);
    if (hw.gpus.empty()) out += tr(lang, Msg::HwNoGpu);
    for (const auto& g : hw.gpus)
        out += fill_all(tr(lang, Msg::HwGpu), {exec::escape_control(g.id), exec::escape_control(g.name),
                                               tr(lang, kind_msg(g.kind)), mib(g.free_bytes), mib(g.total_bytes)});
    out += fill_all(tr(lang, Msg::HwRam),
                    {mib(hw.ram_available_bytes), mib(hw.ram_total_bytes), std::to_string(hw.cores)});
    return out;
}

// --detect --json. The keys are fixed (the release smoke test reads them).
std::string detect_json(const infer::HwInfo& hw, const infer::Choice& c) {
    using Json = nlohmann::ordered_json;
    Json gpus = Json::array();
    for (const auto& g : hw.gpus)
        gpus.push_back({{"id", g.id},
                        {"name", g.name},
                        {"kind", kind_id(g.kind)},
                        {"free_mib", g.free_bytes >> 20},
                        {"total_mib", g.total_bytes >> 20}});
    const Target t = target_of(hw, c);
    Json j;
    j["vulkan"] = hw.vulkan;
    j["gpus"] = gpus;
    j["ram_total_mib"] = hw.ram_total_bytes >> 20;
    j["ram_available_mib"] = hw.ram_available_bytes >> 20;
    j["cores"] = hw.cores;
    j["choice"] = {{"device", std::string(infer::device_name(t.device))},
                   {"gpu", t.device == infer::Device::Gpu ? Json(t.gpu_id) : Json(nullptr)},
                   {"model", std::string(infer::tier_name(t.tier))}};
    return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

#if ASKMDB_TEST_HOOKS
// ASKMDB_TEST_GPU_ERROR=<code> (tests only): every suggestion on the GPU fails with that engine
// error, to exercise the CPU fallbacks without a GPU.
class GpuErrorEngine final : public infer::Engine {
public:
    explicit GpuErrorEngine(std::string code) : code_(std::move(code)) {}
    infer::EngineResult suggest(const infer::Request&) override {
        infer::EngineResult r;
        r.error = code_;
        r.error_detail = "ASKMDB_TEST_GPU_ERROR";
        return r;
    }

private:
    std::string code_;
};
#endif

infer::LlamaOptions llama_options(std::string model_path, const config::Config& cfg);

// The engine for a model on a target. With FakeEngine (tests) a new FakeEngine.
std::unique_ptr<infer::Engine> make_engine(const std::string& model_path, const Target& t, const config::Config& cfg,
                                           bool fake) {
#if ASKMDB_TEST_HOOKS
    if (t.device == infer::Device::Gpu)
        if (const char* code = env_nonempty("ASKMDB_TEST_GPU_ERROR")) return std::make_unique<GpuErrorEngine>(code);
#endif
    if (fake) return fake_engine();
    infer::LlamaOptions o = llama_options(model_path, cfg);
    o.device = t.device;
    o.gpu_id = t.gpu_id;
    return infer::make_llama_engine(o);
}

// One short suggestion over a tiny built-in schema: any well-formed answer passes (also refuse
// or unsure). It loads the model, so the engine is kept for the user's question.
// On a GPU the driver compiles its shaders for each prompt shape the first time it is used
// (~11 s on a 16 GB NVIDIA GPU, saved on disk afterwards): there the block is about as long as a real
// schema (~1,500 tokens) and the question goes twice (prefix state missing, then present), so the
// preparation happens here and not in the user's first questions. On the CPU, one short request.
infer::EngineResult self_check(infer::Engine& engine, infer::Device device) {
    static const std::string kShort = "db=prueba\n## pedidos\n_id oid\nfecha date\nimporte double\n";
    static const std::string kLong = [] {
        std::string b = kShort;
        for (int i = 0; i < 200; ++i) b += "campo_" + std::to_string(100 + i) + " str\n";
        return b;
    }();
    const bool gpu = device == infer::Device::Gpu;
    const infer::Request request{gpu ? kLong : kShort, "cuántos pedidos hay"};
    const infer::EngineResult first = engine.suggest(request);
    if (!gpu || !first.suggestion) return first;
    return engine.suggest(request);
}

// What the run uses. engine is set when the first start already loaded the model.
struct Setup {
    Target target;
    std::string model_path;
    std::optional<std::string> model_name;  // the tier's name when the file is one of the family
    std::unique_ptr<infer::Engine> engine;
    bool forced = false;                    // --device or config device: no fallback to the CPU
    std::string explicit_path;
    int exit_code = -1;
};

Setup setup_fail(int code) {
    Setup s;
    s.exit_code = code;
    return s;
}

// The family's name of a GGUF file given by path (e.g. --model .../askmdb-4b-Q4_K_M.gguf).
std::optional<std::string> family_name(const std::string& path) {
    const std::string base = file_name(path);
    for (const auto& m : fetch::kModels)
        if (base == m.filename) return std::string(m.name);
    return std::nullopt;
}

void save_choice(const Target& t, const config::Config& cfg, Run& r, bool tell) {
    config::State st;
    st.device = t.device;
    st.model = t.tier;
    st.gpu_id = t.gpu_id;
    st.gpu_name = t.gpu_name;
    st.vram_free_mib = t.free_bytes >> 20;
    st.date = config::utc_now_iso();
    std::string error;
    if (!config::save_state(st, error)) {
        r.host.err(std::string(tr(r.lang, Msg::ChoiceNotSaved)) + exec::escape_control(error) + "\n");
        return;
    }
    if (!tell) return;
    r.host.err(fill_all(tr(r.lang, Msg::ChoiceSaved), {infer::tier_name(t.tier), where(t, r.lang)}));
    // Shader compilation and a cold read of a multi-GB model make the first answers slower.
    if (t.device == infer::Device::Gpu) r.host.err(tr(r.lang, Msg::GpuWarmup));
    if (cfg.device != config::DeviceMode::Auto || !cfg.model.empty()) r.host.err(tr(r.lang, Msg::ConfigOverrides));
}

// First start (and --detect): detect, propose, download, check and save. Interactive: asks before
// anything is downloaded, offers askmdb-0.8b on the CPU when a GPU model is declined, and falls
// back to it when the GPU fails the check. Not interactive (no terminal, or --json): never
// downloads and never saves (nothing was checked); uses the chosen model if its file is already
// there, else askmdb-0.8b on the CPU if that one is, else fails naming the chosen model.
Setup first_start(const config::Config& cfg, Run& r, bool fake) {
    const UiLang lang = r.lang;
    infer::vulkan_set_enabled(true);  // detection needs it
    const infer::HwInfo hw = infer::detect_hardware();
    const Target chosen = target_of(hw, infer::choose(hw));
    const std::string dir = fetch::models_dir();
    if (dir.empty()) return setup_fail(r.fail(kExitError, tr(lang, Msg::NoModelsDir), "no_models_dir"));

    if (!interactive(r)) {
        std::vector<Target> candidates{chosen};
        if (chosen.tier == infer::ModelTier::M9B) {  // the user was not asked: the default (4B) first
            Target four = chosen;
            four.tier = infer::ModelTier::M4B;
            candidates.insert(candidates.begin(), four);
        }
        if (chosen.device == infer::Device::Gpu) candidates.push_back(cpu_target());
        for (const Target& c : candidates) {
            const ModelSpec spec = model_spec(c.tier);
            const ModelResolution res = resolve_model("", dir, spec);
            if (res.status != ModelResolution::Status::Ready) continue;
            Setup s;
            s.target = c;
            s.model_path = res.path;
            s.model_name = spec.name;
            return s;
        }
        const ModelSpec spec = model_spec(candidates.front().tier);  // the recommended one
        return setup_fail(r.fail(kExitError,
                                 fill_all(tr(lang, Msg::DownloadNoTty),
                                          {spec.filename, std::to_string(size_mb(spec.size)), spec.url}),
                                 "model_not_downloaded"));
    }

    r.host.err(hw_summary(hw, lang));
    std::vector<Target> candidates{chosen};
    if (chosen.device == infer::Device::Gpu) candidates.push_back(cpu_target());
    // With memory for the 9B, the user picks 4B or 9B (choosing one is the yes to download it).
    std::size_t first = 0;
    bool picked = false;
    if (chosen.tier == infer::ModelTier::M9B) {
        r.host.err(fill(tr(lang, Msg::GpuPickHeader), exec::escape_control(chosen.gpu_name)));
        int n = 1;
        for (const auto& [tier, note] : {std::pair{infer::ModelTier::M4B, Msg::GpuPick4b},
                                         std::pair{infer::ModelTier::M9B, Msg::GpuPick9b}}) {
            const ModelSpec spec = model_spec(tier);
            r.host.err(fill_all(tr(lang, Msg::GpuPickOption), {std::to_string(n++), spec.name,
                                                                std::to_string(size_mb(spec.size)), tr(lang, note)}));
        }
        if (const std::optional<infer::ModelTier> tier = ask_gpu_model(r)) {
            candidates[0].tier = *tier;
            picked = true;
        } else {
            first = 1;  // none of the two: the CPU offer
        }
    }
    std::optional<Target> accepted;
    for (std::size_t i = first; i < candidates.size() && !accepted; ++i) {
        const Target& c = candidates[i];
        const ModelSpec spec = model_spec(c.tier);
        const std::string mb = std::to_string(size_mb(spec.size));
        const bool present = resolve_model("", dir, spec).status == ModelResolution::Status::Ready;
        if (i == 0 && !picked) r.host.err(fill_all(tr(lang, Msg::Proposal), {spec.name, where(c, lang), mb}));
        if (present) r.host.err(fill(tr(lang, Msg::ModelPresent), exec::escape_control(dir + "/" + spec.filename)));
        else r.host.err(fill_all(tr(lang, Msg::ModelSource), {spec.filename, spec.url}));
        if (i == 0 && picked) {
            accepted = c;
            break;
        }
        const std::string prompt = i > 0     ? fill_all(tr(lang, Msg::OfferCpu), {spec.name, mb})
                                   : present ? std::string(tr(lang, Msg::UsePrompt))
                                             : std::string(tr(lang, Msg::DownloadPrompt));
        if (ask_yes(r, prompt)) accepted = c;
    }
    if (!accepted) return setup_fail(r.fail(kExitError, tr(lang, Msg::FirstStartDeclined), "model_not_downloaded"));

    Setup s;
    s.target = *accepted;
    for (;;) {
        const ModelSpec spec = model_spec(s.target.tier);
        const ModelResolution res = resolve_model("", dir, spec);
        if (res.status == ModelResolution::Status::Ready) {
            s.model_path = res.path;
        } else {
            const ModelPick d = download_model(spec, dir, r);
            if (d.exit_code >= 0) return setup_fail(d.exit_code);
            s.model_path = d.path;
        }
        s.model_name = spec.name;
        r.host.err(fill(tr(lang, Msg::SelfCheck), where(s.target, lang)));
        if (s.target.device == infer::Device::Gpu) r.host.err(tr(lang, Msg::SelfCheckGpuNote));
        s.engine = make_engine(s.model_path, s.target, cfg, fake);
        const infer::EngineResult check = self_check(*s.engine, s.target.device);
        if (check.suggestion) break;
        if (debug()) r.host.err("askmdb: error_detail: " + exec::escape_control(check.error_detail) + "\n");
        if (s.target.device == infer::Device::Cpu)
            return setup_fail(r.fail(kExitError, fill(tr(lang, Msg::SelfCheckFailed), exec::escape_control(check.error)),
                                     check.error, check.error_detail));
        // The GPU failed: askmdb-0.8b on the CPU, asking first if it has to be downloaded.
        r.host.err(fill(tr(lang, Msg::SelfCheckGpuFailed), exec::escape_control(check.error)));
        s.engine.reset();
        s.target = cpu_target();
        const ModelSpec cpu = model_spec(s.target.tier);
        if (resolve_model("", dir, cpu).status != ModelResolution::Status::Ready) {
            r.host.err(fill_all(tr(lang, Msg::ModelSource), {cpu.filename, cpu.url}));
            if (!ask_yes(r, fill_all(tr(lang, Msg::OfferCpu), {cpu.name, std::to_string(size_mb(cpu.size))})))
                return setup_fail(r.fail(kExitError, tr(lang, Msg::FirstStartDeclined), "model_not_downloaded"));
        }
    }
    save_choice(s.target, cfg, r, true);
    return s;
}

int fall_back_to_cpu(Setup& s, Run& r);

// Device and model of a run (the order is at the top of this section). A saved GPU is looked up
// on the current hardware (saved_gpu); if it is gone, the CPU fallback without a forced device.
Setup resolve_setup(const Args& args, const config::Config& cfg, Run& r, bool fake) {
    const config::DeviceMode mode = args.device.value_or(cfg.device);
    const std::string explicit_path = args.model ? *args.model : cfg.model;
    const std::optional<config::State> state = config::load_state();
    Target t;
    bool gpu_gone = false;  // the saved GPU is not on this machine now: the CPU fallback
    if (mode == config::DeviceMode::Cpu) {
        infer::vulkan_set_enabled(false);
        if (state && state->device == infer::Device::Cpu) t.tier = state->model;
    } else if (mode == config::DeviceMode::Gpu) {
        infer::vulkan_set_enabled(true);
        const infer::HwInfo hw = infer::detect_hardware();
        std::optional<Target> g;
        if (state && state->device == infer::Device::Gpu) g = saved_gpu(hw, *state);
        if (!g) g = forced_gpu(hw);
        if (!g) return setup_fail(r.fail(kExitError, tr(r.lang, Msg::GpuUnavailable), "gpu_unavailable"));
        t = *g;
    } else if (state && state->device == infer::Device::Gpu) {
        infer::vulkan_set_enabled(true);
        const std::optional<Target> g = saved_gpu(infer::detect_hardware(), *state);
        t = g ? *g : target_of(*state);
        gpu_gone = !g;
    } else if (state) {
        infer::vulkan_set_enabled(false);
        t = target_of(*state);
    } else if (explicit_path.empty()) {
        Setup s = first_start(cfg, r, fake);
        return s;
    } else {
        // A model of one's own and nothing chosen yet: the device detection would choose, unsaved.
        infer::vulkan_set_enabled(true);
        const infer::HwInfo hw = infer::detect_hardware();
        t = target_of(hw, infer::choose(hw));
    }
    Setup s;
    s.target = t;
    s.forced = mode != config::DeviceMode::Auto;
    s.explicit_path = explicit_path;
    // The saved GPU's tier is not looked up (nor offered for download) when it cannot run.
    if (!gpu_gone || !explicit_path.empty()) {
        const ModelSpec spec = model_spec(t.tier);
        const ModelPick m = pick_model(explicit_path, spec, r);
        if (m.exit_code >= 0) return setup_fail(m.exit_code);
        s.model_path = m.path;
        s.model_name = explicit_path.empty() ? std::optional<std::string>(spec.name) : family_name(explicit_path);
    }
    if (gpu_gone)
        if (const int code = fall_back_to_cpu(s, r); code >= 0) return setup_fail(code);
    return s;
}

// A saved (or detected) GPU that is not there at run time: warn and use the CPU, with the user's
// own model or askmdb-0.8b (offered for download in a terminal). The saved choice is kept: the GPU
// may come back. Returns -1, or the exit code after r.fail().
int fall_back_to_cpu(Setup& s, Run& r) {
    const UiLang lang = r.lang;
    const ModelSpec cpu = default_model_spec();
    const std::string shown = s.target.gpu_name.empty() ? s.target.gpu_id : s.target.gpu_name;
    r.host.err(fill_all(tr(lang, Msg::GpuFallback),
                        {exec::escape_control(shown), s.explicit_path.empty() ? cpu.name : file_name(s.explicit_path)}));
    s.target = cpu_target();
    if (!s.explicit_path.empty()) return -1;
    const std::string dir = fetch::models_dir();
    const ModelResolution res = resolve_model("", dir, cpu);
    s.model_name = cpu.name;
    if (res.status == ModelResolution::Status::Ready) {
        s.model_path = res.path;
        return -1;
    }
    if (res.status == ModelResolution::Status::NoDir) return r.fail(kExitError, tr(lang, Msg::NoModelsDir), "no_models_dir");
    const std::string mb = std::to_string(size_mb(cpu.size));
    if (!interactive(r))
        return r.fail(kExitError, fill_all(tr(lang, Msg::DownloadNoTty), {cpu.filename, mb, cpu.url}),
                      "model_not_downloaded");
    r.host.err(fill_all(tr(lang, Msg::ModelSource), {cpu.filename, cpu.url}));
    if (!ask_yes(r, fill_all(tr(lang, Msg::OfferCpu), {cpu.name, mb})))
        return r.fail(kExitError, tr(lang, Msg::FirstStartDeclined), "model_not_downloaded");
    const ModelPick d = download_model(cpu, dir, r);
    if (d.exit_code >= 0) return d.exit_code;
    s.model_path = d.path;
    return -1;
}

// --detect: --json prints only the hardware and the choice; without a terminal, the same as text;
// in a terminal, the first start again. The saved choice is only replaced when it succeeds (the
// write is atomic): declining, Ctrl-C or a failed download keep it. Nothing is asked about a
// question.
int run_detect(const config::Config& cfg, Run& r, bool fake) {
    if (r.json || !interactive(r)) {
        infer::vulkan_set_enabled(true);
        const infer::HwInfo hw = infer::detect_hardware();
        const infer::Choice c = infer::choose(hw);
        if (r.json) {
            r.host.out(detect_json(hw, c) + "\n");
        } else {
            const Target t = target_of(hw, c);
            r.host.out(hw_summary(hw, r.lang) +
                       fill_all(tr(r.lang, Msg::ChoiceLine), {infer::tier_name(t.tier), where(t, r.lang)}));
        }
        return kExitOk;
    }
    const Setup s = first_start(cfg, r, fake);
    return s.exit_code >= 0 ? s.exit_code : kExitOk;
}

bool contains(const std::vector<std::string>& v, std::string_view s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

bool has_collection(const schema::Schema& s, std::string_view name) {
    for (const auto& c : s.collections)
        if (c.name == name) return true;
    return false;
}

// --schema-from: the file must be a block exactly as render() writes it, so that a snapshot is
// never read differently from how it was written. Any failure is a usage error (2).
inline constexpr std::size_t kMaxSchemaFileBytes = 1 << 20;

struct Snapshot {
    std::optional<schema::Schema> schema;
    int exit_code = -1;
};

Snapshot load_snapshot(const std::string& path, Run& r) {
    const UiLang lang = r.lang;
    const std::string shown = exec::escape_control(path);
    const auto fail = [&](std::string message, std::string detail) {
        return Snapshot{std::nullopt, r.fail(kExitUsage, message, "schema_from", std::move(detail))};
    };
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        const std::string why = std::strerror(errno);
        return fail(fill_all(tr(lang, Msg::SchemaFromRead), {shown, why}), "cannot read: " + why);
    }
    // Read up to one byte past the cap (the file may be a pipe, so its size is not trusted).
    std::string block;
    char buf[65536];
    int read_errno = 0;
    while (block.size() <= kMaxSchemaFileBytes) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) read_errno = errno;
        if (n <= 0) break;
        block.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    if (read_errno != 0) {
        const std::string why = std::strerror(read_errno);
        return fail(fill_all(tr(lang, Msg::SchemaFromRead), {shown, why}), "cannot read: " + why);
    }
    if (block.size() > kMaxSchemaFileBytes) {
        const std::string max = std::to_string(kMaxSchemaFileBytes);
        return fail(fill_all(tr(lang, Msg::SchemaFromTooLarge), {shown, max}), "larger than " + max + " bytes");
    }
    schema::ParseResult p = schema::parse(block);
    if (!p.schema)
        return fail(fill_all(tr(lang, Msg::SchemaFromInvalid), {shown, exec::escape_control(p.error)}), p.error);
    if (schema::render(*p.schema) != block)
        return fail(fill(tr(lang, Msg::SchemaFromRender), shown), "render(parse(block)) != block");
    return {std::move(p.schema), -1};
}

// Schema of the database: fresh cache, plus a sampling of the selected collections it lacks
// (saved back to the cache). Fills full and selected; returns -1, or the exit code after r.fail().
int load_schema(const Args& args, const config::Config& cfg, Run& r, mongo::Client& client, schema::Schema& full,
                std::vector<std::string>& selected) {
    const UiLang lang = r.lang;
    const std::string cache_file = schema::cache_path(r.uri->host_key(), full.db);
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    std::optional<schema::CacheEntry> cached;
    if (!args.refresh && !cache_file.empty()) {
        schema::CacheRead cr = schema::read_cache(cache_file, now);
        if (cr.status == schema::CacheStatus::Fresh && cr.entry) cached = std::move(cr.entry);
    }

    if (args.collections) {
        selected = *args.collections;
    } else if (!cfg.collections.empty()) {
        selected = cfg.collections;
    } else if (cached) {
        for (const auto& c : cached->schema.collections) selected.push_back(c.name);
    } else {
        auto list = client.list_collections();
        if (!list.value) return r.mongo_fail(list.error);
        if (list.value->size() > mongo::kMaxCollections) {
            std::string msg = fill_all(tr(lang, Msg::TooManyCollections),
                                       {std::to_string(list.value->size()), std::to_string(mongo::kMaxCollections)});
            for (const auto& name : *list.value) msg += "\n  " + exec::escape_control(name);
            return r.fail(kExitUsage, msg, "too_many_collections");
        }
        selected = std::move(*list.value);
    }

    if (cached) full = cached->schema;
    const std::int64_t sampled_at = cached ? cached->sampled_at : now;
    bool sampled = false;
    for (const auto& name : selected) {
        if (has_collection(full, name)) continue;
        auto docs = client.sample(name);
        if (!docs.value) return r.mongo_fail(docs.error);
        std::vector<const bson_t*> ptrs;
        ptrs.reserve(docs.value->size());
        for (const auto& d : *docs.value) ptrs.push_back(d.get());
        schema::upsert(full, schema::infer(name, ptrs));
        sampled = true;
    }
    if (sampled && !cache_file.empty()) {
        std::string write_error;
        if (!schema::write_cache(cache_file, schema::CacheEntry{full, sampled_at}, write_error))
            return r.fail(kExitError, std::string(tr(lang, Msg::CacheWrite)) + r.safe(write_error), "cache_write",
                          write_error);
    }
    return -1;
}

// Options of the real engine. ASKMDB_TEST_TIMEOUT_MS (test builds only) lowers the timeout.
infer::LlamaOptions llama_options(std::string model_path, const config::Config& cfg) {
    infer::LlamaOptions o;
    o.model_path = std::move(model_path);
    o.threads = cfg.threads > 0 ? cfg.threads : infer::default_threads();
    o.debug = debug();
#if ASKMDB_TEST_HOOKS
    if (const char* t = env_nonempty("ASKMDB_TEST_TIMEOUT_MS"))
        o.timeout = std::chrono::milliseconds(std::strtoll(t, nullptr, 10));
#endif
    return o;
}

// --version: the first line is "askmdb <version>"; release/build.sh and release/smoke.sh grep the
// "TLS:" and "Vulkan:" lines.
std::string version_text() {
    std::string models;
    for (const auto& m : fetch::kModels) models += std::string(models.empty() ? "" : ", ") + m.name;
    return std::string("askmdb " ASKMDB_VERSION "\nTLS: ") + (ASKMDB_HAVE_TLS ? "yes" : "no") +
           "\nVulkan: " + (infer::vulkan_built() ? "yes" : "no") + "\nModels: " + models + " " +
           fetch::kModelsVersion + " (" + fetch::kModelsQuant + ")\n" + mongo::library_versions() + "\n";
}

// Usage error before the run starts: message, blank line and help to stderr.
int usage(const Host& host, UiLang lang, const std::string& message) {
    host.err(message + "\n\n");
    host.err(tr(lang, Msg::Help));
    return kExitUsage;
}

}  // namespace

int run(const std::vector<std::string>& argv, const Host& host) {
    const Args args = parse_args(argv);

    // Before reading the config, the language comes from --lang or the environment.
    const UiLang env_lang = ui_lang_from_env(args.lang.value_or(config::Lang::Auto));
    switch (args.action) {
        case Args::Action::Help:
            host.out(tr(env_lang, Msg::Help));
            return kExitOk;
        case Args::Action::Version:
            host.out(version_text());
            return kExitOk;
        case Args::Action::BadFlag:
            return usage(host, env_lang, std::string(tr(env_lang, Msg::UnknownFlag)) + exec::escape_control(args.bad_flag));
        case Args::Action::MissingValue:
            return usage(host, env_lang, std::string(tr(env_lang, Msg::MissingValue)) + args.bad_flag);
        case Args::Action::BadValue:
            return usage(host, env_lang,
                         fill_all(tr(env_lang, Msg::BadValue), {args.bad_flag, exec::escape_control(args.bad_value)}));
        case Args::Action::Conflict:
            if (args.bad_flag == "--detect") return usage(host, env_lang, std::string(tr(env_lang, Msg::DetectQuestion)));
            return usage(host, env_lang, std::string(tr(env_lang, Msg::Conflict)) + args.bad_flag);
        case Args::Action::Run:
            break;
    }

    Run r{host, env_lang, args.json, {}, std::nullopt};

    // 1. Question: normalized; needed unless --print-schema.
    const std::string question = infer::normalize_request(args.question);
    r.rep.question = question;
    if (!args.print_schema && !args.detect) {
        if (question.empty()) {
            host.err(tr(env_lang, Msg::Help));
            r.rep.error = "usage";
            return r.finish(kExitUsage);
        }
        if (question.size() > infer::kMaxQuestionBytes)
            return r.fail(kExitUsage, fill(tr(env_lang, Msg::QuestionTooLong), std::to_string(infer::kMaxQuestionBytes)),
                          "question_too_long");
    }

    // 2. Config and language.
    const config::ConfigResult cfg_result = config::load_config();
    if (!cfg_result.config)
        return r.fail(kExitError, "askmdb: " + (env_lang == UiLang::Es ? cfg_result.error : cfg_result.error_en),
                      "config", cfg_result.error_en);
    const config::Config& cfg = *cfg_result.config;
    r.lang = ui_lang_from_env(args.lang ? *args.lang : cfg.lang);
    const UiLang lang = r.lang;
    const config::Mode mode = args.mode.value_or(cfg.mode);

    // --detect: hardware and choice only; no question, schema or MongoDB.
    if (args.detect) return run_detect(cfg, r, fake_engine() != nullptr);

    // A device fixed to the CPU never initialises Vulkan (nothing has touched ggml yet).
    if (args.device.value_or(cfg.device) == config::DeviceMode::Cpu) infer::vulkan_set_enabled(false);

    // 3. --schema-from: read and checked first, since it gives the database and decides whether
    // a URI is needed at all.
    std::optional<schema::Schema> snapshot;
    if (args.schema_from) {
        const Snapshot snap = load_snapshot(*args.schema_from, r);
        if (snap.exit_code >= 0) return snap.exit_code;
        snapshot = snap.schema;
    }

    // 4. URI and database: --uri > ASKMDB_URI > config. With --schema-from the URI is only for
    // running the pipeline (the database is the block's), so without a chance of running nothing
    // is resolved and nothing connects.
    const bool may_run = !args.print_schema && !args.print_prompt && mode != config::Mode::Suggest &&
                         (args.json ? mode == config::Mode::Auto : host.stdin_tty && host.stdout_tty);
    if (!snapshot || may_run) {
        std::string uri_text;
        if (args.uri) uri_text = *args.uri;
        else if (const char* e = env_nonempty("ASKMDB_URI")) uri_text = e;
        else uri_text = cfg.uri;
        if (uri_text.empty()) return r.fail(kExitUsage, tr(lang, Msg::NoUri), "no_uri");
        if (args.uri && config::uri_has_password(uri_text))
            host.err(std::string(tr(lang, Msg::UriPasswordWarning)) + "\n");
        std::string uri_error;
        r.uri = mongo::Uri::parse(uri_text, uri_error);
        if (!r.uri)
            return r.fail(kExitUsage, std::string(tr(lang, Msg::InvalidUri)) + exec::escape_control(uri_error),
                          "invalid_uri", uri_error);
        r.rep.hosts = r.uri->hosts;
    } else {
        r.rep.hosts = std::vector<std::string>{};
    }
    const std::string db = snapshot ? snapshot->db : args.db ? *args.db : r.uri->db;
    r.rep.db = db;
    if (db.empty()) return r.fail(kExitUsage, tr(lang, Msg::NoDb), "no_db");
    if (const auto reason = verify::check_db(db)) {
        r.rep.assessment = verify::Assessment{verify::Level::Blocked, {*reason}};
        return r.fail(kExitBlocked,
                      fill_all(tr(lang, Msg::DbBlocked), {exec::escape_control(db), reason_text(*reason, lang)}),
                      "db_blocked");
    }

    // 5. Schema: the snapshot, or the fresh cache plus a sampling of what is missing. The client
    // only connects when used.
    std::optional<mongo::Client> client;
    if (r.uri) client.emplace(*r.uri, db);
    schema::Schema full{db, {}};
    std::vector<std::string> selected;
    if (snapshot) {
        full = std::move(*snapshot);
        if (args.collections) selected = *args.collections;
        else if (!cfg.collections.empty()) selected = cfg.collections;
        else
            for (const auto& c : full.collections) selected.push_back(c.name);
    } else if (const int code = load_schema(args, cfg, r, *client, full, selected); code >= 0) {
        return code;
    }

    // 6. Block for the model: only the selected collections, within the budget.
    schema::Schema subset{db, {}};
    for (const auto& c : full.collections)
        if (contains(selected, c.name)) subset.collections.push_back(c);
    const schema::Fit fit = schema::render(subset, schema::kSchemaBudgetBytes);
    if (!fit.ok) {
        std::string msg(tr(lang, Msg::SchemaTooLarge));
        for (const auto& c : fit.costs)
            msg += "\n" + fill_all(tr(lang, Msg::CostLine), {exec::escape_control(c.name), std::to_string(c.tokens)});
        msg += "\n";
        msg += tr(lang, Msg::CollectionsHint);
        return r.fail(kExitUsage, msg, "schema_too_large");
    }
    if (args.print_schema) {
        // The block is plain text (schema::plain_text): escape_control leaves it byte for byte.
        host.out(exec::escape_control(fit.block));
        return kExitOk;
    }
    const infer::Request request{fit.block, question};
    if (args.print_prompt) {
        // Raw, not through escape_control: it is exactly what the model reads (snapshots, the
        // evaluation harness). The block is plain text; only the question, typed by the user,
        // may carry control characters, and the model gets them as they are.
        host.out(infer::render_prompt(request));
        return kExitOk;
    }

    // 7. Engine. FakeEngine (tests) takes precedence; with it the model is not looked up, unless
    // --model or the ASKMDB_TEST_MODEL_* hooks are given. Otherwise device and model as in
    // resolve_setup (first start included).
    std::unique_ptr<infer::Engine> engine = fake_engine();
    const bool fake = engine != nullptr;
    std::string model_path;
    Setup setup;
    const bool resolved = !engine || args.model || test_model_hooks();
    if (resolved) {
        setup = resolve_setup(args, cfg, r, fake);
        if (setup.exit_code >= 0) return setup.exit_code;
        model_path = setup.model_path;
        r.rep.device = std::string(infer::device_name(setup.target.device));
        r.rep.model_name = setup.model_name;
        engine = setup.engine ? std::move(setup.engine) : make_engine(model_path, setup.target, cfg, fake);
    }

    // 8. Suggestion. A GPU that is gone, out of memory or failing to decode when it was not
    // forced: the CPU.
    infer::EngineResult result = engine->suggest(request);
    if (resolved && !setup.forced && setup.target.device == infer::Device::Gpu &&
        (result.error == "gpu_unavailable" || result.error == "load_failed" || result.error == "decode_failed")) {
        if (debug()) host.err("askmdb: error_detail: " + exec::escape_control(result.error_detail) + "\n");
        engine.reset();
        if (const int code = fall_back_to_cpu(setup, r); code >= 0) return code;
        model_path = setup.model_path;
        r.rep.device = std::string(infer::device_name(setup.target.device));
        r.rep.model_name = setup.model_name;
        engine = make_engine(model_path, setup.target, cfg, fake);
        result = engine->suggest(request);
    }
    r.rep.engine = EngineInfo{fake ? std::nullopt : std::optional<std::string>(file_name(model_path)), result.raw,
                              result.timings, result.prefix_cache};
    if (!result.suggestion) {
        if (debug()) {
            host.err("askmdb: error_detail: " + exec::escape_control(result.error_detail) + "\n");
            host.err("askmdb: raw: " + exec::escape_control(result.raw) + "\n");
        }
        return r.fail(kExitError, engine_error_message(result, model_path, lang), result.error, result.error_detail);
    }
    const infer::Suggestion& s = *result.suggestion;
    r.rep.type = kind_name(s.kind);
    r.rep.collection = s.collection;
    r.rep.explanation = s.explanation;
    r.rep.pipeline = s.pipeline.bson;

    const bool color = use_color(host.stdout_tty, std::getenv("NO_COLOR"), std::getenv("TERM"));
    if (s.kind != infer::Suggestion::Kind::Query) {
        if (!args.json) host.out(render_refusal(s.kind == infer::Suggestion::Kind::Unsure, s.explanation, lang, color));
        return r.finish(kExitOk);
    }
    if (!s.pipeline.bson) {
        if (debug()) host.err("askmdb: parse_error: " + exec::escape_control(s.pipeline.parse_error) + "\n");
        return r.fail(kExitError, tr(lang, Msg::InvalidResponse), "invalid_pipeline", s.pipeline.parse_error);
    }
    const std::string mongosh = mongo::to_mongosh(s.collection, *s.pipeline.bson);
    r.rep.mongosh = mongosh;

    // 9-11. Verify (against the full schema, never the trimmed block), then the mode. A run needs
    // the client, which exists whenever may_run (step 4).
    const auto assess = [&](const infer::Suggestion& sug) {
        return verify::verify(sug.pipeline, sug.collection, db, full);
    };
    const mongo::Limits limits{cfg.max_time_ms, cfg.max_rows};

    if (args.json) {
        const verify::Assessment a = assess(s);
        r.rep.assessment = a;
        if (a.level == verify::Level::Blocked) return r.finish(kExitBlocked);
        if (mode == config::Mode::Auto && a.level == verify::Level::Read && client) {
            auto rows = client->aggregate(s.collection, *s.pipeline.bson, limits);
            if (!rows.value) return r.mongo_fail(rows.error);
            r.rep.executed = true;
            r.rep.rows_json = mongo::rows_json(rows.value->docs);
            r.rep.truncated = rows.value->truncated;
        }
        return r.finish(kExitOk);
    }

    FlowOptions opt;
    opt.mode = mode;
    opt.lang = lang;
    opt.color = color;
    opt.interactive = host.stdin_tty && host.stdout_tty;

    Io io;
    io.write = host.out;
    io.read_line = host.read_line;
    io.verify = assess;
    io.copy = host.copy;
    io.execute = [&](const infer::Suggestion& sug) {
        if (!client) return kExitError;
        auto rows = client->aggregate(sug.collection, *sug.pipeline.bson, limits);
        if (!rows.value) return r.mongo_fail(rows.error);
        const mongo::NumberStyle style = lang == UiLang::Es ? mongo::NumberStyle::Es : mongo::NumberStyle::En;
        host.out(render_results(mongo::render_table(rows.value->docs, style), rows.value->truncated, cfg.max_rows,
                                lang, color));
        return kExitOk;
    };
    return run_flow(s, mongosh, opt, io);
}

int run(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    std::signal(SIGINT, on_sigint);

    Host host;
    host.out = [](std::string_view t) { write_fd(1, t); };
    host.err = [](std::string_view t) { write_fd(2, t); };
    host.read_line = read_stdin_line;
    host.copy = exec::copy_to_clipboard;
    host.stdin_tty = ::isatty(0) == 1;
    host.stdout_tty = ::isatty(1) == 1;
    host.stderr_tty = ::isatty(2) == 1;
    return run(args, host);
}

}  // namespace askmdb::cli
