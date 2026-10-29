#pragma once
// Database schema deduced from a sample of documents, its text block for the model (the format of
// the model contract) and its on-disk cache.
//
// The block given to the model is always rendered from the cached Schema, which is what makes the
// prompt deterministic. The verifier gets the full Schema, never the trimmed block.

#include <bson/bson.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace askmdb::schema {

// Sampling and format limits.
inline constexpr int kSampleSize = 200;              // find({}).sort({_id:-1}).limit(200)
inline constexpr int kMaxDepth = 4;                  // path depth (a.b.c.d)
inline constexpr std::size_t kMaxPaths = 40;         // per collection, first to appear
inline constexpr std::size_t kMaxValues = 12;        // distinct frequent values
inline constexpr std::size_t kMaxValueBytes = 30;    // per value
inline constexpr std::size_t kMinDocsForValues = 20; // sample size needed to list values
inline constexpr std::size_t kMinValueCount = 2;     // each listed value appears at least twice
// Budget of the whole block in bytes, so it does not depend on a tokenizer. Calibrated with the
// Qwen3.5 tokenizer: a realistic schema trimmed to it is ~1,260 tokens (3.5 bytes per token); one
// dense with value lists, ~1,900 (2.3 bytes per token), which still fits n_ctx.
inline constexpr std::size_t kSchemaBudgetBytes = 4500;
inline constexpr std::size_t kBytesPerToken = 3;      // for cost(): estimate only

struct Path {
    std::string path;                 // "estado", "cliente.ciudad", "items[]", "items[].sku"
    std::vector<std::string> types;   // short types, sorted: oid str int long double decimal bool
                                      // date obj arr null bin other
    bool optional = false;            // "?": missing in some sampled document
    std::vector<std::string> values;  // frequent string values: by frequency, ties alphabetical
};

struct Collection {
    std::string name;
    std::vector<Path> paths;  // in order of first appearance
};

struct Schema {
    std::string db;
    std::vector<Collection> collections;  // alphabetical by name
};

// Deduces the paths of one collection from its sampled documents (pure). The details (arrays,
// field names that are skipped, email/phone heuristics) are in infer.cpp.
Collection infer(std::string_view name, const std::vector<const bson_t*>& docs);

// Inserts or replaces a collection, keeping alphabetical order.
void upsert(Schema& schema, Collection collection);

// Text that may appear in the block as is: valid UTF-8 without C0 controls (not even "\n" or
// "\t"), DEL, C1 controls, bidi controls (the ones exec::escape_control rewrites) or "<|" (the
// start of the model's special tokens). Field names and values that fail it are not listed, so the
// block printed by --print-schema is byte for byte the one the model sees.
bool plain_text(std::string_view text);

// Exactly the block of the model contract: "db=<db>", then "## <collection>" and one line per path.
// No budget applied. Ends with "\n".
std::string render(const Schema& schema);

struct CollectionCost {
    std::string name;
    std::size_t tokens;  // estimated: bytes of its block / kBytesPerToken, rounded up
};

struct Fit {
    bool ok = false;
    std::string block;                  // rendered, trimmed, <= budget (if ok; empty if not)
    std::vector<CollectionCost> costs;  // per collection of the untrimmed schema (always filled)
};

// Renders within the budget with the deterministic trimming: first the value lists (from the last
// path to the first), then paths of depth 4, 3, 2 (last to first; never top-level paths). "Last to
// first" runs across collections: the last collection's last path goes first.
Fit render(const Schema& schema, std::size_t budget_bytes);

// Estimated tokens of a collection's block.
std::size_t cost(const Collection& collection);

struct ParseResult {
    std::optional<Schema> schema;
    std::string error;  // "line N: reason" (untranslated) if it does not parse
};

// Inverse of render(): render(*parse(b).schema) == b for any block render() produces.
ParseResult parse(std::string_view block);

// --- Cache: ~/.cache/askmdb/schema-<sha256(host_key + "/" + db)> -----------------------------

inline constexpr int kCacheFormatVersion = 1;
inline constexpr std::int64_t kCacheMaxAgeSeconds = 7 * 24 * 3600;

// $XDG_CACHE_HOME/askmdb if set and absolute (the XDG spec ignores a relative one); otherwise
// $HOME/.cache/askmdb. Empty if neither.
std::string cache_dir();

// Creates dir (and its parents) with mode 0700, or fixes its mode if it is ours; an error if it is
// not a directory of the current user. Shared by the schema cache and the prefix states.
bool ensure_cache_dir(const std::string& dir, std::string& error);

// cache_dir() + "/schema-" + sha256_hex(host_key + "/" + db). Empty if cache_dir() is empty.
std::string cache_path(std::string_view host_key, std::string_view db);

struct CacheEntry {
    Schema schema;            // full, untrimmed
    std::int64_t sampled_at = 0;  // unix seconds of the oldest sampling it contains
};

enum class CacheStatus { Fresh, Missing, Stale, Invalid };

struct CacheRead {
    CacheStatus status = CacheStatus::Missing;
    std::optional<CacheEntry> entry;  // only when Fresh
};

// Missing: no file. Invalid: unreadable, not JSON, another format_version or malformed. Stale: older
// than kCacheMaxAgeSeconds or sampled more than a day in the future. Fresh: entry filled.
CacheRead read_cache(const std::string& path, std::int64_t now);

// Directory 0700 (created with its parents, or its mode fixed if it is ours; an error if it is
// not ours), file 0600, atomic (mkstemp in the same directory, fsync, rename, fsync of the
// directory). false + error (untranslated) on failure.
bool write_cache(const std::string& path, const CacheEntry& entry, std::string& error);

}  // namespace askmdb::schema
