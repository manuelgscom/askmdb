#include "config/config.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace askmdb::config {

namespace {

std::string_view trim(std::string_view s) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

// Strips trailing slashes ("/home/x/" -> "/home/x"; "/" -> "").
std::string_view strip_trailing_slashes(std::string_view s) {
    while (!s.empty() && s.back() == '/') s.remove_suffix(1);
    return s;
}

void append_hex(std::string& out, unsigned char c) {
    static const char digits[] = "0123456789abcdef";
    out += "\\x";
    out += digits[c >> 4];
    out += digits[c & 0x0f];
}

// Length of the valid UTF-8 sequence starting at s[i], or 0 if invalid.
// C1 controls (U+0080..U+009F) count as invalid: they get escaped.
std::size_t utf8_len(std::string_view s, std::size_t i) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    std::size_t n = 0;
    unsigned cp = 0;
    if (c >= 0xc2 && c <= 0xdf) {
        n = 2;
        cp = c & 0x1fu;
    } else if (c >= 0xe0 && c <= 0xef) {
        n = 3;
        cp = c & 0x0fu;
    } else if (c >= 0xf0 && c <= 0xf4) {
        n = 4;
        cp = c & 0x07u;
    } else {
        return 0;
    }
    if (i + n > s.size()) return 0;
    for (std::size_t k = 1; k < n; ++k) {
        if ((b(i + k) & 0xc0) != 0x80) return 0;
        cp = (cp << 6) | (b(i + k) & 0x3fu);
    }
    if (cp < 0xa0) return 0;  // C1 or overlong form
    if (n == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) return 0;
    if (n == 4 && (cp < 0x10000 || cp > 0x10ffff)) return 0;
    return n;
}

// Quoted value for a message: no control characters that could reach the terminal.
std::string quote(std::string_view s) {
    std::string out = "\"";
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
            ++i;
        } else if (c < 0x20 || c == 0x7f) {
            append_hex(out, c);
            ++i;
        } else if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
        } else if (const std::size_t n = utf8_len(s, i); n > 0) {
            out.append(s.substr(i, n));
            i += n;
        } else {
            append_hex(out, c);
            ++i;
        }
    }
    out += '"';
    return out;
}

ConfigResult fail(std::string_view filename, int line, const std::string& es, const std::string& en) {
    const std::string prefix = std::string(filename) + ":" + std::to_string(line) + ": ";
    return {std::nullopt, prefix + es, prefix + en};
}

// Integer in [lo, hi]: digits only, no sign; leading zeros do not count towards the length.
bool parse_int(std::string_view v, int lo, int hi, int& out) {
    if (v.empty()) return false;
    long n = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + (c - '0');
        if (n > hi) return false;  // stop before overflowing
    }
    if (n < lo) return false;
    out = static_cast<int>(n);
    return true;
}

constexpr std::string_view kSchemes[] = {"mongodb://", "mongodb+srv://"};

bool has_mongodb_scheme(std::string_view uri) {
    for (std::string_view s : kSchemes)
        if (uri.starts_with(s)) return true;
    return false;
}

constexpr const char* kKeys[] = {"mode",        "lang",     "model",      "threads", "device",
                                 "uri",         "collections", "max_rows", "max_time_ms"};
constexpr int kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);
constexpr const char* kKeyList = "mode, lang, model, threads, device, uri, collections, max_rows, max_time_ms";

}  // namespace

bool uri_has_password(std::string_view uri) {
    for (std::string_view s : kSchemes) {
        if (!uri.starts_with(s)) continue;
        std::string_view rest = uri.substr(s.size());
        // Conservative on purpose: the userinfo ends at the last "@" before the options, so an
        // unencoded "/" in a password still counts. A false positive only asks for 0600.
        rest = rest.substr(0, rest.find('?'));
        const std::size_t at = rest.rfind('@');
        if (at == std::string_view::npos) return false;
        const std::string_view userinfo = rest.substr(0, at);
        const std::size_t colon = userinfo.find(':');
        return colon != std::string_view::npos && colon + 1 < userinfo.size();
    }
    return false;
}

std::vector<std::string> split_list(std::string_view text) {
    std::vector<std::string> out;
    while (true) {
        const std::size_t comma = text.find(',');
        const std::string_view item = trim(text.substr(0, comma));
        if (!item.empty()) {
            bool seen = false;
            for (const auto& o : out) seen = seen || o == item;
            if (!seen) out.emplace_back(item);
        }
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    return out;
}

ConfigResult parse_config(std::string_view text, std::string_view filename, std::string_view home) {
    Config cfg;
    int seen_line[kKeyCount] = {};

    // Leading UTF-8 BOM: ignored.
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);

    int lineno = 0;
    while (!text.empty()) {
        ++lineno;
        const std::size_t nl = text.find('\n');
        const std::string_view raw = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);

        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#') continue;

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            return fail(filename, lineno, "falta \"=\" (formato: clave = valor)",
                        "missing \"=\" (format: key = value)");

        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));

        int k = -1;
        for (int i = 0; i < kKeyCount; ++i)
            if (key == kKeys[i]) k = i;
        if (k < 0)
            return fail(filename, lineno,
                        "clave desconocida: " + quote(key) + " (válidas: " + kKeyList + ")",
                        "unknown key: " + quote(key) + " (valid: " + kKeyList + ")");

        const std::string name = kKeys[k];
        if (seen_line[k] != 0)
            return fail(filename, lineno,
                        "clave repetida: " + name + " (ya definida en la línea " + std::to_string(seen_line[k]) + ")",
                        "duplicate key: " + name + " (already set on line " + std::to_string(seen_line[k]) + ")");
        seen_line[k] = lineno;

        if (value.empty()) return fail(filename, lineno, "valor vacío para " + name, "empty value for " + name);

        const auto invalid = [&](const std::string& valid_es, const std::string& valid_en) {
            return fail(filename, lineno,
                        "valor no válido para " + name + ": " + quote(value) + " (válidos: " + valid_es + ")",
                        "invalid value for " + name + ": " + quote(value) + " (valid: " + valid_en + ")");
        };
        const auto range = [&](int lo, int hi) {
            return invalid("entero de " + std::to_string(lo) + " a " + std::to_string(hi),
                           "integer from " + std::to_string(lo) + " to " + std::to_string(hi));
        };

        if (name == "mode") {
            if (value == "suggest") cfg.mode = Mode::Suggest;
            else if (value == "confirm") cfg.mode = Mode::Confirm;
            else if (value == "auto") cfg.mode = Mode::Auto;
            else return invalid("suggest, confirm, auto", "suggest, confirm, auto");
        } else if (name == "lang") {
            if (value == "auto") cfg.lang = Lang::Auto;
            else if (value == "es") cfg.lang = Lang::Es;
            else if (value == "en") cfg.lang = Lang::En;
            else return invalid("auto, es, en", "auto, es, en");
        } else if (name == "model") {
            if (value.starts_with("~/")) {
                const std::string_view base = strip_trailing_slashes(home);
                if (base.empty())
                    return fail(filename, lineno, "no se puede expandir \"~/\" en model: HOME no está definida",
                                "cannot expand \"~/\" in model: HOME is not set");
                cfg.model = std::string(base) + std::string(value.substr(1));
            } else {
                cfg.model = std::string(value);
            }
        } else if (name == "threads") {
            if (!parse_int(value, 1, kMaxThreads, cfg.threads)) return range(1, kMaxThreads);
        } else if (name == "device") {
            if (value == "auto") cfg.device = DeviceMode::Auto;
            else if (value == "cpu") cfg.device = DeviceMode::Cpu;
            else if (value == "gpu") cfg.device = DeviceMode::Gpu;
            else return invalid("auto, cpu, gpu", "auto, cpu, gpu");
        } else if (name == "uri") {
            // Never quoted back: it may carry a password.
            if (!has_mongodb_scheme(value))
                return fail(filename, lineno, "valor no válido para uri (tiene que empezar por mongodb:// o mongodb+srv://)",
                            "invalid value for uri (it must start with mongodb:// or mongodb+srv://)");
            cfg.uri = std::string(value);
        } else if (name == "collections") {
            cfg.collections = split_list(value);
            if (cfg.collections.empty())
                return invalid("nombres de colección separados por comas", "comma-separated collection names");
        } else if (name == "max_rows") {
            if (!parse_int(value, kMinMaxRows, kMaxMaxRows, cfg.max_rows)) return range(kMinMaxRows, kMaxMaxRows);
        } else {
            if (!parse_int(value, kMinMaxTimeMs, kMaxMaxTimeMs, cfg.max_time_ms))
                return range(kMinMaxTimeMs, kMaxMaxTimeMs);
        }
    }
    return {cfg, {}, {}};
}

std::string config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && *xdg != '\0') return std::string(strip_trailing_slashes(xdg)) + "/askmdb/config";
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return {};
    return std::string(strip_trailing_slashes(home)) + "/.config/askmdb/config";
}

ConfigResult load_config() {
    const std::string path = config_path();
    if (path.empty()) return {Config{}, {}, {}};

    const char* home_env = std::getenv("HOME");
    const std::string_view home = home_env != nullptr ? home_env : "";

    // Display name: $HOME/... -> ~/...
    std::string shown = path;
    const std::string_view base = strip_trailing_slashes(home);
    if (!base.empty() && path.size() > base.size() + 1 && path.compare(0, base.size(), base) == 0 &&
        path[base.size()] == '/')
        shown = "~" + path.substr(base.size());

    const auto unreadable = [&](const std::string& why_es, const std::string& why_en) {
        return ConfigResult{std::nullopt, shown + ": no se puede leer: " + why_es, shown + ": cannot read: " + why_en};
    };

    // One open + fstat: the mode checked is the one of the file actually read.
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return {Config{}, {}, {}};
        const std::string why = std::strerror(errno);
        return unreadable(why, why);
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        const std::string why = std::strerror(errno);
        ::close(fd);
        return unreadable(why, why);
    }
    if (S_ISDIR(st.st_mode)) {
        ::close(fd);
        return unreadable("es un directorio", "is a directory");
    }
    std::string text;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return unreadable("error de lectura", "read error");
        }
        if (n == 0) break;
        text.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);

    ConfigResult r = parse_config(text, shown, home);
    if (r.config && uri_has_password(r.config->uri) && (st.st_mode & 077) != 0) {
        char mode[8];
        std::snprintf(mode, sizeof mode, "%04o", static_cast<unsigned>(st.st_mode & 07777));
        return {std::nullopt,
                shown + ": la uri lleva contraseña y el fichero tiene permisos " + mode +
                    "; tiene que ser 0600 (chmod 600 " + shown + ")",
                shown + ": the uri has a password and the file has mode " + mode + "; it must be 0600 (chmod 600 " +
                    shown + ")"};
    }
    return r;
}

}  // namespace askmdb::config
