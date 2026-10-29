// Schema sampling: collection names and the newest documents of a collection.
#include <algorithm>

#include "mongo/client_impl.hpp"
#include "schema/schema.hpp"

namespace askmdb::mongo {

Result<std::vector<std::string>> Client::list_collections() {
    Error error;
    mongoc_client_t* client = impl_->get(error);
    if (!client) return failure<std::vector<std::string>>(error);

    mongoc_database_t* db = mongoc_client_get_database(client, impl_->db.c_str());
    BsonHandle opts;
    bson_append_bool(&opts.doc, "nameOnly", -1, true);
    bson_append_bool(&opts.doc, "authorizedCollections", -1, true);
    bson_error_t err{};
    char** names = mongoc_database_get_collection_names_with_opts(db, &opts.doc, &err);
    mongoc_database_destroy(db);
    if (!names) return failure<std::vector<std::string>>(impl_->error_from(err));

    std::vector<std::string> out;
    for (char** name = names; *name; ++name) {
        std::string_view view(*name);
        // A name that cannot be written in the block as is (see schema::plain_text) is not listed.
        if (!view.starts_with("system.") && schema::plain_text(view)) out.emplace_back(view);
    }
    bson_strfreev(names);
    std::sort(out.begin(), out.end());
    Result<std::vector<std::string>> result;
    result.value = std::move(out);
    return result;
}

Result<std::vector<Doc>> Client::sample(std::string_view collection) {
    Error error;
    mongoc_client_t* client = impl_->get(error);
    if (!client) return failure<std::vector<Doc>>(error);

    std::string name(collection);
    CollectionHandle coll{mongoc_client_get_collection(client, impl_->db.c_str(), name.c_str())};
    BsonHandle filter;
    BsonHandle opts;
    bson_t sort;
    bson_append_document_begin(&opts.doc, "sort", -1, &sort);
    bson_append_int32(&sort, "_id", -1, -1);
    bson_append_document_end(&opts.doc, &sort);
    bson_append_int64(&opts.doc, "limit", -1, kSampleLimit);
    bson_append_int64(&opts.doc, "maxTimeMS", -1, kSampleMaxTimeMs);
    CursorHandle cursor{mongoc_collection_find_with_opts(coll.ptr, &filter.doc, &opts.doc, nullptr)};

    std::vector<Doc> docs;
    std::size_t bytes = 0;
    const bson_t* doc = nullptr;
    while (bytes < kSampleMaxBytes && mongoc_cursor_next(cursor.ptr, &doc)) {
        bytes += doc->len;
        docs.push_back(adopt(bson_copy(doc)));
    }
    bson_error_t err{};
    if (mongoc_cursor_error(cursor.ptr, &err)) return failure<std::vector<Doc>>(impl_->error_from(err));
    Result<std::vector<Doc>> result;
    result.value = std::move(docs);
    return result;
}

}  // namespace askmdb::mongo
