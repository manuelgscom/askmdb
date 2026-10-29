// The executor: the only place in src/ that runs anything on the server besides sampling.
#include <cstdio>

#include "mongo/client_impl.hpp"

namespace askmdb::mongo {

Result<Rows> Client::aggregate(std::string_view collection, const bson_t& pipeline, const Limits& limits) {
    Error error;
    mongoc_client_t* client = impl_->get(error);
    if (!client) return failure<Rows>(error);

    const std::int64_t fetch = static_cast<std::int64_t>(limits.max_rows) + 1;

    // A copy of the verified stages, byte for byte, plus the final $limit.
    BsonHandle stages;
    std::uint32_t index = 0;
    bson_iter_t it;
    if (bson_iter_init(&it, &pipeline)) {
        while (bson_iter_next(&it)) {
            char buf[16];
            const char* key = nullptr;
            auto len = bson_uint32_to_string(index++, &key, buf, sizeof buf);
            bson_append_iter(&stages.doc, key, static_cast<int>(len), &it);
        }
    }
    {
        char buf[16];
        const char* key = nullptr;
        auto len = bson_uint32_to_string(index, &key, buf, sizeof buf);
        bson_t limit;
        bson_append_document_begin(&stages.doc, key, static_cast<int>(len), &limit);
        bson_append_int64(&limit, "$limit", -1, fetch);
        bson_append_document_end(&stages.doc, &limit);
    }

    BsonHandle opts;
    bson_append_int64(&opts.doc, "maxTimeMS", -1, limits.max_time_ms);
    bson_append_bool(&opts.doc, "allowDiskUse", -1, false);
    bson_append_int32(&opts.doc, "batchSize", -1, static_cast<std::int32_t>(fetch));

    std::string name(collection);
    CollectionHandle coll{mongoc_client_get_collection(client, impl_->db.c_str(), name.c_str())};
    CursorHandle cursor{mongoc_collection_aggregate(coll.ptr, MONGOC_QUERY_NONE, &stages.doc, &opts.doc, nullptr)};

    Rows rows;
    std::int64_t seen = 0;
    const bson_t* doc = nullptr;
    while (seen < fetch && mongoc_cursor_next(cursor.ptr, &doc)) {
        if (++seen <= limits.max_rows) rows.docs.push_back(adopt(bson_copy(doc)));
    }
    bson_error_t err{};
    if (mongoc_cursor_error(cursor.ptr, &err)) return failure<Rows>(impl_->error_from(err));
    rows.truncated = seen > limits.max_rows;
    Result<Rows> result;
    result.value = std::move(rows);
    return result;
}

}  // namespace askmdb::mongo
