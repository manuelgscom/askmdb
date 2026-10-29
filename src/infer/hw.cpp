#include "infer/hw.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include <sys/sysinfo.h>

#include "ggml-backend.h"
#include "infer/cpu.hpp"
#include "infer/vk_loader.hpp"
#include "llama.h"

namespace askmdb::infer {

namespace {

// llama_log_set also installs the callback in ggml; Vulkan reports what it finds through it.
void log_callback(ggml_log_level, const char* text, void* debug) {
    if (debug != nullptr) std::fputs(text, stderr);
}

bool debug_logs() {
    const char* v = std::getenv("ASKMDB_DEBUG");
    return v != nullptr && std::string_view(v) == "1";
}

bool contains_nocase(std::string_view haystack, std::string_view needle) {
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != haystack.end();
}

// A Vulkan driver that runs on the CPU: listed, never chosen.
bool software_renderer(std::string_view description) {
    for (std::string_view s : {"llvmpipe", "lavapipe", "SwiftShader"})
        if (contains_nocase(description, s)) return true;
    return false;
}

void detect_ram(HwInfo& hw) {
    std::ifstream in("/proc/meminfo");
    std::string key;
    std::uint64_t kib = 0;
    std::string unit;
    while (in >> key >> kib) {
        std::getline(in, unit);
        if (key == "MemTotal:") hw.ram_total_bytes = kib * 1024;
        if (key == "MemAvailable:") hw.ram_available_bytes = kib * 1024;
    }
    if (hw.ram_total_bytes == 0) {
        struct sysinfo si {};
        if (::sysinfo(&si) == 0) {
            hw.ram_total_bytes = std::uint64_t{si.totalram} * si.mem_unit;
            hw.ram_available_bytes = std::uint64_t{si.freeram} * si.mem_unit;
        }
    }
}

void detect_gpus(HwInfo& hw) {
    // The registry is created on the first ggml_backend_* call, here or in the engine, and is
    // registered statically (no llama_backend_init needed). Its logs only with ASKMDB_DEBUG=1.
    static bool debug = debug_logs();
    llama_log_set(log_callback, debug ? &debug : nullptr);
    vulkan_set_enabled(true);
    for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        const enum ggml_backend_dev_type type = ggml_backend_dev_type(dev);
        if (type != GGML_BACKEND_DEVICE_TYPE_GPU && type != GGML_BACKEND_DEVICE_TYPE_IGPU) continue;
        GpuInfo g;
        g.id = ggml_backend_dev_name(dev);
        g.name = ggml_backend_dev_description(dev);
        g.kind = software_renderer(g.name)                 ? GpuKind::Software
                 : type == GGML_BACKEND_DEVICE_TYPE_IGPU ? GpuKind::Integrated
                                                         : GpuKind::Discrete;
        std::size_t free = 0, total = 0;
        ggml_backend_dev_memory(dev, &free, &total);
        g.free_bytes = free;
        g.total_bytes = total;
        hw.gpus.push_back(std::move(g));
    }
    hw.vulkan = vulkan_loaded();
}

#if ASKMDB_TEST_HOOKS
bool parse_mib(std::string_view s, std::uint64_t& bytes) {
    std::uint64_t mib = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), mib);
    if (s.empty() || ec != std::errc() || end != s.data() + s.size() || mib > (1ULL << 40)) return false;
    bytes = mib << 20;
    return true;
}

// ASKMDB_TEST_HW (see hw.hpp). Anything malformed (unknown kind, empty or ':'-containing name,
// a field missing or extra, a non-decimal size) gives the same as "none": no GPU. With GPUs,
// vulkan is true.
std::vector<GpuInfo> parse_test_hw(std::string_view spec) {
    std::vector<GpuInfo> gpus;
    if (spec == "none") return gpus;
    std::stringstream entries{std::string(spec)};
    std::string entry;
    while (std::getline(entries, entry, ';')) {
        std::vector<std::string> f;
        std::stringstream fields(entry);
        for (std::string x; std::getline(fields, x, ':');) f.push_back(x);
        if (!entry.empty() && entry.back() == ':') f.emplace_back();
        GpuInfo g;
        if (f.size() != 4 || f[1].empty() || !parse_mib(f[2], g.free_bytes) || !parse_mib(f[3], g.total_bytes))
            return {};
        if (f[0] == "discrete")
            g.kind = GpuKind::Discrete;
        else if (f[0] == "integrated")
            g.kind = GpuKind::Integrated;
        else if (f[0] == "software")
            g.kind = GpuKind::Software;
        else
            return {};
        g.id = "Vulkan" + std::to_string(gpus.size());
        g.name = f[1];
        gpus.push_back(std::move(g));
    }
    return gpus;
}
#endif

}  // namespace

Choice choose(const HwInfo& hw) {
    int best = -1;
    for (std::size_t i = 0; i < hw.gpus.size(); ++i) {
        const GpuInfo& g = hw.gpus[i];
        if (g.kind != GpuKind::Discrete) continue;
        // Strictly more: ties keep the lowest index.
        if (best < 0 || g.free_bytes > hw.gpus[static_cast<std::size_t>(best)].free_bytes) best = static_cast<int>(i);
    }
    if (best < 0) return {};
    const std::uint64_t free = hw.gpus[static_cast<std::size_t>(best)].free_bytes;
    if (free < kMin4bFreeBytes) return {};
    return {Device::Gpu, best, free >= kMin9bFreeBytes ? ModelTier::M9B : ModelTier::M4B};
}

HwInfo detect_hardware() {
    HwInfo hw;
    detect_ram(hw);
    hw.cores = physical_cores();
#if ASKMDB_TEST_HOOKS
    if (const char* spec = std::getenv("ASKMDB_TEST_HW")) {
        hw.gpus = parse_test_hw(spec);
        hw.vulkan = !hw.gpus.empty();
        return hw;
    }
#endif
    detect_gpus(hw);
    return hw;
}

std::string_view tier_name(ModelTier tier) {
    switch (tier) {
        case ModelTier::M08B: return "askmdb-0.8b";
        case ModelTier::M4B: return "askmdb-4b";
        case ModelTier::M9B: return "askmdb-9b";
    }
    return "askmdb-0.8b";
}

std::optional<ModelTier> parse_tier(std::string_view name) {
    for (ModelTier t : {ModelTier::M08B, ModelTier::M4B, ModelTier::M9B})
        if (tier_name(t) == name) return t;
    return std::nullopt;
}

std::string_view device_name(Device device) { return device == Device::Gpu ? "gpu" : "cpu"; }

}  // namespace askmdb::infer
