#pragma once
// Private to src/mongo: the lazily created libmongoc client shared by sample.cpp and execute.cpp.

#include <mongoc/mongoc.h>

#include <string>

#include "mongo/mongo.hpp"

namespace askmdb::mongo {

// libmongoc logs to stderr on its own, unscrubbed; askmdb reports errors itself. Idempotent.
void silence_driver_logs();

struct Client::Impl {
    Uri uri;
    std::string db;
    mongoc_client_t* client = nullptr;

    Impl(Uri u, std::string d) : uri(std::move(u)), db(std::move(d)) {}
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // Creates the client on first use (no network traffic yet). nullptr and `error` on failure.
    mongoc_client_t* get(Error& error);
    Error error_from(const bson_error_t& err) const;
};

template <class T>
Result<T> failure(Error error) {
    Result<T> out;
    out.error = std::move(error);
    return out;
}

// RAII for the handles used by one operation.
struct CollectionHandle {
    mongoc_collection_t* ptr;
    ~CollectionHandle() { mongoc_collection_destroy(ptr); }
};
struct CursorHandle {
    mongoc_cursor_t* ptr;
    ~CursorHandle() { mongoc_cursor_destroy(ptr); }
};
struct BsonHandle {
    bson_t doc;
    BsonHandle() { bson_init(&doc); }
    BsonHandle(const BsonHandle&) = delete;
    BsonHandle& operator=(const BsonHandle&) = delete;
    ~BsonHandle() { bson_destroy(&doc); }
};

}  // namespace askmdb::mongo
