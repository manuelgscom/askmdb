// On-disk cache of the full Schema, one JSON file per (host, db).

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <set>

#include "fetch/sha256.hpp"
#include "schema/schema.hpp"

namespace askmdb::schema {

namespace {

using nlohmann::json;

constexpr std::int64_t kFutureToleranceSeconds = 24 * 3600;
constexpr std::size_t kMaxCacheBytes = 64u << 20;

std::string sys_error(const std::string& what, const std::string& path) {
    return what + " " + path + ": " + std::strerror(errno);
}

bool is_type(const std::string& t) {
    static const std::set<std::string> kTypes = {"oid",  "str", "int", "long", "double", "decimal", "bool",
                                                 "date", "obj", "arr", "null", "bin",    "other"};
    return kTypes.count(t) > 0;
}

json to_json(const CacheEntry& e) {
    json cs = json::array();
    for (const auto& c : e.schema.collections) {
        json ps = json::array();
        for (const auto& p : c.paths)
            ps.push_back({{"path", p.path}, {"types", p.types}, {"optional", p.optional}, {"values", p.values}});
        cs.push_back({{"name", c.name}, {"paths", std::move(ps)}});
    }
    return {{"format_version", kCacheFormatVersion},
            {"sampled_at", e.sampled_at},
            {"db", e.schema.db},
            {"collections", std::move(cs)}};
}

bool string_list(const json& j, std::vector<std::string>& out) {
    if (!j.is_array()) return false;
    for (const auto& v : j) {
        if (!v.is_string()) return false;
        out.push_back(v.get<std::string>());
    }
    return true;
}

// Strict: anything render() could not write back as a valid block is malformed.
std::optional<CacheEntry> from_json(const json& j) {
    if (!j.is_object()) return std::nullopt;
    auto fv = j.find("format_version"), sa = j.find("sampled_at"), db = j.find("db"), cs = j.find("collections");
    if (fv == j.end() || sa == j.end() || db == j.end() || cs == j.end()) return std::nullopt;
    if (!fv->is_number_integer() || fv->get<std::int64_t>() != kCacheFormatVersion) return std::nullopt;
    if (!sa->is_number_integer() || !db->is_string() || db->get<std::string>().empty() || !cs->is_array())
        return std::nullopt;
    CacheEntry e;
    e.sampled_at = sa->get<std::int64_t>();
    e.schema.db = db->get<std::string>();
    for (const auto& c : *cs) {
        if (!c.is_object() || !c.contains("name") || !c["name"].is_string() || !c.contains("paths") ||
            !c["paths"].is_array())
            return std::nullopt;
        Collection col{c["name"].get<std::string>(), {}};
        if (col.name.empty()) return std::nullopt;
        if (!e.schema.collections.empty() && !(e.schema.collections.back().name < col.name)) return std::nullopt;
        for (const auto& p : c["paths"]) {
            if (!p.is_object() || !p.contains("path") || !p["path"].is_string() || !p.contains("optional") ||
                !p["optional"].is_boolean() || !p.contains("types") || !p.contains("values"))
                return std::nullopt;
            Path path;
            path.path = p["path"].get<std::string>();
            path.optional = p["optional"].get<bool>();
            if (path.path.empty() || !string_list(p["types"], path.types) || path.types.empty() ||
                !std::all_of(path.types.begin(), path.types.end(), is_type) || !string_list(p["values"], path.values))
                return std::nullopt;
            col.paths.push_back(std::move(path));
        }
        e.schema.collections.push_back(std::move(col));
    }
    return e;
}

// mkdir -p; directories it creates get 0700.
bool make_dirs(const std::string& dir, std::string& error) {
    for (std::size_t pos = 1; pos <= dir.size(); ++pos) {
        if (pos < dir.size() && dir[pos] != '/') continue;
        std::string sub = dir.substr(0, pos);
        if (::mkdir(sub.c_str(), 0700) != 0 && errno != EEXIST) {
            error = sys_error("cannot create", sub);
            return false;
        }
    }
    return true;
}

// The cache directory must be a directory of ours with mode 0700 (fixed if it is not).
bool prepare_dir(const std::string& dir, std::string& error) {
    if (!make_dirs(dir, error)) return false;
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0) {
        error = sys_error("cannot stat", dir);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        error = "not a directory: " + dir;
        return false;
    }
    if (st.st_uid != ::geteuid()) {
        error = "not owned by the current user: " + dir;
        return false;
    }
    if ((st.st_mode & 07777) != 0700 && ::chmod(dir.c_str(), 0700) != 0) {
        error = sys_error("cannot chmod", dir);
        return false;
    }
    return true;
}

bool write_all(int fd, const std::string& data) {
    std::size_t done = 0;
    while (done < data.size()) {
        auto n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

std::string cache_dir() {
    // A relative $XDG_CACHE_HOME is ignored, as the XDG spec says.
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0] == '/') return std::string(xdg) + "/askmdb";
    const char* home = std::getenv("HOME");
    if (home && home[0]) return std::string(home) + "/.cache/askmdb";
    return {};
}

bool ensure_cache_dir(const std::string& dir, std::string& error) { return prepare_dir(dir, error); }

std::string cache_path(std::string_view host_key, std::string_view db) {
    std::string dir = cache_dir();
    if (dir.empty()) return {};
    std::string key(host_key);
    key += '/';
    key += db;
    return dir + "/schema-" + fetch::sha256_hex(key);
}

CacheRead read_cache(const std::string& path, std::int64_t now) {
    CacheRead r;
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        r.status = errno == ENOENT ? CacheStatus::Missing : CacheStatus::Invalid;
        return r;
    }
    std::string data;
    char buf[65536];
    bool ok = true;
    while (true) {
        auto n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || data.size() + static_cast<std::size_t>(n) > kMaxCacheBytes) ok = false;
        if (n <= 0 || !ok) break;
        data.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    r.status = CacheStatus::Invalid;
    if (!ok) return r;
    auto j = json::parse(data, nullptr, false);
    if (j.is_discarded()) return r;
    auto e = from_json(j);
    if (!e) return r;
    auto age = static_cast<__int128>(now) - e->sampled_at;  // no overflow with absurd values
    if (age > kCacheMaxAgeSeconds || -age > kFutureToleranceSeconds) {
        r.status = CacheStatus::Stale;
        return r;
    }
    r.status = CacheStatus::Fresh;
    r.entry = std::move(e);
    return r;
}

bool write_cache(const std::string& path, const CacheEntry& entry, std::string& error) {
    auto slash = path.rfind('/');
    if (path.empty() || slash == std::string::npos || slash + 1 == path.size()) {
        error = "invalid cache path: " + path;
        return false;
    }
    std::string dir = slash == 0 ? "/" : path.substr(0, slash);
    if (!prepare_dir(dir, error)) return false;

    std::string data = to_json(entry).dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    std::string tmp = dir + "/.schema-XXXXXX";
    int fd = ::mkstemp(tmp.data());  // created 0600
    if (fd < 0) {
        error = sys_error("cannot create a temporary file in", dir);
        return false;
    }
    bool ok = ::fchmod(fd, 0600) == 0 && write_all(fd, data) && ::fsync(fd) == 0;
    if (!ok) error = sys_error("cannot write", tmp);
    if (::close(fd) != 0 && ok) {
        ok = false;
        error = sys_error("cannot write", tmp);
    }
    if (ok && ::rename(tmp.c_str(), path.c_str()) != 0) {
        ok = false;
        error = sys_error("cannot rename to", path);
    }
    if (!ok) {
        ::unlink(tmp.c_str());
        return false;
    }
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

}  // namespace askmdb::schema
