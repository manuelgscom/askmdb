#pragma once
// Owning handle for a libbson document, shared by schema/, verify/ and mongo/.

#include <bson/bson.h>

#include <memory>

namespace askmdb {

struct BsonDeleter {
    void operator()(bson_t* doc) const noexcept { bson_destroy(doc); }
};

using Doc = std::unique_ptr<bson_t, BsonDeleter>;

// Takes ownership of a heap document (bson_new, bson_copy, bson_new_from_json...). Null stays null.
inline Doc adopt(bson_t* doc) { return Doc(doc); }

}  // namespace askmdb
