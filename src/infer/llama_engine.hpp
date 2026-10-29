#pragma once
// Engine backed by llama.cpp, on the CPU or entirely on one GPU (Vulkan), with the model loaded
// lazily on the first suggest() and the state of the prompt prefix (instructions + schema block)
// saved in the cache directory.
//
// Contract of the engine:
//   n_ctx 4096, n_batch 2048, n_ubatch 512, greedy, <= 384 generated tokens, stop when the
//   top-level JSON object closes, timeout counted from the end of the prefill;
//   the prompt is always decoded in two parts (prefix, rest), each in chunks of <= n_batch, so the
//   output is the same with and without a saved state;
//   states: <cache_dir>/prefix-<sha256(key)>.state, directory 0700 and files 0600, LRU; the key
//   includes the device, so CPU and GPU states (or two GPUs') never mix.
// Load errors: gpu_unavailable (Device::Gpu and no GPU device named gpu_id; the caller may retry
// on the CPU) and load_failed (anything else, e.g. an unreadable model or not enough memory).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>

#include "infer/engine.hpp"
#include "infer/hw.hpp"

namespace askmdb::infer {

inline constexpr int kCtx = 4096;
inline constexpr int kBatch = 2048;
inline constexpr int kUbatch = 512;
inline constexpr int kMaxGenTokens = 384;
inline constexpr std::size_t kMaxPrefixStates = 8;
inline constexpr std::uint64_t kMaxPrefixCacheBytes = 1ULL << 30;  // 1 GiB

struct LlamaOptions {
    std::string model_path;
    Device device = Device::Cpu;  // Cpu: no GPU touches the model (no layers, no op offload)
    std::string gpu_id;           // Gpu: ggml device name (GpuInfo::id); every layer goes there
    int threads = 4;
    std::chrono::milliseconds timeout{15000};  // from the end of the prefill
    bool debug = false;                        // llama.cpp/ggml logs to stderr (ASKMDB_DEBUG=1)
};

std::unique_ptr<Engine> make_llama_engine(const LlamaOptions& options);

// File name of a prefix state: "prefix-<sha256>.state" over the contract, the model identity
// (model_id), n_ctx, the askmdb and ggml versions, the device ("cpu" or "gpu:<description>") and
// the prefix text.
std::string prefix_state_name(const std::string& model_id, const std::string& device, const std::string& prefix);

// LRU pruning of dir: keeps the most recently used (by mtime) regular files "prefix-*.state",
// at most max_files and at most max_bytes in total, but never removes keep_name (the state just
// saved, a bare file name; empty = none). Also removes temporaries ".prefix.state-*" older than
// one hour. Errors are ignored.
void prune_prefix_cache(const std::string& dir, const std::string& keep_name, std::time_t now,
                        std::size_t max_files = kMaxPrefixStates, std::uint64_t max_bytes = kMaxPrefixCacheBytes);

}  // namespace askmdb::infer
