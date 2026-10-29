#include "cli/args.hpp"

#include <utility>

namespace askmdb::cli {

namespace {

// Flags that take a value. "-c" is the short form of "--collections".
constexpr std::string_view kValueFlags[] = {"--uri",  "--db",   "--collections", "--model",
                                           "--mode", "--lang", "--schema-from", "--device"};

}  // namespace

Args parse_args(const std::vector<std::string>& args) {
    Args out;
    bool help = false, version = false, flags_done = false;
    std::string unknown, missing, bad_flag, bad_value;
    std::vector<std::string> words;

    const auto set_bad = [&](std::string_view flag, std::string_view value) {
        if (bad_flag.empty()) {
            bad_flag = flag;
            bad_value = value;
        }
    };

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (flags_done || a.size() < 2 || a[0] != '-') {
            if (!a.empty()) words.push_back(a);
            continue;
        }
        if (a == "--") {
            flags_done = true;
        } else if (a == "-h" || a == "--help") {
            help = true;
        } else if (a == "--version") {
            version = true;
        } else if (a == "--json") {
            out.json = true;
        } else if (a == "--refresh") {
            out.refresh = true;
        } else if (a == "--print-schema") {
            out.print_schema = true;
        } else if (a == "--print-prompt") {
            out.print_prompt = true;
        } else if (a == "--detect") {
            out.detect = true;
        } else {
            // A flag with a value: "--flag value", "--flag=value" or "-c value".
            std::string flag = a == "-c" ? "--collections" : a;
            std::optional<std::string> value;
            if (const auto eq = flag.find('='); eq != std::string::npos && flag.starts_with("--")) {
                value = flag.substr(eq + 1);
                flag.resize(eq);
            }
            bool known = false;
            for (std::string_view f : kValueFlags) known = known || flag == f;
            if (!known) {
                if (unknown.empty()) unknown = a;
                continue;
            }
            if (!value && i + 1 < args.size()) value = args[++i];
            if (!value || value->empty()) {
                if (missing.empty()) missing = a == "-c" ? "-c" : flag;
                continue;
            }
            if (flag == "--uri") {
                out.uri = *value;
            } else if (flag == "--db") {
                out.db = *value;
            } else if (flag == "--model") {
                out.model = *value;
            } else if (flag == "--schema-from") {
                out.schema_from = *value;
            } else if (flag == "--collections") {
                std::vector<std::string> list = config::split_list(*value);
                if (list.empty()) set_bad(a == "-c" ? "-c" : flag, *value);
                else out.collections = std::move(list);
            } else if (flag == "--mode") {
                if (*value == "suggest") out.mode = config::Mode::Suggest;
                else if (*value == "confirm") out.mode = config::Mode::Confirm;
                else if (*value == "auto") out.mode = config::Mode::Auto;
                else set_bad(flag, *value);
            } else if (flag == "--lang") {
                if (*value == "es") out.lang = config::Lang::Es;
                else if (*value == "en") out.lang = config::Lang::En;
                else set_bad(flag, *value);
            } else if (flag == "--device") {
                if (*value == "auto") out.device = config::DeviceMode::Auto;
                else if (*value == "cpu") out.device = config::DeviceMode::Cpu;
                else if (*value == "gpu") out.device = config::DeviceMode::Gpu;
                else set_bad(flag, *value);
            }
        }
    }
    for (const auto& w : words) {
        if (!out.question.empty()) out.question += ' ';
        out.question += w;
    }
    if (!unknown.empty()) {
        out.action = Args::Action::BadFlag;
        out.bad_flag = unknown;
    } else if (!missing.empty()) {
        out.action = Args::Action::MissingValue;
        out.bad_flag = missing;
    } else if (!bad_flag.empty()) {
        out.action = Args::Action::BadValue;
        out.bad_flag = bad_flag;
        out.bad_value = bad_value;
    } else if (help) {
        out.action = Args::Action::Help;
    } else if (version) {
        out.action = Args::Action::Version;
    } else {
        // The first pair that clashes, in a fixed order.
        const std::pair<bool, const char*> conflicts[] = {
            {out.print_schema && out.json, "--print-schema --json"},
            {out.print_prompt && out.json, "--print-prompt --json"},
            {out.print_schema && out.print_prompt, "--print-schema --print-prompt"},
            {out.schema_from && out.db, "--schema-from --db"},
            {out.schema_from && out.refresh, "--schema-from --refresh"},
            {out.detect && out.print_schema, "--detect --print-schema"},
            {out.detect && out.print_prompt, "--detect --print-prompt"},
            {out.detect && out.schema_from, "--detect --schema-from"},
            {out.detect && out.model, "--detect --model"},
            {out.detect && out.device, "--detect --device"},
            {out.detect && !out.question.empty(), "--detect"},
        };
        out.action = Args::Action::Run;
        for (const auto& [clash, flags] : conflicts) {
            if (!clash) continue;
            out.action = Args::Action::Conflict;
            out.bad_flag = flags;
            break;
        }
    }
    return out;
}

}  // namespace askmdb::cli
