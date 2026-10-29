#pragma once

#include <string>
#include <string_view>

namespace askmdb::exec {

// Standard Base64 (RFC 4648, "+/" alphabet, with "=" padding).
std::string base64_encode(std::string_view data);

}  // namespace askmdb::exec
