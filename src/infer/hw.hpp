#pragma once
// Hardware detection and the automatic choice of device and model (first start).
//
//   no discrete GPU, or free VRAM < kMin4bFreeBytes   -> CPU, askmdb-0.8b
//   discrete GPU with kMin4bFreeBytes <= free < 8 GiB  -> GPU, askmdb-4b
//   discrete GPU with free >= 8 GiB                    -> GPU, askmdb-9b
//
// Only discrete GPUs count. Integrated GPUs and software devices (llvmpipe) are listed by
// detect_hardware() but never chosen. With several discrete GPUs, the one with most free VRAM.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace askmdb::infer {

enum class Device { Cpu, Gpu };

enum class GpuKind { Discrete, Integrated, Software };

struct GpuInfo {
    std::string id;           // ggml device name, e.g. "Vulkan0" (what LlamaOptions::gpu_id takes)
    std::string name;         // ggml device description, e.g. "NVIDIA GeForce RTX 4080 Super"
    GpuKind kind = GpuKind::Discrete;
    std::uint64_t free_bytes = 0;
    std::uint64_t total_bytes = 0;
};

struct HwInfo {
    std::vector<GpuInfo> gpus;            // every non-CPU device ggml sees, in ggml order
    std::uint64_t ram_total_bytes = 0;
    std::uint64_t ram_available_bytes = 0;
    int cores = 0;                        // physical cores available to the process
    bool vulkan = false;                  // the Vulkan loader (libvulkan) was opened
};

// Model tiers of the family. The files behind them are in fetch/model_info.hpp.
enum class ModelTier { M08B, M4B, M9B };

inline constexpr std::uint64_t kGiB = 1ULL << 30;
inline constexpr std::uint64_t kMin9bFreeBytes = 8 * kGiB;
// Measured peak of askmdb-4b with n_ctx 4096 on Vulkan (3,369 MiB, driver context included;
// 16 GB NVIDIA GPU) + 0.5 GiB, rounded up to 0.5 GiB. askmdb-9b peaks at 5,637 MiB.
inline constexpr std::uint64_t kMin4bFreeBytes = 4 * kGiB;

struct Choice {
    Device device = Device::Cpu;
    int gpu = -1;                         // index into HwInfo::gpus; -1 on CPU
    ModelTier model = ModelTier::M08B;
};

// Pure: the rule above.
Choice choose(const HwInfo& hw);

// Real hardware through ggml's device registry (initialises Vulkan if it is available). In test
// builds (ASKMDB_TEST_HOOKS) the variable ASKMDB_TEST_HW replaces it:
//   ASKMDB_TEST_HW=none                                  no GPU
//   ASKMDB_TEST_HW=discrete:NAME:FREE_MIB:TOTAL_MIB[;integrated:...;software:...]
// ids are "Vulkan0", "Vulkan1"... in that order; RAM and cores stay the real ones.
HwInfo detect_hardware();

std::string_view tier_name(ModelTier tier);                      // "askmdb-0.8b", "askmdb-4b", "askmdb-9b"
std::optional<ModelTier> parse_tier(std::string_view name);
std::string_view device_name(Device device);                     // "cpu", "gpu"

}  // namespace askmdb::infer
