#include "infer/llama_engine.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fetch/sha256.hpp"
#include "infer/json_tracker.hpp"
#include "infer/parser.hpp"
#include "infer/prompt.hpp"
#include "infer/vk_loader.hpp"
#include "llama.h"
#include "schema/schema.hpp"

namespace askmdb::infer {

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// llama_log_set also installs the callback in ggml (llama-impl.cpp), so it covers both.
// Non-null user_data => LlamaOptions::debug: logs go to stderr.
void log_callback(ggml_log_level, const char* text, void* debug) {
    if (debug != nullptr) std::fputs(text, stderr);
}

// Fast checksum (FNV-1a over 64-bit words) to detect a corrupt state file.
std::uint64_t checksum(const std::vector<unsigned char>& data) {
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t h = 14695981039346656037ull;
    std::size_t i = 0;
    for (; i + 8 <= data.size(); i += 8) {
        std::uint64_t w = 0;
        std::memcpy(&w, data.data() + i, 8);
        h = (h ^ w) * kPrime;
    }
    for (; i < data.size(); ++i) h = (h ^ data[i]) * kPrime;
    return h;
}

// Reads exactly size bytes. false on error or end of file.
bool read_all(int fd, void* buf, std::size_t size) {
    auto* p = static_cast<unsigned char*>(buf);
    while (size > 0) {
        const ssize_t n = ::read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

bool write_all(int fd, const void* buf, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(buf);
    while (size > 0) {
        const ssize_t n = ::write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

// Own file format: header, prefix tokens and the llama_state_seq_get_data blob.
// llama_state_seq_load_file is not used because it aborts the process (GGML_ASSERT) on a
// truncated file; here size and checksum are verified before handing anything to llama.cpp.
struct StateHeader {
    char magic[8];
    std::uint64_t n_tokens;
    std::uint64_t n_state;
    std::uint64_t checksum;  // of the tokens and the state
};
constexpr char kStateMagic[8] = {'A', 'S', 'K', 'M', 'P', 'F', 'X', '1'};
constexpr std::uint64_t kMaxStateBytes = std::uint64_t{1} << 32;

class LlamaEngine : public Engine {
public:
    explicit LlamaEngine(LlamaOptions options) : options_(std::move(options)) {}

    ~LlamaEngine() override {
        if (sampler_ != nullptr) llama_sampler_free(sampler_);
        if (ctx_ != nullptr) llama_free(ctx_);
        if (model_ != nullptr) llama_model_free(model_);
        if (backend_) llama_backend_free();
    }

    LlamaEngine(const LlamaEngine&) = delete;
    LlamaEngine& operator=(const LlamaEngine&) = delete;

    EngineResult suggest(const Request& request) override {
        const auto t_start = Clock::now();
        const std::string prompt = render_prompt(request);
        const std::string prefix = prompt.substr(0, std::min(prefix_bytes(request), prompt.size()));

        EngineResult r;
        if (load(r)) {
            generate(prompt, prefix, r);
            if (r.error.empty()) {
                ParseResult p = parse_response(r.raw);
                r.suggestion = std::move(p.suggestion);
                r.error = std::move(p.error);
                r.error_detail = std::move(p.detail);
            }
        }
        r.timings.total_ms = ms_since(t_start);
        return r;
    }

private:
    // Lazy load: backend, model, context and sampler. Attempted only once; a failure is kept and
    // returned by every later call.
    bool load(EngineResult& r) {
        if (ctx_ != nullptr) return true;
        if (!load_error_.empty()) {
            r.error = load_code_;
            r.error_detail = load_error_;
            return false;
        }
        const auto t0 = Clock::now();
        llama_log_set(log_callback, options_.debug ? &options_ : nullptr);
        vulkan_set_enabled(options_.device == Device::Gpu);
        llama_backend_init();
        backend_ = true;

        // An explicit device list, never llama.cpp's default choice: on the CPU an empty list (no
        // layers anywhere else) and no op offload, so no GPU, not even an integrated one, touches
        // the model; on a GPU, that device alone with every layer.
        static ggml_backend_dev_t no_devices[] = {nullptr};
        ggml_backend_dev_t gpu_devices[] = {nullptr, nullptr};
        llama_model_params mp = llama_model_default_params();
        mp.load_mode = LLAMA_LOAD_MODE_MMAP;
        if (options_.device == Device::Gpu) {
            ggml_backend_dev_t dev = ggml_backend_dev_by_name(options_.gpu_id.c_str());
            if (dev == nullptr || ggml_backend_dev_type(dev) == GGML_BACKEND_DEVICE_TYPE_CPU) {
                load_code_ = "gpu_unavailable";
                load_error_ = "no GPU device named '" + options_.gpu_id + "'";
            } else {
                gpu_devices[0] = dev;
                mp.devices = gpu_devices;
                mp.n_gpu_layers = 999;
                mp.split_mode = LLAMA_SPLIT_MODE_NONE;
                mp.main_gpu = 0;
                device_id_ = std::string("gpu:") + ggml_backend_dev_description(dev);
            }
        } else {
            mp.devices = no_devices;
            mp.n_gpu_layers = 0;
            device_id_ = "cpu";
        }
        if (!load_error_.empty()) {
            // Nothing to load.
        } else if ((model_ = llama_model_load_from_file(options_.model_path.c_str(), mp)) == nullptr) {
            load_error_ = "cannot load the model: " + options_.model_path;
        } else {
            llama_context_params cp = llama_context_default_params();
            cp.n_ctx = kCtx;
            cp.n_batch = kBatch;
            // A larger n_ubatch reserves a much bigger compute buffer without speeding up the
            // CPU prefill; longer batches are split into micro-batches inside llama_decode.
            cp.n_ubatch = kUbatch;
            cp.n_threads = options_.threads;
            cp.n_threads_batch = options_.threads;
            if (options_.device == Device::Cpu) cp.op_offload = false;
            ctx_ = llama_init_from_model(model_, cp);
            if (ctx_ == nullptr) load_error_ = "cannot create the context: " + options_.model_path;
        }
        r.timings.load_ms = ms_since(t0);
        if (ctx_ == nullptr) {
            r.error = load_code_;
            r.error_detail = load_error_;
            return false;
        }
        vocab_ = llama_model_get_vocab(model_);
        sampler_ = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(sampler_, llama_sampler_init_greedy());

        // Model identity for the cache key: name, size and mtime.
        struct stat st {};
        if (::stat(options_.model_path.c_str(), &st) == 0) {
            const std::size_t slash = options_.model_path.rfind('/');
            model_id_ = "model=" + options_.model_path.substr(slash == std::string::npos ? 0 : slash + 1) +
                        "\nsize=" + std::to_string(st.st_size) + "\nmtime=" + std::to_string(st.st_mtim.tv_sec) +
                        "." + std::to_string(st.st_mtim.tv_nsec) + "\n";
        }
        return true;
    }

    std::vector<llama_token> tokenize(const std::string& text) const {
        const bool add_bos = llama_vocab_get_add_bos(vocab_);
        const auto len = static_cast<std::int32_t>(text.size());
        std::vector<llama_token> tokens(text.size() + 2);
        std::int32_t n = llama_tokenize(vocab_, text.data(), len, tokens.data(),
                                        static_cast<std::int32_t>(tokens.size()), add_bos, true);
        if (n < 0) {
            tokens.resize(static_cast<std::size_t>(-n));
            n = llama_tokenize(vocab_, text.data(), len, tokens.data(),
                               static_cast<std::int32_t>(tokens.size()), add_bos, true);
        }
        tokens.resize(n < 0 ? 0 : static_cast<std::size_t>(n));
        return tokens;
    }

    std::string piece(llama_token token) const {
        char buf[256];
        const std::int32_t n = llama_token_to_piece(vocab_, token, buf, sizeof(buf), 0, false);
        if (n >= 0) return std::string(buf, static_cast<std::size_t>(n));
        std::string big(static_cast<std::size_t>(-n), '\0');
        const std::int32_t m = llama_token_to_piece(vocab_, token, big.data(), -n, 0, false);
        big.resize(m < 0 ? 0 : static_cast<std::size_t>(m));
        return big;
    }

    // Decodes tokens[from, to) right after what is already in memory, in chunks of <= kBatch at
    // fixed offsets from `from`, so the cuts never depend on whether a state was loaded.
    bool decode(std::vector<llama_token>& tokens, std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to; i += kBatch) {
            const std::size_t n = std::min<std::size_t>(kBatch, to - i);
            if (llama_decode(ctx_, llama_batch_get_one(tokens.data() + i, static_cast<std::int32_t>(n))) != 0) {
                return false;
            }
        }
        return true;
    }

    void clear_memory() { llama_memory_clear(llama_get_memory(ctx_), true); }

    // Directory of the prefix states (0700, ours). Empty => cache off.
    static std::string state_dir() {
        const std::string dir = schema::cache_dir();
        std::string error;
        if (dir.empty() || !schema::ensure_cache_dir(dir, error)) return {};
        return dir;
    }


    // Loads the saved state and checks it holds exactly the prefix tokens. If the file exists but
    // is invalid, deletes it. On failure, leaves the memory cleared.
    bool load_prefix(const std::string& path, const std::vector<llama_token>& tokens, std::size_t n_prefix) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return false;
        const std::size_t token_bytes = n_prefix * sizeof(llama_token);
        StateHeader h{};
        std::vector<unsigned char> data;
        struct stat st {};
        bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && read_all(fd, &h, sizeof(h)) &&
                  std::memcmp(h.magic, kStateMagic, sizeof(kStateMagic)) == 0 && h.n_tokens == n_prefix &&
                  h.n_state > 0 && h.n_state < kMaxStateBytes &&
                  static_cast<std::uint64_t>(st.st_size) == sizeof(h) + token_bytes + h.n_state;
        if (ok) {
            data.resize(token_bytes + h.n_state);
            ok = read_all(fd, data.data(), data.size()) && checksum(data) == h.checksum &&
                 std::memcmp(data.data(), tokens.data(), token_bytes) == 0;
        }
        ::close(fd);
        ok = ok && llama_state_seq_set_data(ctx_, data.data() + token_bytes, h.n_state, 0) > 0 &&
             llama_memory_seq_pos_max(llama_get_memory(ctx_), 0) == static_cast<llama_pos>(n_prefix) - 1;
        if (!ok) {
            ::unlink(path.c_str());
            clear_memory();
        }
        return ok;
    }

    // Saves the prefix state to a temporary file in the same directory (0600, fsync) and renames
    // it (atomic), then prunes the directory. A failure only means there is no cache.
    void save_prefix(const std::string& dir, const std::string& name, const std::vector<llama_token>& tokens,
                     std::size_t n_prefix) {
        const std::size_t n_state = llama_state_seq_get_size(ctx_, 0);
        if (n_state == 0 || n_state >= kMaxStateBytes) return;
        const std::size_t token_bytes = n_prefix * sizeof(llama_token);
        std::vector<unsigned char> data(token_bytes + n_state);
        std::memcpy(data.data(), tokens.data(), token_bytes);
        if (llama_state_seq_get_data(ctx_, data.data() + token_bytes, n_state, 0) != n_state) return;
        StateHeader h{};
        std::memcpy(h.magic, kStateMagic, sizeof(kStateMagic));
        h.n_tokens = n_prefix;
        h.n_state = n_state;
        h.checksum = checksum(data);

        std::string tmp = dir + "/.prefix.state-XXXXXX";
        const int fd = ::mkstemp(tmp.data());
        if (fd < 0) return;
        const bool written = ::fchmod(fd, 0600) == 0 && write_all(fd, &h, sizeof(h)) &&
                             write_all(fd, data.data(), data.size()) && ::fsync(fd) == 0;
        const bool closed = ::close(fd) == 0;
        if (!written || !closed || std::rename(tmp.c_str(), (dir + "/" + name).c_str()) != 0) {
            ::unlink(tmp.c_str());
            return;
        }
        prune_prefix_cache(dir, name, std::time(nullptr));
    }

    // Prefill, always in two parts (prefix and rest) when the prefix tokens are a prefix of the
    // prompt tokens, with or without a saved state, so the output is the same either way.
    bool prefill(std::vector<llama_token>& tokens, const std::string& prefix, EngineResult& r) {
        clear_memory();
        std::size_t n_prefix = 0;
        if (!prefix.empty()) {
            const std::vector<llama_token> p = tokenize(prefix);
            if (!p.empty() && p.size() < tokens.size() && std::equal(p.begin(), p.end(), tokens.begin())) {
                n_prefix = p.size();
            }
        }
        r.timings.prefix_tokens = static_cast<int>(n_prefix);
        r.prefix_cache = PrefixCache::Off;
        if (n_prefix == 0) return decode(tokens, 0, tokens.size());

        const std::string dir = model_id_.empty() ? std::string() : state_dir();
        const std::string name = dir.empty() ? std::string() : prefix_state_name(model_id_, device_id_, prefix);
        const std::string path = dir.empty() ? std::string() : dir + "/" + name;
        if (!path.empty()) {
            r.prefix_cache = PrefixCache::Miss;
            if (load_prefix(path, tokens, n_prefix)) {
                if (decode(tokens, n_prefix, tokens.size())) {
                    r.prefix_cache = PrefixCache::Hit;
                    ::utimensat(AT_FDCWD, path.c_str(), nullptr, 0);  // most recently used
                    return true;
                }
                ::unlink(path.c_str());  // loaded state unusable: decode everything
                clear_memory();
            }
        }
        if (!decode(tokens, 0, n_prefix)) return false;
        if (!path.empty()) save_prefix(dir, name, tokens, n_prefix);
        return decode(tokens, n_prefix, tokens.size());
    }

    // Prefill and greedy decoding. Stores in r the raw text, the timings and, if it stops before
    // the parser, the error.
    void generate(const std::string& prompt, const std::string& prefix, EngineResult& r) {
        llama_sampler_reset(sampler_);

        const auto t_prefill = Clock::now();
        std::vector<llama_token> tokens = tokenize(prompt);
        r.timings.prompt_tokens = static_cast<int>(tokens.size());
        if (tokens.empty()) {
            r.error = "decode_failed";
            r.error_detail = "the prompt could not be tokenized";
            return;
        }
        if (tokens.size() + kMaxGenTokens > static_cast<std::size_t>(kCtx)) {
            r.error = "too_long";
            r.error_detail = "the prompt has " + std::to_string(tokens.size()) + " tokens; with " +
                             std::to_string(kMaxGenTokens) + " for the answer it exceeds " + std::to_string(kCtx);
            return;
        }
        if (!prefill(tokens, prefix, r)) {
            r.error = "decode_failed";
            r.error_detail = "llama_decode failed in the prefill";
            return;
        }
        r.timings.prefill_ms = ms_since(t_prefill);

        const auto t_gen = Clock::now();
        JsonTracker tracker;
        for (;;) {
            llama_token token = llama_sampler_sample(sampler_, ctx_, -1);
            if (llama_vocab_is_eog(vocab_, token)) break;
            ++r.timings.gen_tokens;
            const std::string text = piece(token);
            r.raw.append(text, 0, tracker.feed(text));
            if (tracker.state() == JsonTracker::State::Done) break;
            if (tracker.state() == JsonTracker::State::Invalid) {
                r.error = "invalid_json";
                r.error_detail = "the answer does not start with '{'";
                break;
            }
            if (r.timings.gen_tokens >= kMaxGenTokens) {
                r.error = "truncated";
                r.error_detail = "more than " + std::to_string(kMaxGenTokens) + " generated tokens";
                break;
            }
            if (Clock::now() - t_gen > options_.timeout) {
                r.error = "timeout";
                r.error_detail = "more than " + std::to_string(options_.timeout.count()) + " ms generating";
                break;
            }
            if (llama_decode(ctx_, llama_batch_get_one(&token, 1)) != 0) {
                r.error = "decode_failed";
                r.error_detail = "llama_decode failed in the generation";
                break;
            }
        }
        r.timings.generate_ms = ms_since(t_gen);
    }

    LlamaOptions options_;
    bool backend_ = false;
    std::string load_code_ = "load_failed";  // with load_error_
    std::string load_error_;
    std::string model_id_;   // empty => no prefix cache
    std::string device_id_;  // "cpu" or "gpu:<ggml device description>", for the cache key
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    const llama_vocab* vocab_ = nullptr;
    llama_sampler* sampler_ = nullptr;
};

}  // namespace

void prune_prefix_cache(const std::string& dir, const std::string& keep_name, std::time_t now,
                        std::size_t max_files, std::uint64_t max_bytes) {
    constexpr std::time_t kStaleTmp = 60 * 60;
    struct Entry {
        std::string path;
        struct timespec mtime;
        std::uint64_t size;
    };
    std::vector<Entry> states;
    std::size_t kept_files = 0;
    std::uint64_t kept_bytes = 0;

    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return;
    while (const dirent* e = ::readdir(d)) {
        const std::string_view name = e->d_name;
        const bool state = name.starts_with("prefix-") && name.ends_with(".state");
        const bool tmp = name.starts_with(".prefix.state-");
        if (!state && !tmp) continue;
        const std::string path = dir + "/" + std::string(name);
        struct stat st {};
        if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (tmp) {
            if (now - st.st_mtime > kStaleTmp) ::unlink(path.c_str());
        } else if (!keep_name.empty() && name == keep_name) {
            ++kept_files;
            kept_bytes += static_cast<std::uint64_t>(st.st_size);
        } else {
            states.push_back({path, st.st_mtim, static_cast<std::uint64_t>(st.st_size)});
        }
    }
    ::closedir(d);

    // Newest first (ties by name, so the order is deterministic); once one state does not fit,
    // it and every older one are removed.
    std::sort(states.begin(), states.end(), [](const Entry& a, const Entry& b) {
        if (a.mtime.tv_sec != b.mtime.tv_sec) return a.mtime.tv_sec > b.mtime.tv_sec;
        if (a.mtime.tv_nsec != b.mtime.tv_nsec) return a.mtime.tv_nsec > b.mtime.tv_nsec;
        return a.path < b.path;
    });
    bool full = false;
    for (const Entry& e : states) {
        full = full || kept_files + 1 > max_files || kept_bytes + e.size > max_bytes;
        if (full) {
            ::unlink(e.path.c_str());
        } else {
            ++kept_files;
            kept_bytes += e.size;
        }
    }
}

// The key covers everything that changes the state, including the prefix text (instructions and
// schema block) and the device (a state computed on one backend is not reused on another).
std::string prefix_state_name(const std::string& model_id, const std::string& device, const std::string& prefix) {
    const std::string key = "contract=" + std::to_string(kContractVersion) + "\n" + model_id + "n_ctx=" +
                            std::to_string(kCtx) + "\nversion=" ASKMDB_VERSION "\nggml=" + ggml_version() + "-" +
                            ggml_commit() + "\ndevice=" + device + "\n" + prefix;
    return "prefix-" + fetch::sha256_hex(key) + ".state";
}

std::unique_ptr<Engine> make_llama_engine(const LlamaOptions& options) {
    return std::make_unique<LlamaEngine>(options);
}

}  // namespace askmdb::infer
