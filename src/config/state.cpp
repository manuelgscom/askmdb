#include "config/state.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <utility>

namespace askmdb::config {

namespace {

constexpr std::size_t kMaxStateBytes = 4096;

std::string_view trim(std::string_view s) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

// Unsigned decimal without sign, at most 12 digits (MiB).
std::optional<std::uint64_t> parse_u64(std::string_view v) {
    if (v.empty() || v.size() > 12) return std::nullopt;
    std::uint64_t n = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return std::nullopt;
        n = n * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return n;
}

// One line, no control characters: what goes into the file can always be read back.
std::string one_line(std::string_view s) {
    std::string out;
    for (char c : s) out += static_cast<unsigned char>(c) < 0x20 || c == 0x7f ? ' ' : c;
    return std::string(trim(out));
}

std::string sys_error(const std::string& what, const std::string& path) {
    return what + " " + path + ": " + std::strerror(errno);
}

// mkdir -p; new components get 0700 (XDG base directories).
bool make_dirs(const std::string& dir, std::string& error) {
    for (std::size_t pos = 1; pos <= dir.size(); ++pos) {
        if (pos < dir.size() && dir[pos] != '/') continue;
        const std::string part = dir.substr(0, pos);
        if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) {
            error = sys_error("cannot create", part);
            return false;
        }
    }
    struct stat st{};
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        error = "not a directory: " + dir;
        return false;
    }
    return true;
}

bool write_all(int fd, const std::string& data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const auto n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

std::string state_path() {
    // A relative $XDG_STATE_HOME is ignored, as the XDG spec says.
    const char* xdg = std::getenv("XDG_STATE_HOME");
    if (xdg != nullptr && xdg[0] == '/') return std::string(xdg) + "/askmdb/choice";
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') return std::string(home) + "/.local/state/askmdb/choice";
    return {};
}

std::optional<State> parse_state(std::string_view text) {
    State s;
    bool has_device = false, has_model = false;
    bool seen_gpu_id = false, seen_gpu_name = false, seen_vram = false, seen_date = false;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        const std::string_view line = trim(text.substr(0, nl));
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        if (line.empty() || line.front() == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) return std::nullopt;
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        const auto once = [](bool& seen) { return !std::exchange(seen, true); };
        if (key == "device") {
            if (!once(has_device)) return std::nullopt;
            if (value == "cpu") s.device = infer::Device::Cpu;
            else if (value == "gpu") s.device = infer::Device::Gpu;
            else return std::nullopt;
        } else if (key == "model") {
            if (!once(has_model)) return std::nullopt;
            const auto tier = infer::parse_tier(value);
            if (!tier) return std::nullopt;
            s.model = *tier;
        } else if (key == "gpu_id") {
            if (!once(seen_gpu_id)) return std::nullopt;
            s.gpu_id = value;
        } else if (key == "gpu_name") {
            if (!once(seen_gpu_name)) return std::nullopt;
            s.gpu_name = value;
        } else if (key == "vram_free_mib") {
            if (!once(seen_vram)) return std::nullopt;
            const auto n = parse_u64(value);
            if (!n) return std::nullopt;
            s.vram_free_mib = *n;
        } else if (key == "date") {
            if (!once(seen_date)) return std::nullopt;
            s.date = value;
        } else {
            return std::nullopt;
        }
    }
    if (!has_device || !has_model) return std::nullopt;
    if (s.device == infer::Device::Gpu && s.gpu_id.empty()) return std::nullopt;
    return s;
}

std::string render_state(const State& s) {
    std::string out;
    out += "device = " + std::string(infer::device_name(s.device)) + "\n";
    out += "model = " + std::string(infer::tier_name(s.model)) + "\n";
    if (s.device == infer::Device::Gpu) {
        out += "gpu_id = " + one_line(s.gpu_id) + "\n";
        if (const std::string name = one_line(s.gpu_name); !name.empty()) out += "gpu_name = " + name + "\n";
        out += "vram_free_mib = " + std::to_string(s.vram_free_mib) + "\n";
    }
    if (const std::string date = one_line(s.date); !date.empty()) out += "date = " + date + "\n";
    return out;
}

std::string saved_name(std::string_view name) { return one_line(name); }

std::optional<State> load_state() {
    const std::string path = state_path();
    if (path.empty()) return std::nullopt;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return std::nullopt;
    }
    std::string text;
    char buf[1024];
    bool ok = true;
    while (text.size() <= kMaxStateBytes) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) ok = false;
        if (n <= 0) break;
        text.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    if (!ok || text.size() > kMaxStateBytes) return std::nullopt;
    return parse_state(text);
}

bool save_state(const State& state, std::string& error) {
    const std::string path = state_path();
    if (path.empty()) {
        error = "neither XDG_STATE_HOME nor HOME is set";
        return false;
    }
    const std::string dir = path.substr(0, path.rfind('/'));
    if (!make_dirs(dir, error)) return false;

    const std::string data = render_state(state);
    std::string tmp = dir + "/.choice-XXXXXX";
    const int fd = ::mkstemp(tmp.data());  // created 0600
    if (fd < 0) {
        error = sys_error("cannot create a temporary file in", dir);
        return false;
    }
    bool ok = ::fchmod(fd, 0644) == 0 && write_all(fd, data) && ::fsync(fd) == 0;
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
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

bool forget_state() {
    const std::string path = state_path();
    return path.empty() || ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

std::string utc_now_iso() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    char buf[32];
    if (::gmtime_r(&now, &tm) == nullptr || std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) return {};
    return buf;
}

}  // namespace askmdb::config
