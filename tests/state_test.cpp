#include "doctest.h"

#include "config/state.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using namespace askmdb::config;
using askmdb::infer::Device;
using askmdb::infer::ModelTier;

namespace {

// Saves an environment variable and restores it when going out of scope.
class EnvGuard {
public:
    explicit EnvGuard(const char* name) : name_(name) {
        if (const char* v = std::getenv(name)) old_ = v;
    }
    ~EnvGuard() {
        if (old_) setenv(name_, old_->c_str(), 1);
        else unsetenv(name_);
    }
    void set(const std::string& v) { setenv(name_, v.c_str(), 1); }
    void unset() { unsetenv(name_); }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    const char* name_;
    std::optional<std::string> old_;
};

struct TempHome {
    std::filesystem::path path;
    EnvGuard xdg{"XDG_STATE_HOME"}, home{"HOME"};
    TempHome() {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("askmdb_state_test_" + std::to_string(getpid()) + "_" + std::to_string(counter++));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        home.set(path.string());
        xdg.unset();
    }
    ~TempHome() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

State gpu_state() {
    State s;
    s.device = Device::Gpu;
    s.model = ModelTier::M9B;
    s.gpu_id = "Vulkan0";
    s.gpu_name = "NVIDIA GeForce RTX 4080 Super";
    s.vram_free_mib = 14321;
    s.date = "2026-01-15T10:00:00Z";
    return s;
}

}  // namespace

TEST_SUITE("config") {

TEST_CASE("estado: ida y vuelta") {
    const State g = gpu_state();
    const std::string text = render_state(g);
    CHECK(text ==
          "device = gpu\nmodel = askmdb-9b\ngpu_id = Vulkan0\ngpu_name = NVIDIA GeForce RTX 4080 Super\n"
          "vram_free_mib = 14321\ndate = 2026-01-15T10:00:00Z\n");
    const auto back = parse_state(text);
    REQUIRE(back);
    CHECK(back->device == Device::Gpu);
    CHECK(back->model == ModelTier::M9B);
    CHECK(back->gpu_id == "Vulkan0");
    CHECK(back->gpu_name == g.gpu_name);
    CHECK(back->vram_free_mib == 14321);
    CHECK(back->date == g.date);

    State c;
    c.date = "2026-01-15T10:00:00Z";
    CHECK(render_state(c) == "device = cpu\nmodel = askmdb-0.8b\ndate = 2026-01-15T10:00:00Z\n");
    const auto cpu = parse_state(render_state(c));
    REQUIRE(cpu);
    CHECK(cpu->device == Device::Cpu);
    CHECK(cpu->model == ModelTier::M08B);
    CHECK(cpu->gpu_id.empty());

    // Names with control characters stay on one line.
    State odd = gpu_state();
    odd.gpu_name = "GPU\nmodel = askmdb-0.8b";
    const auto o = parse_state(render_state(odd));
    REQUIRE(o);
    CHECK(o->model == ModelTier::M9B);
    CHECK(o->gpu_name == "GPU model = askmdb-0.8b");
    // saved_name gives what a loaded name is compared with.
    CHECK(askmdb::config::saved_name(odd.gpu_name) == o->gpu_name);
    CHECK(askmdb::config::saved_name(" RTX 4080 Super\t") == "RTX 4080 Super");

    CHECK(parse_state("# comentario\n\n device = cpu \r\nmodel=askmdb-4b\n"));
}

TEST_CASE("estado: lo que no vale cuenta como ausente") {
    for (const char* text : {"", "device = cpu\n", "model = askmdb-0.8b\n", "device = tpu\nmodel = askmdb-0.8b\n",
                             "device = cpu\nmodel = askmdb-2b\n", "device = cpu\nmodel = askmdb-0.8b\nextra = 1\n",
                             "device = cpu\ndevice = cpu\nmodel = askmdb-0.8b\n", "device cpu\nmodel = askmdb-0.8b\n",
                             "device = gpu\nmodel = askmdb-4b\n",
                             "device = gpu\nmodel = askmdb-4b\ngpu_id = Vulkan0\nvram_free_mib = -3\n",
                             "device = gpu\nmodel = askmdb-4b\ngpu_id = Vulkan0\nvram_free_mib = 1e3\n",
                             "\x01\x02\x03"}) {
        CAPTURE(text);
        CHECK_FALSE(parse_state(text));
    }
}

TEST_CASE("estado: ruta según XDG_STATE_HOME y HOME") {
    EnvGuard xdg("XDG_STATE_HOME"), home("HOME");
    xdg.set("/x/state");
    home.set("/home/ana");
    CHECK(state_path() == "/x/state/askmdb/choice");
    xdg.set("relativo");
    CHECK(state_path() == "/home/ana/.local/state/askmdb/choice");
    xdg.unset();
    CHECK(state_path() == "/home/ana/.local/state/askmdb/choice");
    home.unset();
    CHECK(state_path().empty());
    CHECK_FALSE(load_state());
    std::string error;
    CHECK_FALSE(save_state(State{}, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("estado: guardar, leer y olvidar") {
    TempHome t;
    CHECK_FALSE(load_state());
    CHECK(forget_state());  // nothing to forget is fine
    std::string error;
    REQUIRE(save_state(gpu_state(), error));
    const std::filesystem::path dir = t.path / ".local" / "state" / "askmdb";
    struct stat st{};
    REQUIRE(::stat(dir.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
    REQUIRE(::stat((t.path / ".local" / "state").c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
    // No temporaries left behind.
    int files = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        CHECK(e.path().filename() == "choice");
        ++files;
    }
    CHECK(files == 1);
    const auto loaded = load_state();
    REQUIRE(loaded);
    CHECK(loaded->model == ModelTier::M9B);
    CHECK(loaded->vram_free_mib == 14321);

    // Overwritten in place.
    State cpu;
    REQUIRE(save_state(cpu, error));
    CHECK(load_state()->device == Device::Cpu);

    // A broken or unreadable file is absent, never an error.
    std::ofstream(dir / "choice") << "basura";
    CHECK_FALSE(load_state());
    std::ofstream(dir / "choice") << std::string(10000, '#');
    CHECK_FALSE(load_state());
    std::filesystem::remove(dir / "choice");
    std::filesystem::create_directory(dir / "choice");
    CHECK_FALSE(load_state());
    std::filesystem::remove(dir / "choice");

    REQUIRE(save_state(cpu, error));
    CHECK(forget_state());
    CHECK_FALSE(std::filesystem::exists(dir / "choice"));
    CHECK_FALSE(load_state());
}

TEST_CASE("utc_now_iso") {
    const std::string now = utc_now_iso();
    REQUIRE(now.size() == 20);
    CHECK(now[4] == '-');
    CHECK(now[10] == 'T');
    CHECK(now.back() == 'Z');
}

}  // TEST_SUITE("config")
