#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace askmdb::cli {

// The process' terminal, injectable so that the orchestration can be tested in-process.
struct Host {
    std::function<void(std::string_view)> out;
    std::function<void(std::string_view)> err;
    std::function<std::optional<std::string>()> read_line;  // stdin, without "\n"; nullopt = EOF
    std::function<bool(const std::string&)> copy;           // clipboard
    bool stdin_tty = false;
    bool stdout_tty = false;
    bool stderr_tty = false;
};

// The whole program over a host (args without argv[0]). Returns the exit code. Reads the
// environment, the config file, the schema cache and talks to MongoDB.
int run(const std::vector<std::string>& args, const Host& host);

// The real process: stdin/stdout/stderr, the clipboard and the SIGINT handler (exit 130).
int run(int argc, char** argv);

}  // namespace askmdb::cli
