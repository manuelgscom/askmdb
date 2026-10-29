#pragma once
// The device and model chosen on the first start: $XDG_STATE_HOME/askmdb/choice, else
// ~/.local/state/askmdb/choice. "key = value" lines written by askmdb itself (unlike the config,
// which askmdb never writes). The config and the flags always take precedence over it.
//
//   device = gpu
//   model = askmdb-4b
//   gpu_id = Vulkan0
//   gpu_name = NVIDIA GeForce RTX 4080 Super
//   vram_free_mib = 12034
//   date = 2026-01-15T10:00:00Z

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "infer/hw.hpp"

namespace askmdb::config {

struct State {
    infer::Device device = infer::Device::Cpu;
    infer::ModelTier model = infer::ModelTier::M08B;
    std::string gpu_id;              // empty on the CPU
    std::string gpu_name;            // must still match the GPU at run time (with gpu_id)
    std::uint64_t vram_free_mib = 0; // at detection time; informative
    std::string date;                // UTC, ISO 8601; informative
};

// $XDG_STATE_HOME/askmdb/choice if set and non-empty; otherwise $HOME/.local/state/askmdb/choice.
// Empty if neither is available.
std::string state_path();

// nullopt if the text is not a valid state: unknown or repeated key, bad value, device or model
// missing, or gpu without gpu_id. Blank lines and "#" comments are allowed.
std::optional<State> parse_state(std::string_view text);

// The file's text. Control characters in the names are replaced with spaces.
std::string render_state(const State& state);

// A name as the file keeps it (one line, control characters as spaces, trimmed): what a loaded
// gpu_name is compared with.
std::string saved_name(std::string_view name);

// The saved state; nullopt if there is none or it cannot be read or parsed (treated as absent).
std::optional<State> load_state();

// Atomic write: directory created with 0700, temporary file, fsync, rename. false + error
// (untranslated) on failure.
bool save_state(const State& state, std::string& error);

// Removes the saved state (missing file = ok).
bool forget_state();

// Current time as "2026-01-15T10:00:00Z".
std::string utc_now_iso();

}  // namespace askmdb::config
