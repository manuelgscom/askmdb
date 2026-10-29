#pragma once
// Command-line arguments.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"

namespace askmdb::cli {

struct Args {
    enum class Action { Run, Help, Version, BadFlag, MissingValue, BadValue, Conflict };
    Action action = Action::Run;
    std::string question;  // non-flag arguments, joined with spaces (not normalized)
    std::string bad_flag;  // BadFlag / MissingValue / BadValue: the flag; Conflict: "--print-schema --json"
    std::string bad_value; // BadValue: the value

    std::optional<std::string> uri;                       // --uri
    std::optional<std::string> db;                        // --db
    std::optional<std::vector<std::string>> collections;  // -c / --collections a,b
    std::optional<std::string> model;                     // --model PATH
    std::optional<std::string> schema_from;               // --schema-from FILE
    std::optional<config::Mode> mode;                     // --mode
    std::optional<config::Lang> lang;                     // --lang es|en
    std::optional<config::DeviceMode> device;             // --device auto|cpu|gpu
    bool detect = false;                                  // --detect
    bool refresh = false;                                 // --refresh
    bool print_schema = false;                            // --print-schema
    bool print_prompt = false;                            // --print-prompt
    bool json = false;                                    // --json
};

// args without argv[0]. "--" ends the flags. Flags with a value take the next argument (even if
// it starts with "-") or "--flag=value". Precedence of failures: unknown flag, missing value,
// bad value; then --help, then --version; then conflict. Conflicts: --print-schema, --print-prompt
// and --json are exclusive; --schema-from replaces the schema step, so --db and --refresh make no
// sense with it; --detect only detects and chooses, so it takes no --print-*, --schema-from,
// --model, --device or question (that last conflict has bad_flag "--detect" alone).
Args parse_args(const std::vector<std::string>& args);

}  // namespace askmdb::cli
