// Deduction of a collection's paths from its sampled documents.
//
// Field names that cannot be written unambiguously in the block are skipped together with their
// children: empty names, names that are not plain_text() (control or bidi characters, "<|", not
// valid UTF-8), names with "." (they would read as a subdocument), and top-level names starting
// with "## " (they would read as a collection header). A value that is not plain_text() drops the
// path's value list.
//
// Arrays: "x[]" holds the element types and "x[].f" the fields of its elements; "x[]" is present
// in a document where x is an array (even empty). A field that is only ever an array prints just
// "x[]", in the place of "x"; one that is also something else keeps "x arr|..." as well. A field
// that is always an empty array is "x arr". Nested arrays stop at the element type "arr".

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

#include "schema/schema.hpp"

namespace askmdb::schema {

namespace {

const char* short_type(bson_type_t t) {
    switch (t) {
    case BSON_TYPE_DOUBLE: return "double";
    case BSON_TYPE_UTF8: return "str";
    case BSON_TYPE_DOCUMENT: return "obj";
    case BSON_TYPE_ARRAY: return "arr";
    case BSON_TYPE_BINARY: return "bin";
    case BSON_TYPE_OID: return "oid";
    case BSON_TYPE_BOOL: return "bool";
    case BSON_TYPE_DATE_TIME: return "date";
    case BSON_TYPE_NULL: return "null";
    case BSON_TYPE_INT32: return "int";
    case BSON_TYPE_INT64: return "long";
    case BSON_TYPE_DECIMAL128: return "decimal";
    default: return "other";  // regex, timestamp, minKey, maxKey, code, symbol, dbPointer...
    }
}

bool valid_key(const char* key, std::size_t len, bool top_level) {
    const std::string_view k(key, len);
    if (k.empty() || k.find('.') != std::string_view::npos) return false;
    if (top_level && k.starts_with("## ")) return false;
    return plain_text(k);
}

// "x@y": an '@' with something on both sides.
bool looks_like_email(std::string_view v) {
    auto at = v.find('@');
    return at != std::string_view::npos && at > 0 && at + 1 < v.size();
}

// Optional '+', then only digits, spaces and - . ( ), with 7 to 15 digits. An ISO date
// (YYYY-MM-DD) is not a phone.
bool looks_like_phone(std::string_view v) {
    if (v.size() == 10 && v[4] == '-' && v[7] == '-') {
        bool date = true;
        for (std::size_t i = 0; i < v.size(); ++i)
            if (i != 4 && i != 7 && (v[i] < '0' || v[i] > '9')) date = false;
        if (date) return false;
    }
    if (!v.empty() && v.front() == '+') v.remove_prefix(1);
    std::size_t digits = 0;
    for (char c : v) {
        if (c >= '0' && c <= '9')
            ++digits;
        else if (c != ' ' && c != '-' && c != '.' && c != '(' && c != ')')
            return false;
    }
    return digits >= 7 && digits <= 15;
}

struct Acc {
    std::string path;
    std::set<std::string> types;
    std::size_t docs_present = 0;      // documents where the path appears
    std::size_t last_doc = SIZE_MAX;
    std::size_t array_docs = 0;        // documents where the value is an array (for "x[]")
    std::size_t last_array_doc = SIZE_MAX;
    std::size_t elements = SIZE_MAX;   // index of the "x[]" accumulator, if any element was seen
    std::size_t array_of = SIZE_MAX;   // for "x[]": index of the "x" accumulator
    bool values_ok = true;             // every non-null value so far is a short, non-private string
    std::map<std::string, std::size_t> counts;
};

class Inferrer {
public:
    explicit Inferrer(std::size_t ndocs) : ndocs_(ndocs) {}

    void document(const bson_t* doc, std::size_t index) {
        doc_ = index;
        bson_iter_t it;
        if (bson_iter_init(&it, doc)) children(it, "", 1);
    }

    Collection finish(std::string_view name) const {
        Collection c{std::string(name), {}};
        for (const auto& a : accs_) {
            if (c.paths.size() == kMaxPaths) break;
            if (a.array_of != SIZE_MAX && redundant(accs_[a.array_of])) continue;  // emitted at "x"
            const Acc& e = redundant(a) ? accs_[a.elements] : a;
            Path p;
            p.path = e.path;
            p.types.assign(e.types.begin(), e.types.end());
            p.optional = e.array_of != SIZE_MAX ? accs_[e.array_of].array_docs < ndocs_ : e.docs_present < ndocs_;
            p.values = values(e);
            c.paths.push_back(std::move(p));
        }
        return c;
    }

private:
    // "x" that is only ever an array with elements: its line is "x[]", in the place of "x".
    static bool redundant(const Acc& a) {
        return a.elements != SIZE_MAX && a.types.size() == 1 && *a.types.begin() == "arr";
    }

    std::vector<std::string> values(const Acc& a) const {
        if (ndocs_ < kMinDocsForValues || !a.values_ok || a.counts.empty()) return {};
        std::vector<std::pair<std::string, std::size_t>> v(a.counts.begin(), a.counts.end());
        for (const auto& [s, n] : v)
            if (n < kMinValueCount) return {};
        std::stable_sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
        std::vector<std::string> out;
        for (auto& [s, n] : v) out.push_back(std::move(s));
        return out;
    }

    std::size_t acc(const std::string& path) {
        auto [it, inserted] = index_.try_emplace(path, accs_.size());
        if (inserted) {
            accs_.emplace_back();
            accs_.back().path = path;
        }
        Acc& a = accs_[it->second];
        if (a.last_doc != doc_) {
            a.last_doc = doc_;
            ++a.docs_present;
        }
        return it->second;
    }

    void children(bson_iter_t& it, const std::string& prefix, int depth) {
        while (bson_iter_next(&it)) {
            const char* key = bson_iter_key(&it);
            std::size_t len = bson_iter_key_len(&it);
            if (!valid_key(key, len, prefix.empty())) continue;
            value(it, prefix + std::string(key, len), depth);
        }
    }

    // The value of a field.
    void value(bson_iter_t& it, const std::string& path, int depth) {
        std::size_t i = acc(path);
        bson_type_t t = bson_iter_type(&it);
        accs_[i].types.insert(short_type(t));
        count_value(accs_[i], it, t);
        bson_iter_t child;
        if (t == BSON_TYPE_DOCUMENT) {
            if (depth < kMaxDepth && bson_iter_recurse(&it, &child)) children(child, path + ".", depth + 1);
        } else if (t == BSON_TYPE_ARRAY) {
            if (accs_[i].last_array_doc != doc_) {
                accs_[i].last_array_doc = doc_;
                ++accs_[i].array_docs;
            }
            if (!bson_iter_recurse(&it, &child)) return;
            while (bson_iter_next(&child)) {
                std::size_t e = acc(path + "[]");
                accs_[e].array_of = i;
                accs_[i].elements = e;
                element(child, e, depth);
            }
        }
    }

    // An element of an array: its type goes to "x[]", its fields to "x[].f". A nested array stays
    // as the element type "arr".
    void element(bson_iter_t& it, std::size_t i, int depth) {
        bson_type_t t = bson_iter_type(&it);
        accs_[i].types.insert(short_type(t));
        count_value(accs_[i], it, t);
        bson_iter_t child;
        if (t == BSON_TYPE_DOCUMENT && depth < kMaxDepth && bson_iter_recurse(&it, &child))
            children(child, accs_[i].path + ".", depth + 1);
    }

    static void count_value(Acc& a, const bson_iter_t& it, bson_type_t t) {
        if (!a.values_ok || t == BSON_TYPE_NULL) return;
        bool ok = false;
        if (t == BSON_TYPE_UTF8) {
            uint32_t len = 0;
            const char* s = bson_iter_utf8(&it, &len);
            std::string_view v(s, len);
            ok = len <= kMaxValueBytes && plain_text(v) && !looks_like_email(v) &&
                 !looks_like_phone(v);
            if (ok) {
                ++a.counts[std::string(v)];
                ok = a.counts.size() <= kMaxValues;
            }
        }
        if (!ok) {
            a.values_ok = false;
            a.counts.clear();
        }
    }

    std::size_t ndocs_;
    std::size_t doc_ = 0;
    std::vector<Acc> accs_;
    std::unordered_map<std::string, std::size_t> index_;
};

}  // namespace

Collection infer(std::string_view name, const std::vector<const bson_t*>& docs) {
    Inferrer inf(docs.size());
    for (std::size_t i = 0; i < docs.size(); ++i) inf.document(docs[i], i);
    return inf.finish(name);
}

void upsert(Schema& schema, Collection collection) {
    auto& cs = schema.collections;
    auto it = std::lower_bound(cs.begin(), cs.end(), collection.name,
                               [](const Collection& c, const std::string& n) { return c.name < n; });
    if (it != cs.end() && it->name == collection.name)
        *it = std::move(collection);
    else
        cs.insert(it, std::move(collection));
}

}  // namespace askmdb::schema
