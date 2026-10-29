#include "verify/verify.hpp"

#include <cstdint>

namespace askmdb::verify {

Pipeline load_pipeline(std::string_view text) {
    Pipeline p;
    p.text = std::string(text);
    bson_error_t error;
    bson_t* doc = bson_new_from_json(reinterpret_cast<const std::uint8_t*>(p.text.data()),
                                     static_cast<ssize_t>(p.text.size()), &error);
    if (!doc) {
        p.parse_error = error.message;
        return p;
    }
    p.bson = std::shared_ptr<const bson_t>(doc, [](const bson_t* d) { bson_destroy(const_cast<bson_t*>(d)); });
    return p;
}

}  // namespace askmdb::verify
