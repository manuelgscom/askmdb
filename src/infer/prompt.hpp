#pragma once
// Prompt of the model contract. The code is the source of truth;
// any change bumps kContractVersion and regenerates tests/golden.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "infer/engine.hpp"

namespace askmdb::infer {

inline constexpr std::size_t kMaxQuestionBytes = 1000;  // after normalize_request

// Instructions of contract v1 (no examples).
extern const std::string_view kSystemPrompt;

struct Message {
    std::string role;  // system | user | assistant
    std::string content;
    bool operator==(const Message&) const = default;
};

// Unicode whitespace (as Python's str.isspace) collapsed to one ASCII space, trimmed.
// Equivalent to Python's " ".join(question.split()). Invalid UTF-8 is kept as is.
std::string normalize_request(std::string_view question);

// system = kSystemPrompt + "\n\n" + schema block; user = question (already normalized).
std::vector<Message> build_messages(const Request& request);

// Equivalent to apply_chat_template(..., add_generation_prompt=True, enable_thinking=False).
std::string render_qwen35(const std::vector<Message>& messages);

// render_qwen35(build_messages(request)). The question must already be normalized.
std::string render_prompt(const Request& request);

// Bytes of render_prompt(request) that form the cacheable prefix: the system turn and the
// "<|im_start|>user\n" header. Computed from the structure, so text inside the question or the
// schema can never move the cut.
std::size_t prefix_bytes(const Request& request);

}  // namespace askmdb::infer
