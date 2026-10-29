#include "doctest.h"

#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "infer/cpu.hpp"
#include "infer/hw.hpp"
#include "infer/llama_engine.hpp"
#include "infer/prompt.hpp"
#include "schema/schema.hpp"

using namespace askmdb::infer;
namespace fs = std::filesystem;

namespace {

// A temporary directory removed at the end of the test.
struct TempDir {
    fs::path path;
    TempDir() {
        std::string t = (fs::temp_directory_path() / "askmdb-engine-XXXXXX").string();
        REQUIRE(::mkdtemp(t.data()) != nullptr);
        path = t;
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Sets an environment variable and restores it at the end.
struct EnvVar {
    std::string name;
    std::optional<std::string> old;
    EnvVar(const char* n, const std::string& value) : name(n) {
        if (const char* v = std::getenv(n)) old = v;
        ::setenv(n, value.c_str(), 1);
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

unsigned mode_of(const fs::path& p) {
    struct stat st {};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    return st.st_mode & 07777;
}

// Creates dir/name with size bytes and an mtime age_s seconds in the past.
void make_file(const fs::path& dir, const std::string& name, std::size_t size, int age_s) {
    std::ofstream(dir / name, std::ios::binary) << std::string(size, 'x');
    fs::last_write_time(dir / name, fs::file_time_type::clock::now() - std::chrono::seconds(age_s));
}

std::vector<std::string> names_in(const fs::path& dir) {
    std::vector<std::string> out;
    for (const auto& e : fs::directory_iterator(dir)) out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

const char* test_model() {
    const char* m = std::getenv("ASKMDB_TEST_MODEL");
    return (m != nullptr && *m != '\0') ? m : nullptr;
}

std::string humo_block() { return read_file(fs::path(ASKMDB_TESTS_DIR) / "verify/schemas/humo.txt"); }

// The block with one listed value replaced, i.e. another schema of the same size.
std::string variant(const std::string& block, int i) {
    std::string out = block;
    const std::string from = "\"BLACKFRIDAY\"";
    const std::size_t at = out.find(from);
    REQUIRE(at != std::string::npos);
    out.replace(at, from.size(), "\"PROMO" + std::to_string(100 + i) + "\"");
    return out;
}

// The .state files in dir.
std::set<std::string> states_in(const fs::path& dir) {
    std::set<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".state") out.insert(e.path().filename().string());
    }
    return out;
}

// The one name in after that is not in before ("" if none or several).
std::string added(const std::set<std::string>& before, const std::set<std::string>& after) {
    std::vector<std::string> diff;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(), std::back_inserter(diff));
    return diff.size() == 1 ? diff[0] : std::string();
}

// ASKMDB_TEST_DEVICE=gpu runs the model suite on the first discrete GPU detect_hardware() finds.
LlamaOptions options(const char* model, std::chrono::milliseconds timeout = std::chrono::milliseconds(15000)) {
    LlamaOptions o;
    o.model_path = model;
    o.threads = default_threads();
    o.timeout = timeout;
    const char* device = std::getenv("ASKMDB_TEST_DEVICE");
    if (device != nullptr && std::string(device) == "gpu") {
        static const std::string gpu_id = [] {
            for (const GpuInfo& g : detect_hardware().gpus)
                if (g.kind == GpuKind::Discrete) return g.id;
            return std::string();
        }();
        if (gpu_id.empty()) FAIL("ASKMDB_TEST_DEVICE=gpu but detect_hardware() finds no discrete GPU");
        o.device = Device::Gpu;
        o.gpu_id = gpu_id;
    }
    return o;
}

// For tests of the cache mechanics only: the prefill (and the state saved) happens, generation
// stops after the first token.
constexpr std::chrono::milliseconds kNoGen{1};

const char* kQuestion = "¿cuántos pedidos cancelados hay de Madrid?";

#define SKIP_WITHOUT_MODEL(model)                                       \
    const char* model = test_model();                                   \
    if (model == nullptr) {                                             \
        MESSAGE("ASKMDB_TEST_MODEL not set: skipped");                  \
        return;                                                         \
    }

}  // namespace

TEST_SUITE("infer") {

TEST_CASE("llama: default_threads is between 1 and min(kMaxThreads, CPUs in the mask)") {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    REQUIRE(sched_getaffinity(0, sizeof(mask), &mask) == 0);
    const int n = default_threads();
    CHECK(n >= 1);
    CHECK(n <= kMaxThreads);
    CHECK(n <= CPU_COUNT(&mask));
}

TEST_CASE("llama: a missing model gives load_failed with the path, also the second time") {
    const std::string path = "/nonexistent/askmdb/model.gguf";
    LlamaOptions o;
    o.model_path = path;
    o.threads = 1;
    auto engine = make_llama_engine(o);
    for (int i = 0; i < 2; ++i) {  // the second call returns the same error without reloading
        EngineResult r = engine->suggest({"db=x\n", "how many?"});
        CHECK_FALSE(r.suggestion.has_value());
        CHECK(r.error == "load_failed");
        CHECK(r.error_detail.find(path) != std::string::npos);
        CHECK(r.prefix_cache == PrefixCache::Off);
    }
}

TEST_CASE("llama: a GPU that does not exist gives gpu_unavailable, also the second time") {
    LlamaOptions o;
    o.model_path = "/nonexistent/askmdb/model.gguf";
    o.device = Device::Gpu;
    o.gpu_id = "Vulkan99";
    o.threads = 1;
    auto engine = make_llama_engine(o);
    for (int i = 0; i < 2; ++i) {
        EngineResult r = engine->suggest({"db=x\n", "how many?"});
        CHECK_FALSE(r.suggestion.has_value());
        CHECK(r.error == "gpu_unavailable");
        CHECK(r.error_detail.find("Vulkan99") != std::string::npos);
    }
    o.gpu_id = "CPU";  // a device that is not a GPU
    CHECK(make_llama_engine(o)->suggest({"db=x\n", "how many?"}).error == "gpu_unavailable");
}

TEST_CASE("llama: the prefix state name depends on the device, the model and the prefix") {
    const std::string model = "model=a.gguf\nsize=1\nmtime=1.0\n";
    const std::string cpu = prefix_state_name(model, "cpu", "prefix");
    CHECK(cpu == prefix_state_name(model, "cpu", "prefix"));
    CHECK(cpu.size() == std::string("prefix-.state").size() + 64);
    CHECK(cpu.starts_with("prefix-"));
    CHECK(cpu.ends_with(".state"));
    const std::set<std::string> names = {cpu,
                                         prefix_state_name(model, "gpu:NVIDIA GeForce RTX 4080 Super", "prefix"),
                                         prefix_state_name(model, "gpu:AMD Radeon RX 7800 XT", "prefix"),
                                         prefix_state_name("model=b.gguf\nsize=1\nmtime=1.0\n", "cpu", "prefix"),
                                         prefix_state_name(model, "cpu", "prefix2")};
    CHECK(names.size() == 5);
}

TEST_CASE("llama: prune keeps the 8 most recent states and the one just saved") {
    TempDir tmp;
    const fs::path& dir = tmp.path;
    // prefix-00 is the newest; prefix-09 the oldest, but it is the one just saved.
    for (int i = 0; i < 10; ++i) make_file(dir, "prefix-0" + std::to_string(i) + ".state", 10, 100 + i * 10);
    make_file(dir, ".prefix.state-old", 10, 7200);      // abandoned temporary: removed
    make_file(dir, ".prefix.state-new", 10, 60);        // another process saving now: kept
    make_file(dir, "prefix-00.state.bak", 10, 7200);    // not a state: kept
    make_file(dir, "schema-abc", 10, 7200);             // the schema cache: kept
    fs::create_directory(dir / "prefix-dir.state");     // not a regular file: kept
    fs::create_symlink(dir / "schema-abc", dir / "prefix-link.state");  // a link: kept

    prune_prefix_cache(dir.string(), "prefix-09.state", std::time(nullptr));

    CHECK(names_in(dir) == std::vector<std::string>{".prefix.state-new", "prefix-00.state", "prefix-00.state.bak",
                                                    "prefix-01.state", "prefix-02.state", "prefix-03.state",
                                                    "prefix-04.state", "prefix-05.state", "prefix-06.state",
                                                    "prefix-09.state", "prefix-dir.state", "prefix-link.state",
                                                    "schema-abc"});
}

TEST_CASE("llama: prune without keep_name keeps exactly max_files") {
    TempDir tmp;
    for (int i = 0; i < 5; ++i) make_file(tmp.path, "prefix-" + std::to_string(i) + ".state", 1, i * 10);
    prune_prefix_cache(tmp.path.string(), "", std::time(nullptr), 3);
    CHECK(names_in(tmp.path) == std::vector<std::string>{"prefix-0.state", "prefix-1.state", "prefix-2.state"});
}

TEST_CASE("llama: prune by bytes removes from the first state that does not fit, newest first") {
    TempDir tmp;
    const fs::path& dir = tmp.path;
    make_file(dir, "prefix-keep.state", 40, 500);  // just saved (old mtime on purpose)
    make_file(dir, "prefix-a.state", 30, 10);      // 70
    make_file(dir, "prefix-b.state", 20, 20);      // 90
    make_file(dir, "prefix-c.state", 20, 30);      // 110 > 100: removed
    make_file(dir, "prefix-d.state", 5, 40);       // would fit, but it is older: removed
    prune_prefix_cache(dir.string(), "prefix-keep.state", std::time(nullptr), 8, 100);
    CHECK(names_in(dir) == std::vector<std::string>{"prefix-a.state", "prefix-b.state", "prefix-keep.state"});
}

TEST_CASE("llama: prune never removes keep_name, even alone over the byte cap") {
    TempDir tmp;
    make_file(tmp.path, "prefix-big.state", 200, 0);
    make_file(tmp.path, "prefix-small.state", 1, 0);
    prune_prefix_cache(tmp.path.string(), "prefix-big.state", std::time(nullptr), 8, 100);
    CHECK(names_in(tmp.path) == std::vector<std::string>{"prefix-big.state"});
}

TEST_CASE("llama: prune of a missing directory does not fail") {
    TempDir tmp;
    prune_prefix_cache((tmp.path / "missing").string(), "prefix-x.state", std::time(nullptr));
    CHECK(fs::is_empty(tmp.path));
}

}  // TEST_SUITE

TEST_SUITE("model") {

TEST_CASE("llama: the same raw output 3 times without cache and 3 times with cache miss/hit") {
    SKIP_WITHOUT_MODEL(model);
    TempDir tmp;
    const Request request{humo_block(), kQuestion};

    std::vector<std::string> raws;
    {
        // No cache: XDG_CACHE_HOME under a regular file, so the directory cannot be created.
        std::ofstream(tmp.path / "file") << "x";
        EnvVar xdg("XDG_CACHE_HOME", (tmp.path / "file" / "cache").string());
        auto engine = make_llama_engine(options(model));
        for (int i = 0; i < 3; ++i) {
            EngineResult r = engine->suggest(request);
            CHECK(r.prefix_cache == PrefixCache::Off);
            CHECK(r.error != "decode_failed");
            CHECK(r.error != "timeout");
            CHECK(r.timings.prefix_tokens > 0);
            CHECK(r.timings.prefix_tokens < r.timings.prompt_tokens);
            if (i > 0) CHECK(r.timings.load_ms == 0);
            raws.push_back(r.raw);
            MESSAGE("raw: " << r.raw << " | error: " << r.error << " " << r.error_detail);
            MESSAGE("tokens prompt/prefix/gen: " << r.timings.prompt_tokens << "/" << r.timings.prefix_tokens << "/"
                                                << r.timings.gen_tokens << ", ms load/prefill/gen: "
                                                << r.timings.load_ms << "/" << r.timings.prefill_ms << "/"
                                                << r.timings.generate_ms);
        }
    }
    {
        EnvVar xdg("XDG_CACHE_HOME", (tmp.path / "cache").string());
        const fs::path dir = tmp.path / "cache" / "askmdb";
        const std::vector<PrefixCache> expected = {PrefixCache::Miss, PrefixCache::Hit, PrefixCache::Hit};
        auto engine = make_llama_engine(options(model));
        for (int i = 0; i < 3; ++i) {
            if (i == 2) engine = make_llama_engine(options(model));  // a new process loads the state
            EngineResult r = engine->suggest(request);
            CHECK(r.prefix_cache == expected[static_cast<std::size_t>(i)]);
            raws.push_back(r.raw);
            MESSAGE("ms prefill (" << (i == 0 ? std::string("miss") : std::string("hit")) << "): " << r.timings.prefill_ms);
        }
        CHECK(mode_of(dir) == 0700);
        const std::set<std::string> files = states_in(dir);
        REQUIRE(files.size() == 1);
        CHECK(mode_of(dir / *files.begin()) == 0600);
        CHECK(files.begin()->size() == std::string("prefix-.state").size() + 64);
    }
    REQUIRE(raws.size() == 6);
    CHECK_FALSE(raws[0].empty());
    for (const std::string& raw : raws) CHECK(raw == raws[0]);
}

TEST_CASE("llama: a corrupt state gives the same raw output and is rewritten") {
    SKIP_WITHOUT_MODEL(model);
    TempDir tmp;
    EnvVar xdg("XDG_CACHE_HOME", tmp.path.string());
    const fs::path dir = tmp.path / "askmdb";
    const Request request{humo_block(), kQuestion};
    auto engine = make_llama_engine(options(model));

    const EngineResult first = engine->suggest(request);
    REQUIRE(first.prefix_cache == PrefixCache::Miss);
    const std::set<std::string> files = states_in(dir);
    REQUIRE(files.size() == 1);
    const fs::path state = dir / *files.begin();
    const std::string good = read_file(state);

    for (const std::string kind : {"truncated", "flipped byte", "other magic"}) {
        CAPTURE(kind);
        std::string bad = good;
        if (kind == "truncated") bad.resize(good.size() / 2);
        if (kind == "flipped byte") bad[good.size() / 2] = static_cast<char>(bad[good.size() / 2] ^ 0x01);
        if (kind == "other magic") bad.replace(0, 8, "ASKMPFX0");
        std::ofstream(state, std::ios::binary | std::ios::trunc) << bad;

        const EngineResult r = engine->suggest(request);
        CHECK(r.prefix_cache == PrefixCache::Miss);
        CHECK(r.raw == first.raw);
        CHECK(states_in(dir) == files);
        CHECK(read_file(state) == good);
        CHECK(mode_of(state) == 0600);
    }
    CHECK(engine->suggest(request).prefix_cache == PrefixCache::Hit);
}

TEST_CASE("llama: another schema block gives another state and keeps the old one") {
    SKIP_WITHOUT_MODEL(model);
    TempDir tmp;
    EnvVar xdg("XDG_CACHE_HOME", tmp.path.string());
    const fs::path dir = tmp.path / "askmdb";
    auto engine = make_llama_engine(options(model, kNoGen));

    CHECK(engine->suggest({humo_block(), kQuestion}).prefix_cache == PrefixCache::Miss);
    const std::set<std::string> a = states_in(dir);
    REQUIRE(a.size() == 1);
    CHECK(engine->suggest({variant(humo_block(), 0), kQuestion}).prefix_cache == PrefixCache::Miss);
    const std::set<std::string> ab = states_in(dir);
    CHECK(ab.size() == 2);
    CHECK(ab.count(*a.begin()) == 1);
    CHECK(engine->suggest({humo_block(), "otra pregunta"}).prefix_cache == PrefixCache::Hit);
}

TEST_CASE("llama: LRU keeps 8 states and removes the least recently used") {
    SKIP_WITHOUT_MODEL(model);
    TempDir tmp;
    EnvVar xdg("XDG_CACHE_HOME", tmp.path.string());
    const fs::path dir = tmp.path / "askmdb";
    auto engine = make_llama_engine(options(model, kNoGen));
    const std::string block = humo_block();

    std::vector<std::string> names;  // state of variant i
    for (int i = 0; i < 8; ++i) {
        const std::set<std::string> before = states_in(dir);
        CHECK(engine->suggest({variant(block, i), kQuestion}).prefix_cache == PrefixCache::Miss);
        names.push_back(added(before, states_in(dir)));
        REQUIRE_FALSE(names.back().empty());
    }
    // Variant 0 is used again: now variant 1 is the least recently used.
    CHECK(engine->suggest({variant(block, 0), kQuestion}).prefix_cache == PrefixCache::Hit);
    CHECK(engine->suggest({variant(block, 8), kQuestion}).prefix_cache == PrefixCache::Miss);

    const std::set<std::string> left = states_in(dir);
    CHECK(left.size() == kMaxPrefixStates);
    CHECK(left.count(names[0]) == 1);
    CHECK(left.count(names[1]) == 0);
    for (int i = 2; i < 8; ++i) CHECK(left.count(names[static_cast<std::size_t>(i)]) == 1);
}

TEST_CASE("llama: two alternating schemas A, B, A, B hit from the third request") {
    SKIP_WITHOUT_MODEL(model);
    TempDir tmp;
    EnvVar xdg("XDG_CACHE_HOME", tmp.path.string());
    auto engine = make_llama_engine(options(model, kNoGen));
    const std::string a = humo_block(), b = variant(a, 1);
    const std::vector<PrefixCache> expected = {PrefixCache::Miss, PrefixCache::Miss, PrefixCache::Hit,
                                               PrefixCache::Hit};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(engine->suggest({i % 2 == 0 ? a : b, kQuestion}).prefix_cache == expected[i]);
    }
}

TEST_CASE("llama: a huge schema block and a 1000-byte question give too_long") {
    SKIP_WITHOUT_MODEL(model);
    TempDir tmp;
    EnvVar xdg("XDG_CACHE_HOME", tmp.path.string());
    auto engine = make_llama_engine(options(model));
    std::string block = humo_block();
    for (int c = 0; block.size() < 16000; ++c) {
        block += "## extra" + std::to_string(c) + "\n_id oid\n";
        for (int f = 0; f < 10; ++f) block += "campo" + std::to_string(f) + " str [\"valor-" + std::to_string(f) + "\"]\n";
    }
    std::string question;
    while (question.size() + 10 <= kMaxQuestionBytes) question += "¿cuántos? ";

    const EngineResult r = engine->suggest({block, question});
    CHECK(r.error == "too_long");
    CHECK_FALSE(r.suggestion.has_value());
    CHECK(r.timings.prompt_tokens + kMaxGenTokens > kCtx);
    CHECK(r.raw.empty());
    MESSAGE("prompt tokens: " << r.timings.prompt_tokens << " | " << r.error_detail);
}

TEST_CASE("llama: a 1 ms timeout gives the timeout error and no suggestion") {
    SKIP_WITHOUT_MODEL(model);
    TempDir tmp;
    EnvVar xdg("XDG_CACHE_HOME", tmp.path.string());
    auto engine = make_llama_engine(options(model, std::chrono::milliseconds(1)));
    const EngineResult r = engine->suggest({humo_block(), kQuestion});
    CHECK(r.error == "timeout");
    CHECK(r.error_detail.find("1 ms") != std::string::npos);
    CHECK_FALSE(r.suggestion.has_value());
    CHECK(r.timings.gen_tokens >= 1);
}

TEST_CASE("llama: size of the prefix state with a block at the schema budget (printed, no threshold)") {
    SKIP_WITHOUT_MODEL(model);
    using askmdb::schema::Collection;
    using askmdb::schema::Path;
    using askmdb::schema::Schema;

    // A realistic schema bigger than the budget: render(schema, budget) trims it to fit.
    Schema s;
    s.db = "tienda";
    const std::vector<std::string> cities = {"Madrid", "Barcelona", "Valencia", "Sevilla", "Zaragoza", "Bilbao",
                                             "Málaga", "Murcia", "Palma", "Alicante", "Córdoba", "Valladolid"};
    for (const std::string name : {"clientes", "envios", "facturas", "pedidos", "productos", "proveedores", "tiendas"}) {
        Collection c;
        c.name = name;
        c.paths.push_back({"_id", {"oid"}, false, {}});
        c.paths.push_back({"fecha_alta", {"date"}, false, {}});
        c.paths.push_back({"nombre", {"str"}, false, {}});
        c.paths.push_back({"email", {"str"}, true, {}});
        c.paths.push_back({"estado", {"str"}, false, {"activo", "pendiente", "cancelado", "baja", "bloqueado"}});
        c.paths.push_back({"importe", {"decimal", "double"}, false, {}});
        c.paths.push_back({"direccion", {"obj"}, false, {}});
        c.paths.push_back({"direccion.calle", {"str"}, false, {}});
        c.paths.push_back({"direccion.ciudad", {"str"}, false, cities});
        c.paths.push_back({"direccion.codigo_postal", {"int", "str"}, true, {}});
        c.paths.push_back({"direccion.geo", {"obj"}, true, {}});
        c.paths.push_back({"direccion.geo.lat", {"double"}, true, {}});
        c.paths.push_back({"direccion.geo.lng", {"double"}, true, {}});
        c.paths.push_back({"canal", {"str"}, false, {"web", "tienda", "app", "telefono"}});
        c.paths.push_back({"etiquetas[]", {"str"}, true, {"vip", "nuevo", "mayorista", "recurrente", "moroso"}});
        c.paths.push_back({"lineas[]", {"obj"}, false, {}});
        c.paths.push_back({"lineas[].sku", {"str"}, false, {}});
        c.paths.push_back({"lineas[].cantidad", {"int"}, false, {}});
        c.paths.push_back({"lineas[].precio", {"double"}, false, {}});
        c.paths.push_back({"actualizado", {"date", "null"}, false, {}});
        askmdb::schema::upsert(s, std::move(c));
    }
    const askmdb::schema::Fit fit = askmdb::schema::render(s, askmdb::schema::kSchemaBudgetBytes);
    REQUIRE(fit.ok);

    const std::vector<std::pair<std::string, std::string>> blocks = {{"budget", fit.block}, {"humo", humo_block()}};
    for (const auto& [label, block] : blocks) {
        TempDir tmp;
        EnvVar xdg("XDG_CACHE_HOME", tmp.path.string());
        auto engine = make_llama_engine(options(model, kNoGen));
        const EngineResult r = engine->suggest({block, kQuestion});
        CHECK(r.prefix_cache == PrefixCache::Miss);
        const std::set<std::string> files = states_in(tmp.path / "askmdb");
        REQUIRE(files.size() == 1);
        const auto size = fs::file_size(tmp.path / "askmdb" / *files.begin());
        MESSAGE(label << ": block " << block.size() << " bytes, prefix " << r.timings.prefix_tokens
                      << " tokens, prompt " << r.timings.prompt_tokens << " tokens, state " << size << " bytes ("
                      << (static_cast<double>(size) / (1024.0 * 1024.0)) << " MiB), prefill "
                      << r.timings.prefill_ms << " ms");
    }
}

}  // TEST_SUITE
