#include "doctest.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "infer/hw.hpp"

using namespace askmdb::infer;

namespace {

constexpr std::uint64_t kMiB = 1ULL << 20;

GpuInfo gpu(GpuKind kind, std::uint64_t free_bytes, std::uint64_t total_bytes = 24 * kGiB) {
    GpuInfo g;
    g.kind = kind;
    g.free_bytes = free_bytes;
    g.total_bytes = total_bytes;
    return g;
}

HwInfo with(std::vector<GpuInfo> gpus) {
    HwInfo hw;
    for (std::size_t i = 0; i < gpus.size(); ++i) gpus[i].id = "Vulkan" + std::to_string(i);
    hw.gpus = std::move(gpus);
    return hw;
}

// Sets (or, with nullopt, unsets) an environment variable and restores it at the end.
struct EnvVar {
    std::string name;
    std::optional<std::string> old;
    EnvVar(const char* n, const std::optional<std::string>& value) : name(n) {
        if (const char* v = std::getenv(n)) old = v;
        if (value)
            ::setenv(n, value->c_str(), 1);
        else
            ::unsetenv(n);
    }
    EnvVar(const EnvVar&) = delete;
    EnvVar& operator=(const EnvVar&) = delete;
    ~EnvVar() {
        if (old)
            ::setenv(name.c_str(), old->c_str(), 1);
        else
            ::unsetenv(name.c_str());
    }
};

}  // namespace

TEST_SUITE("infer") {

TEST_CASE("hw: choose follows the table") {
    struct Row {
        const char* label;
        std::vector<GpuInfo> gpus;
        Device device;
        int gpu;
        ModelTier model;
    };
    const std::vector<Row> rows = {
        {"no GPU", {}, Device::Cpu, -1, ModelTier::M08B},
        {"only integrated, lots of memory", {gpu(GpuKind::Integrated, 32 * kGiB)}, Device::Cpu, -1, ModelTier::M08B},
        {"only software", {gpu(GpuKind::Software, 32 * kGiB)}, Device::Cpu, -1, ModelTier::M08B},
        {"discrete 3 GiB free", {gpu(GpuKind::Discrete, 3 * kGiB)}, Device::Cpu, -1, ModelTier::M08B},
        {"discrete just under 4 GiB", {gpu(GpuKind::Discrete, kMin4bFreeBytes - 1)}, Device::Cpu, -1, ModelTier::M08B},
        {"discrete 4 GiB", {gpu(GpuKind::Discrete, 4 * kGiB)}, Device::Gpu, 0, ModelTier::M4B},
        {"discrete 7.9 GiB", {gpu(GpuKind::Discrete, 7 * kGiB + 900 * kMiB)}, Device::Gpu, 0, ModelTier::M4B},
        {"discrete exactly 8 GiB", {gpu(GpuKind::Discrete, 8 * kGiB)}, Device::Gpu, 0, ModelTier::M9B},
        {"discrete 16 GiB", {gpu(GpuKind::Discrete, 16 * kGiB)}, Device::Gpu, 0, ModelTier::M9B},
        {"two discrete: the one with most free",
         {gpu(GpuKind::Discrete, 5 * kGiB), gpu(GpuKind::Discrete, 10 * kGiB)}, Device::Gpu, 1, ModelTier::M9B},
        {"two discrete, the larger first", {gpu(GpuKind::Discrete, 6 * kGiB), gpu(GpuKind::Discrete, 2 * kGiB)},
         Device::Gpu, 0, ModelTier::M4B},
        {"two discrete, tie: the lowest index",
         {gpu(GpuKind::Discrete, 6 * kGiB), gpu(GpuKind::Discrete, 6 * kGiB)}, Device::Gpu, 0, ModelTier::M4B},
        {"two discrete, both too small", {gpu(GpuKind::Discrete, 2 * kGiB), gpu(GpuKind::Discrete, 3 * kGiB)},
         Device::Cpu, -1, ModelTier::M08B},
        {"discrete + bigger integrated: the discrete",
         {gpu(GpuKind::Integrated, 30 * kGiB), gpu(GpuKind::Discrete, 5 * kGiB)}, Device::Gpu, 1, ModelTier::M4B},
        {"software + discrete small", {gpu(GpuKind::Software, 30 * kGiB), gpu(GpuKind::Discrete, 1 * kGiB)},
         Device::Cpu, -1, ModelTier::M08B},
    };
    for (const Row& row : rows) {
        CAPTURE(row.label);
        const Choice c = choose(with(row.gpus));
        CHECK(c.device == row.device);
        CHECK(c.gpu == row.gpu);
        CHECK(c.model == row.model);
    }
}

TEST_CASE("hw: the test hook lists the GPUs in order, with ids Vulkan0, Vulkan1...") {
    EnvVar hw("ASKMDB_TEST_HW", "discrete:RTX 4080 Super:15000:16303;integrated:Radeon 780M:8000:16000;software:llvmpipe:0:0");
    const HwInfo info = detect_hardware();
    REQUIRE(info.gpus.size() == 3);
    CHECK(info.vulkan);
    CHECK(info.gpus[0].id == "Vulkan0");
    CHECK(info.gpus[0].name == "RTX 4080 Super");
    CHECK(info.gpus[0].kind == GpuKind::Discrete);
    CHECK(info.gpus[0].free_bytes == 15000 * kMiB);
    CHECK(info.gpus[0].total_bytes == 16303 * kMiB);
    CHECK(info.gpus[1].id == "Vulkan1");
    CHECK(info.gpus[1].kind == GpuKind::Integrated);
    CHECK(info.gpus[2].id == "Vulkan2");
    CHECK(info.gpus[2].kind == GpuKind::Software);
    CHECK(info.gpus[2].free_bytes == 0);
    CHECK(info.ram_total_bytes > 0);
    CHECK(info.cores >= 1);

    const Choice c = choose(info);
    CHECK(c.device == Device::Gpu);
    CHECK(c.gpu == 0);
    CHECK(c.model == ModelTier::M9B);
}

TEST_CASE("hw: the test hook with none or a malformed value gives no GPU") {
    for (const char* spec : {"none", "", "discrete:A:100", "discrete:A:100:200:300", "dedicated:A:100:200",
                             "discrete::100:200", "discrete:A:1e3:2000", "discrete:A: 100:200", "discrete:A:-1:200",
                             "discrete:A:100:", "discrete:A:100:200;;integrated:B:1:2", "discrete:A:100:200;bad",
                             "Discrete:A:100:200"}) {
        CAPTURE(spec);
        EnvVar hw("ASKMDB_TEST_HW", spec);
        const HwInfo info = detect_hardware();
        CHECK(info.gpus.empty());
        CHECK_FALSE(info.vulkan);
        CHECK(info.cores >= 1);
        CHECK(choose(info).device == Device::Cpu);
    }
}

TEST_CASE("hw: real detection lists only GPU devices and reads RAM and cores") {
    EnvVar hw("ASKMDB_TEST_HW", std::nullopt);
    const HwInfo info = detect_hardware();
    CHECK(info.ram_total_bytes > 0);
    CHECK(info.ram_available_bytes > 0);
    CHECK(info.ram_available_bytes <= info.ram_total_bytes);
    CHECK(info.cores >= 1);
    for (const GpuInfo& g : info.gpus) {
        CAPTURE(g.id);
        CHECK_FALSE(g.id.empty());
        CHECK(g.id != "CPU");
        CHECK(g.free_bytes <= g.total_bytes);
    }
    MESSAGE("GPUs: " << info.gpus.size() << ", vulkan " << info.vulkan << ", cores " << info.cores << ", RAM "
                     << (info.ram_total_bytes >> 20) << " MiB");
}

TEST_CASE("hw: tier and device names round trip") {
    for (ModelTier t : {ModelTier::M08B, ModelTier::M4B, ModelTier::M9B}) {
        const auto name = tier_name(t);
        REQUIRE(parse_tier(name).has_value());
        CHECK(*parse_tier(name) == t);
    }
    CHECK(tier_name(ModelTier::M08B) == "askmdb-0.8b");
    CHECK(tier_name(ModelTier::M4B) == "askmdb-4b");
    CHECK(tier_name(ModelTier::M9B) == "askmdb-9b");
    CHECK_FALSE(parse_tier("askmdb-2b").has_value());
    CHECK_FALSE(parse_tier("").has_value());
    CHECK_FALSE(parse_tier("ASKMDB-4B").has_value());
    CHECK(device_name(Device::Cpu) == "cpu");
    CHECK(device_name(Device::Gpu) == "gpu");
}

}  // TEST_SUITE
