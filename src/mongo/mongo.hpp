#pragma once
// The only module that talks to libmongoc. It reads: lists collections, samples them and runs
// aggregate (src/mongo/execute.cpp is the only file that calls the driver's aggregate;
// tests/mongoc_check.sh enforces it). Every text that leaves it is scrubbed of credentials.

#include <bson/bson.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/bson_doc.hpp"

namespace askmdb::mongo {

inline constexpr std::int64_t kServerSelectionTimeoutMs = 5000;
inline constexpr std::int64_t kConnectTimeoutMs = 5000;
inline constexpr std::int64_t kSampleLimit = 200;                 // newest documents by _id
inline constexpr std::int64_t kSampleMaxTimeMs = 2000;              // per collection (provisional)
inline constexpr std::size_t kSampleMaxBytes = 8 * 1024 * 1024;   // read per collection (provisional)
inline constexpr std::size_t kMaxCollections = 50;                // sampled without -c

// A parsed connection string. Credentials never leave this struct except through the driver.
struct Uri {
    std::string text;                // as given (may contain credentials: never print it)
    std::vector<std::string> hosts;  // "host:port" as in the URI (SRV: the SRV host name)
    std::string db;                  // database in the URI path; may be empty
    std::string user;                // empty if none
    std::string password;            // empty if none
    bool srv = false;
    bool tls = false;

    // nullopt + error (without credentials) if libmongoc rejects it.
    static std::optional<Uri> parse(std::string_view uri, std::string& error);

    bool has_password() const { return !password.empty(); }
    // "mongodb://h1:27017,h2:27017/db" or "mongodb+srv://host/db": hosts and db only.
    std::string redacted(std::string_view db_override = {}) const;
    // Normalized host list (lowercase, sorted, "srv:" prefix for SRV), without user: cache key.
    std::string host_key() const;
    // Replaces credentials (raw and percent-encoded) with "***": the URI text, the user info and the
    // password anywhere; the user name anywhere if it has 4+ characters, else only as a whole word.
    std::string scrub(std::string_view message) const;
};

struct Error {
    enum class Kind { Connect, Auth, Timeout, Server };
    Kind kind = Kind::Server;
    std::string message;  // from the driver or the server, already scrubbed, untranslated
};

template <class T>
struct Result {
    std::optional<T> value;
    Error error;  // meaningful only when !value
};

struct Limits {
    std::int64_t max_time_ms = 10000;
    int max_rows = 50;  // the executor appends {"$limit": max_rows + 1}
};

struct Rows {
    std::vector<Doc> docs;   // at most max_rows
    bool truncated = false;  // the server returned max_rows + 1
};

// Lazy client: nothing touches the network until the first call.
class Client {
public:
    Client(Uri uri, std::string db);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // listCollections (nameOnly, authorizedCollections), without system.*, alphabetical.
    Result<std::vector<std::string>> list_collections();
    // find({}).sort({_id:-1}).limit(200) with maxTimeMS and a byte cap.
    Result<std::vector<Doc>> sample(std::string_view collection);
    // aggregate with maxTimeMS, allowDiskUse:false and the final $limit. pipeline is the array
    // document from verify::load_pipeline.
    Result<Rows> aggregate(std::string_view collection, const bson_t& pipeline, const Limits& limits);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Run-time versions of the linked libraries, for --version: "libmongoc X, OpenSSL Y" (or
// "OpenSSL: none" in a build without TLS). Touches neither the network nor OpenSSL's config.
std::string library_versions();

// --- Presentation (pure) ---------------------------------------------------------------------

enum class NumberStyle { Es, En };  // 18.430,50 / 18,430.50

// Table: columns in order of first appearance of the top-level keys; integers with thousands
// separators; doubles with two decimals; dates ISO UTC; ObjectId hex; subdocuments and arrays as
// compact JSON cut at 40 characters with "…". Ends with "\n". Empty string if there are no rows.
std::string render_table(const std::vector<Doc>& rows, NumberStyle style);

// The rows as a JSON array of relaxed Extended JSON documents.
std::string rows_json(const std::vector<Doc>& rows);

// mongosh syntax: db.<coll>.aggregate([ ... ]) one stage per line; db.getCollection("…") for odd
// names. What is shown and what is copied.
std::string to_mongosh(std::string_view collection, const bson_t& pipeline);

// --- Internals exposed for tests (pure) -------------------------------------------------------

namespace detail {

// Maps a libmongoc error (error API v2) to a Kind; the message is scrubbed with `uri`.
// Server selection and stream errors: Connect. Authentication (client, SCRAM, server codes 18 and
// 13): Auth. Server code 50 (MaxTimeMSExpired): Timeout. Anything else: Server.
Error classify(std::uint32_t domain, std::uint32_t code, std::string_view message, const Uri& uri);

// Where OpenSSL should look for trusted CAs when the URI has no tlsCAFile. Exactly one of file or
// dir is set.
struct CaLocation {
    std::string file;
    std::string dir;
};
using GetEnv = std::function<std::optional<std::string>(const char* name)>;
using PathExists = std::function<bool(const std::string& path, bool is_dir)>;
// SSL_CERT_FILE, SSL_CERT_DIR, the distro bundles and /etc/ssl/certs/, in that order; a variable
// that is unset, empty or points nowhere is skipped. nullopt: none found (OpenSSL's defaults).
std::optional<CaLocation> find_ca(const GetEnv& getenv, const PathExists& exists);

// One table cell (already terminal-safe) and its width in terminal columns.
std::string format_cell(const bson_iter_t& value, NumberStyle style);
std::size_t display_width(std::string_view text);

}  // namespace detail

}  // namespace askmdb::mongo
