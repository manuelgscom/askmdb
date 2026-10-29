#include "doctest.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "fetch/sha256.hpp"
#include "schema/schema.hpp"

using namespace askmdb::schema;

namespace {

// A temporary directory removed at the end of the test.
struct TempDir {
    std::string path;
    TempDir() {
        std::string t = (std::filesystem::temp_directory_path() / "askmdb-schema-XXXXXX").string();
        REQUIRE(::mkdtemp(t.data()) != nullptr);
        path = t;
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
        std::filesystem::remove_all(path, ec);
    }
};

// Sets (or unsets with nullopt) an environment variable and restores it at the end.
struct EnvVar {
    std::string name;
    std::optional<std::string> old;
    EnvVar(const char* n, std::optional<std::string> value) : name(n) {
        if (const char* v = std::getenv(n)) old = v;
        set(value);
    }
    EnvVar(const EnvVar&) = delete;
    EnvVar& operator=(const EnvVar&) = delete;
    ~EnvVar() { set(old); }
    void set(const std::optional<std::string>& v) {
        if (v)
            ::setenv(name.c_str(), v->c_str(), 1);
        else
            ::unsetenv(name.c_str());
    }
};

unsigned mode_of(const std::string& p) {
    struct stat st {};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    return st.st_mode & 07777;
}

std::vector<std::string> entries(const std::string& dir) {
    std::vector<std::string> out;
    for (const auto& e : std::filesystem::directory_iterator(dir)) out.push_back(e.path().filename().string());
    return out;
}

void write_file(const std::string& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << data;
}

CacheEntry sample_entry() {
    auto r = parse(
        "db=tienda\n"
        "## clientes\n_id str\nnombre completo str\n"
        "## pedidos\n_id oid\nestado str [\"entregado\",\"pendiente\"]\nciudad str [\"Málaga\",\"A Coruña\"]\n"
        "items[] obj\nitems[].sku? str\ncodigo int|str\n");
    REQUIRE_MESSAGE(r.schema, r.error);
    return CacheEntry{*r.schema, 1'700'000'000};
}

const std::string kValidJson =
    R"({"format_version":1,"sampled_at":1700000000,"db":"t","collections":[{"name":"c","paths":[)"
    R"({"path":"a","types":["int"],"optional":false,"values":[]}]}]})";

}  // namespace

TEST_SUITE("schema_cache") {

TEST_CASE("cache_dir: XDG_CACHE_HOME if absolute, else HOME/.cache, else empty") {
    EnvVar home("HOME", "/home/u");
    {
        EnvVar xdg("XDG_CACHE_HOME", "/x/cache");
        CHECK(cache_dir() == "/x/cache/askmdb");
    }
    {
        EnvVar xdg("XDG_CACHE_HOME", "");
        CHECK(cache_dir() == "/home/u/.cache/askmdb");
    }
    {
        EnvVar xdg("XDG_CACHE_HOME", "relative/cache");
        CHECK(cache_dir() == "/home/u/.cache/askmdb");
    }
    {
        EnvVar xdg("XDG_CACHE_HOME", std::nullopt);
        CHECK(cache_dir() == "/home/u/.cache/askmdb");
        EnvVar nohome("HOME", std::nullopt);
        CHECK(cache_dir().empty());
        CHECK(cache_path("h:27017", "tienda").empty());
    }
}

TEST_CASE("cache_path: sha256 of host_key + \"/\" + db") {
    EnvVar xdg("XDG_CACHE_HOME", "/x");
    CHECK(cache_path("h:27017", "tienda") ==
          "/x/askmdb/schema-" + askmdb::fetch::sha256_hex("h:27017/tienda"));
    CHECK(cache_path("h:27017", "tienda") != cache_path("h:27017", "otra"));
    // The key is exactly host_key + "/" + db.
    CHECK(cache_path("h:27017", "a/b") == cache_path("h:27017/a", "b"));
}

TEST_CASE("write then read: same schema, Fresh; directory 0700 and file 0600") {
    TempDir t;
    std::string dir = t.path + "/cache/askmdb";
    std::string p = dir + "/schema-x";
    auto e = sample_entry();
    std::string err;
    REQUIRE_MESSAGE(write_cache(p, e, err), err);
    CHECK(mode_of(t.path + "/cache") == 0700);
    CHECK(mode_of(dir) == 0700);
    CHECK(mode_of(p) == 0600);
    CHECK(entries(dir) == std::vector<std::string>{"schema-x"});

    auto r = read_cache(p, e.sampled_at + 10);
    REQUIRE(r.status == CacheStatus::Fresh);
    REQUIRE(r.entry);
    CHECK(r.entry->sampled_at == e.sampled_at);
    CHECK(render(r.entry->schema) == render(e.schema));
    const auto& items = r.entry->schema.collections[1].paths[4];
    CHECK(items.path == "items[].sku");
    CHECK(items.optional);
}

TEST_CASE("the file is JSON with the documented keys") {
    TempDir t;
    std::string p = t.path + "/schema-x";
    std::string err;
    REQUIRE(write_cache(p, sample_entry(), err));
    std::ifstream f(p);
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(data.starts_with(R"({"collections":[{"name":"clientes","paths":[{"optional":false,"path":"_id",)"
                           R"("types":["str"],"values":[]})"));
    CHECK(data.find(R"("format_version":1)") != std::string::npos);
    CHECK(data.find(R"("sampled_at":1700000000)") != std::string::npos);
    CHECK(data.find(R"("db":"tienda")") != std::string::npos);
}

TEST_CASE("an existing directory of ours gets its mode fixed to 0700") {
    TempDir t;
    std::string dir = t.path + "/askmdb";
    REQUIRE(::mkdir(dir.c_str(), 0755) == 0);
    REQUIRE(::chmod(dir.c_str(), 0755) == 0);
    std::string err;
    REQUIRE_MESSAGE(write_cache(dir + "/schema-x", sample_entry(), err), err);
    CHECK(mode_of(dir) == 0700);
}

TEST_CASE("the write is atomic: a new file replaces the old one and no temporary file is left") {
    TempDir t;
    std::string p = t.path + "/schema-x";
    write_file(p, "old");
    ::chmod(p.c_str(), 0644);
    struct stat before {};
    REQUIRE(::stat(p.c_str(), &before) == 0);
    std::string err;
    REQUIRE_MESSAGE(write_cache(p, sample_entry(), err), err);
    struct stat after {};
    REQUIRE(::stat(p.c_str(), &after) == 0);
    CHECK(after.st_ino != before.st_ino);  // renamed over, not rewritten in place
    CHECK(mode_of(p) == 0600);
    CHECK(entries(t.path) == std::vector<std::string>{"schema-x"});
    CHECK(read_cache(p, 1'700'000'000).status == CacheStatus::Fresh);
}

TEST_CASE("write errors leave nothing behind") {
    TempDir t;
    std::string file = t.path + "/file";
    write_file(file, "x");
    std::string err;
    CHECK_FALSE(write_cache(file + "/schema-x", sample_entry(), err));
    CHECK_FALSE(err.empty());
    err.clear();
    CHECK_FALSE(write_cache("", sample_entry(), err));
    CHECK_FALSE(err.empty());
    if (::geteuid() != 0) {
        std::string ro = t.path + "/ro";
        REQUIRE(::mkdir(ro.c_str(), 0700) == 0);
        REQUIRE(::chmod(ro.c_str(), 0500) == 0);
        err.clear();
        // The mode is fixed to 0700 first, so make it unwritable through a read-only parent instead.
        std::string sub = ro + "/askmdb";
        CHECK_FALSE(write_cache(sub + "/schema-x", sample_entry(), err));
        CHECK(err.find("cannot create") != std::string::npos);
    }
}

TEST_CASE("read: Missing, Invalid, Stale and Fresh") {
    TempDir t;
    std::string p = t.path + "/schema-x";
    const std::int64_t at = 1'700'000'000;

    CHECK(read_cache(p, at).status == CacheStatus::Missing);
    CHECK_FALSE(read_cache(p, at).entry);

    write_file(p, kValidJson);
    CHECK(read_cache(p, at).status == CacheStatus::Fresh);
    CHECK(read_cache(p, at + kCacheMaxAgeSeconds).status == CacheStatus::Fresh);
    CHECK(read_cache(p, at + kCacheMaxAgeSeconds + 1).status == CacheStatus::Stale);
    CHECK_FALSE(read_cache(p, at + kCacheMaxAgeSeconds + 1).entry);
    CHECK(read_cache(p, at - 24 * 3600).status == CacheStatus::Fresh);
    CHECK(read_cache(p, at - 24 * 3600 - 1).status == CacheStatus::Stale);

    auto invalid = [&](const std::string& data) {
        write_file(p, data);
        return read_cache(p, at).status == CacheStatus::Invalid;
    };
    auto replace = [](std::string s, const std::string& from, const std::string& to) {
        auto pos = s.find(from);
        REQUIRE(pos != std::string::npos);
        return s.replace(pos, from.size(), to);
    };
    CHECK(invalid(""));
    CHECK(invalid("not json"));
    CHECK(invalid("[]"));
    CHECK(invalid("{}"));
    CHECK(invalid(replace(kValidJson, R"("format_version":1)", R"("format_version":2)")));
    CHECK(invalid(replace(kValidJson, R"("format_version":1)", R"("format_version":"1")")));
    CHECK(invalid(replace(kValidJson, R"("sampled_at":1700000000)", R"("sampled_at":"x")")));
    CHECK(invalid(replace(kValidJson, R"("db":"t")", R"("db":"")")));
    CHECK(invalid(replace(kValidJson, R"("types":["int"])", R"("types":["integer"])")));
    CHECK(invalid(replace(kValidJson, R"("types":["int"])", R"("types":[])")));
    CHECK(invalid(replace(kValidJson, R"("path":"a")", R"("path":"")")));
    CHECK(invalid(replace(kValidJson, R"("optional":false)", R"("optional":0)")));
    CHECK(invalid(replace(kValidJson, R"("values":[])", R"("values":[1])")));
    CHECK(invalid(replace(kValidJson, R"(,"values":[])", "")));
    CHECK(invalid(replace(kValidJson, R"("name":"c")", R"("name":"")")));
    CHECK(invalid(
        replace(kValidJson, R"({"name":"c","paths":[)", R"({"name":"d","paths":[]},{"name":"c","paths":[)")));
    CHECK(invalid(kValidJson.substr(0, kValidJson.size() - 1)));

    if (::geteuid() != 0) {
        write_file(p, kValidJson);
        REQUIRE(::chmod(p.c_str(), 0) == 0);
        CHECK(read_cache(p, at).status == CacheStatus::Invalid);
    }
}

TEST_CASE("a schema read from the cache renders the same block it was written from") {
    TempDir t;
    std::string p = t.path + "/schema-x";
    auto e = sample_entry();
    std::string err;
    REQUIRE(write_cache(p, e, err));
    auto r = read_cache(p, e.sampled_at);
    REQUIRE(r.entry);
    auto block = render(r.entry->schema);
    auto again = parse(block);
    REQUIRE(again.schema);
    CHECK(render(*again.schema) == block);
}

}
