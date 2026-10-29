#pragma once
// User configuration: ~/.config/askmdb/config, "key = value". askmdb never creates or edits it.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace askmdb::config {

enum class Mode { Suggest, Confirm, Auto };
enum class Lang { Auto, Es, En };
// Where the model runs: auto = the saved choice, or detection on the first start.
enum class DeviceMode { Auto, Cpu, Gpu };

inline constexpr int kDefaultMaxRows = 50;
inline constexpr int kMinMaxRows = 1;
inline constexpr int kMaxMaxRows = 1000;
inline constexpr int kDefaultMaxTimeMs = 10000;
inline constexpr int kMinMaxTimeMs = 100;
inline constexpr int kMaxMaxTimeMs = 300000;
inline constexpr int kMaxThreads = 256;

struct Config {
    Mode mode = Mode::Confirm;
    Lang lang = Lang::Auto;
    std::string model;                     // empty = default model; "~/" already expanded
    int threads = 0;                       // 0 = automatic
    DeviceMode device = DeviceMode::Auto;
    std::string uri;                       // may contain credentials: never print it
    std::vector<std::string> collections;  // empty = all (with the server-side cap)
    int max_rows = kDefaultMaxRows;
    int max_time_ms = kDefaultMaxTimeMs;
};

// Empty config => error in the format "file:line: reason", in both languages
// (the UI language may depend on the broken file itself; the CLI picks one).
struct ConfigResult {
    std::optional<Config> config;
    std::string error;     // Spanish
    std::string error_en;  // English
};

// filename is only used in error messages; home, to expand "~/" in model.
ConfigResult parse_config(std::string_view text, std::string_view filename, std::string_view home = {});

// true if a mongodb:// or mongodb+srv:// URI carries a non-empty password in its authority
// ("user:pass@host"). Does not need libmongoc.
bool uri_has_password(std::string_view uri);

// "a, b,,c" style list: trimmed, empty items dropped, duplicates removed (first kept).
std::vector<std::string> split_list(std::string_view text);

// $XDG_CONFIG_HOME/askmdb/config if set and non-empty; otherwise $HOME/.config/askmdb/config.
std::string config_path();

// Missing file => defaults. A file with a password in its uri must have no group/other
// permission bits (0600), or this fails. askmdb never creates the file.
ConfigResult load_config();

}  // namespace askmdb::config
