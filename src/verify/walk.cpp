// Recursive walk over the BSON of the pipeline.
//
// Two passes over the same document:
//   scan()   every value, whatever its place: duplicate keys, depth, BSON types, strings, regexes,
//            and a net for the names that are never fine anywhere (JavaScript, $out, $merge).
//   stages() the meaning: stages, operators by context, variables in scope and field paths.

#include "verify/walk.hpp"

#include <algorithm>
#include <initializer_list>

namespace askmdb::verify::detail {

namespace {

std::string_view key_of(const bson_iter_t& it) { return {bson_iter_key(&it), bson_iter_key_len(&it)}; }

std::string_view utf8_of(const bson_iter_t& it) {
    std::uint32_t len = 0;
    const char* s = bson_iter_utf8(&it, &len);
    return {s, len};
}

bool dollar(std::string_view s) { return !s.empty() && s[0] == '$'; }

template <class F>
void each(const bson_iter_t& it, F&& f) {
    bson_iter_t c;
    if (!bson_iter_recurse(&it, &c)) return;
    while (bson_iter_next(&c)) f(c);
}

std::size_t count(const bson_iter_t& it) {
    std::size_t n = 0;
    each(it, [&](const bson_iter_t&) { ++n; });
    return n;
}

std::string_view first_key(const bson_iter_t& it) {
    bson_iter_t c;
    if (bson_iter_recurse(&it, &c) && bson_iter_next(&c)) return key_of(c);
    return {};
}

bool child(const bson_iter_t& it, std::string_view key, bson_iter_t& out) {
    bson_iter_t c;
    if (!bson_iter_recurse(&it, &c)) return false;
    while (bson_iter_next(&c)) {
        if (key_of(c) == key) {
            out = c;
            return true;
        }
    }
    return false;
}

bool is_doc(const bson_iter_t& it) { return BSON_ITER_HOLDS_DOCUMENT(&it); }
bool is_array(const bson_iter_t& it) { return BSON_ITER_HOLDS_ARRAY(&it); }
bool is_string(const bson_iter_t& it) { return BSON_ITER_HOLDS_UTF8(&it); }
bool is_number(const bson_iter_t& it) {
    return BSON_ITER_HOLDS_NUMBER(&it) || BSON_ITER_HOLDS_DECIMAL128(&it);
}

std::string_view type_name(bson_type_t t) {
    switch (t) {
        case BSON_TYPE_BINARY: return "binary";
        case BSON_TYPE_UNDEFINED: return "undefined";
        case BSON_TYPE_DBPOINTER: return "dbPointer";
        case BSON_TYPE_CODE: return "code";
        case BSON_TYPE_SYMBOL: return "symbol";
        case BSON_TYPE_CODEWSCOPE: return "codeWScope";
        case BSON_TYPE_TIMESTAMP: return "timestamp";
        case BSON_TYPE_MINKEY: return "minKey";
        case BSON_TYPE_MAXKEY: return "maxKey";
        default: return "other";
    }
}

bool allowed_type(bson_type_t t) {
    switch (t) {
        case BSON_TYPE_DOUBLE:
        case BSON_TYPE_UTF8:
        case BSON_TYPE_DOCUMENT:
        case BSON_TYPE_ARRAY:
        case BSON_TYPE_OID:
        case BSON_TYPE_BOOL:
        case BSON_TYPE_DATE_TIME:
        case BSON_TYPE_NULL:
        case BSON_TYPE_REGEX:
        case BSON_TYPE_INT32:
        case BSON_TYPE_INT64:
        case BSON_TYPE_DECIMAL128:
            return true;
        default:
            return false;
    }
}

std::string join(std::string_view prefix, std::string_view key) {
    return prefix.empty() ? std::string(key) : std::string(prefix) + "." + std::string(key);
}

bool lower_starts_with(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != prefix[i]) return false;
    }
    return true;
}

// "items[].sku" -> "items.sku"
std::string normalize(std::string_view path) {
    std::string out;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '[' && i + 1 < path.size() && path[i + 1] == ']') {
            ++i;
            continue;
        }
        out += path[i];
    }
    return out;
}

// User variables: a lowercase ASCII letter, then letters, digits or '_'.
bool valid_variable_name(std::string_view name) {
    if (name.empty() || name[0] < 'a' || name[0] > 'z') return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

bool covers(std::string_view known, std::string_view path) {
    // equal, or `path` is a prefix of the known path ("cliente" when "cliente.ciudad" exists)
    return known == path || (known.size() > path.size() && known.starts_with(path) && known[path.size()] == '.');
}

}  // namespace

Walker::Walker(const schema::Schema& schema, std::string_view db) : schema_(schema), same_db_(schema.db == db) {
    for (const auto& c : schema.collections) {
        std::vector<std::string> paths;
        for (const auto& p : c.paths) paths.push_back(normalize(p.path));
        normalized_.emplace_back(&c, std::move(paths));
    }
}

void Walker::add(std::string_view id, std::string_view detail) { add(make_reason(id, detail)); }

void Walker::add(Reason r) {
    Level l = level_of(r.id);
    findings_.push_back({l, std::move(r)});
}

Assessment Walker::result() const {
    Assessment a;
    for (const auto& f : findings_) a.level = std::max(a.level, f.level);
    for (const auto& f : findings_) {
        if (f.level != a.level) continue;
        bool seen = std::any_of(a.reasons.begin(), a.reasons.end(), [&](const Reason& r) { return r.id == f.reason.id; });
        if (!seen) a.reasons.push_back(f.reason);
    }
    return a;
}

// --- Collections and fields -------------------------------------------------------------------

Scope Walker::scope_for(std::string_view name) {
    Scope sc;
    sc.unknown = true;
    if (name.empty() || name.find('\0') != std::string_view::npos || name.find('$') != std::string_view::npos ||
        has_control(name) || !valid_utf8(name)) {
        add("B-BAD-COLLECTION", name);
        return sc;
    }
    if (lower_starts_with(name, "system.")) {
        add("B-SYSTEM-COLLECTION", name);
        return sc;
    }
    if (same_db_) {
        for (const auto& c : schema_.collections) {
            if (c.name == name) {
                sc.unknown = false;
                sc.collections.push_back(&c);
                return sc;
            }
        }
    }
    add("W-UNKNOWN-COLLECTION", name);
    return sc;
}

void Walker::field(std::string_view path, const Scope& sc) {
    if (sc.unknown || sc.opaque) return;
    for (const auto& c : sc.created) {
        if (covers(c, path)) return;
        if (path.size() > c.size() && path.starts_with(c) && path[c.size()] == '.') return;  // inside it
    }
    for (const auto* coll : sc.collections) {
        for (const auto& [owner, paths] : normalized_) {
            if (owner != coll) continue;
            for (const auto& p : paths) {
                if (covers(p, path)) return;
            }
        }
    }
    add("W-UNKNOWN-FIELD", path);
}

// --- Pass 1: every value ----------------------------------------------------------------------

void Walker::check_text(std::string_view s) {
    if (has_control(s)) add("B-CONTROL-CHAR");
    if (!valid_utf8(s)) add("B-UTF8");
}

void Walker::check_regex(std::string_view pattern, std::string_view options) {
    for (char c : options) {
        if (c != 'i' && c != 'm' && c != 's' && c != 'x') {
            add("B-REGEX-OPTIONS", options);
            break;
        }
    }
    if (options.find('x') != std::string_view::npos) add("W-REGEX-COMPLEX", options);
    if (pattern.size() > 128) add("W-REGEX-LONG");
    RegexInfo info = analyze_regex(pattern);
    if (info.bad_flags) add("B-REGEX-OPTIONS", pattern);
    if (info.complex) add("W-REGEX-COMPLEX", pattern);
    if (info.nested) add("W-REGEX-NESTED", pattern);
}

void Walker::scan(const bson_t* doc) {
    bson_iter_t it;
    if (bson_iter_init(&it, doc)) scan_children(it, 1);
}

void Walker::scan_children(bson_iter_t it, int depth) {
    std::vector<std::string_view> keys;
    while (bson_iter_next(&it)) {
        auto k = key_of(it);
        check_text(k);
        if (std::find(keys.begin(), keys.end(), k) != keys.end()) add("B-DUP-KEY", k);
        keys.push_back(k);
        if (is_js_operator(k)) add("B-JS", k);
        if (k == "$out" || k == "$merge") add("B-WRITE-STAGE", k);

        bson_type_t t = bson_iter_type(&it);
        if (!allowed_type(t)) {
            add("B-BSON-TYPE", type_name(t));
            continue;
        }
        if (t == BSON_TYPE_UTF8) {
            check_text(utf8_of(it));
        } else if (t == BSON_TYPE_REGEX) {
            const char* options = nullptr;
            const char* pattern = bson_iter_regex(&it, &options);
            std::string_view p = pattern ? pattern : "", o = options ? options : "";
            check_text(p);
            check_text(o);
            check_regex(p, o);
        } else if (t == BSON_TYPE_DOCUMENT || t == BSON_TYPE_ARRAY) {
            if (depth + 1 > kMaxNesting) {
                add("B-TOO-DEEP");
                continue;
            }
            bson_iter_t c;
            if (bson_iter_recurse(&it, &c)) scan_children(c, depth + 1);
        }
    }
}

// --- Pass 2: meaning ---------------------------------------------------------------------------

void Walker::dollar_key(std::string_view k) {
    if (is_js_operator(k))
        add("B-JS", k);
    else
        add("B-UNKNOWN-OPERATOR", k);
}

bool Walker::only(const bson_iter_t& v, std::initializer_list<std::string_view> allowed, std::string_view where) {
    bool ok = true;
    each(v, [&](const bson_iter_t& c) {
        auto k = key_of(c);
        if (std::find(allowed.begin(), allowed.end(), k) != allowed.end()) return;
        ok = false;
        if (dollar(k))
            dollar_key(k);
        else
            add("B-SYNTAX", std::string(where) + " " + std::string(k));
    });
    return ok;
}

void Walker::syntax(std::string_view where) { add("B-SYNTAX", where); }

void Walker::pipeline(const bson_t* doc, Scope& sc) {
    bson_iter_t it;
    if (bson_iter_init(&it, doc)) stages(it, sc, {});
}

void Walker::subpipeline(const bson_iter_t& v, Scope& sc, const Vars& vars, std::string_view where) {
    if (!is_array(v)) {
        syntax(where);
        return;
    }
    bson_iter_t c;
    if (bson_iter_recurse(&v, &c)) stages(c, sc, vars);
}

void Walker::stages(bson_iter_t it, Scope& sc, const Vars& vars) {
    int n = 0;
    while (bson_iter_next(&it)) {
        ++n;
        if (!is_doc(it) || count(it) != 1) add("B-STAGE-SHAPE");
        if (!is_doc(it)) continue;
        each(it, [&](const bson_iter_t& s) { stage(key_of(s), s, sc, vars); });
    }
    if (n > kMaxStages) add("B-TOO-MANY-STAGES");
}

void Walker::stage(std::string_view name, const bson_iter_t& v, Scope& sc, const Vars& vars) {
    if (is_js_operator(name)) {
        add("B-JS", name);
        return;
    }
    switch (stage_kind(name)) {
        case StageKind::Write: add("B-WRITE-STAGE", name); return;
        case StageKind::Server: add("B-SERVER-STAGE", name); return;
        case StageKind::Unsupported: add("B-UNSUPPORTED-STAGE", name); return;
        case StageKind::Unknown: add("B-UNKNOWN-STAGE", name); return;
        case StageKind::Read:
        case StageKind::Warn: break;
    }

    if (name == "$match") {
        if (is_doc(v)) query(v, "", sc, vars);
        else syntax(name);
    } else if (name == "$project") {
        if (!is_doc(v)) return syntax(name);
        std::vector<std::string> added;
        projection(v, "", sc, vars, added);
        sc.created.insert(sc.created.end(), added.begin(), added.end());
    } else if (name == "$addFields" || name == "$set") {
        if (!is_doc(v)) return syntax(name);
        std::vector<std::string> added;
        each(v, [&](const bson_iter_t& c) {
            auto k = key_of(c);
            if (dollar(k)) return dollar_key(k);
            expr(c, sc, vars);
            added.emplace_back(k);
        });
        sc.created.insert(sc.created.end(), added.begin(), added.end());
    } else if (name == "$unset") {
        if (is_string(v)) return;
        if (!is_array(v)) return syntax(name);
        each(v, [&](const bson_iter_t& c) {
            if (!is_string(c)) syntax(name);
        });
    } else if (name == "$group") {
        if (!is_doc(v)) return syntax(name);
        std::vector<std::string> out;
        each(v, [&](const bson_iter_t& c) {
            auto k = key_of(c);
            if (k == "_id") {
                expr(c, sc, vars);
            } else if (dollar(k)) {
                return dollar_key(k);
            } else {
                accumulator(c, sc, vars, false);
            }
            out.emplace_back(k);
        });
        reset(sc, std::move(out));
    } else if (name == "$sort") {
        sort_spec(v, sc, vars);
    } else if (name == "$limit" || name == "$skip") {
        if (!is_number(v)) syntax(name);
    } else if (name == "$count") {
        auto s = is_string(v) ? utf8_of(v) : std::string_view{};
        if (s.empty() || dollar(s) || s.find('.') != std::string_view::npos) return syntax(name);
        reset(sc, {std::string(s)});
    } else if (name == "$unwind") {
        unwind(v, sc);
    } else if (name == "$sortByCount") {
        expr(v, sc, vars);
        reset(sc, {"_id", "count"});
    } else if (name == "$bucket" || name == "$bucketAuto") {
        bucket(name, v, sc, vars);
    } else if (name == "$facet") {
        facet(v, sc, vars);
    } else if (name == "$replaceRoot") {
        bson_iter_t r;
        if (!is_doc(v) || !only(v, {"newRoot"}, name) || !child(v, "newRoot", r)) return syntax(name);
        expr(r, sc, vars);
        sc.opaque = true;
    } else if (name == "$replaceWith") {
        expr(v, sc, vars);
        sc.opaque = true;
    } else if (name == "$redact") {
        expr(v, sc, vars);
    } else if (name == "$setWindowFields") {
        window_fields(v, sc, vars);
    } else if (name == "$densify") {
        densify(v, sc);
    } else if (name == "$fill") {
        fill(v, sc, vars);
    } else if (name == "$sample") {
        add("W-SAMPLE", name);
        bson_iter_t s;
        if (!is_doc(v) || !only(v, {"size"}, name) || !child(v, "size", s) || !is_number(s)) syntax(name);
    } else if (name == "$lookup") {
        lookup(v, sc, vars);
    } else if (name == "$graphLookup") {
        graph_lookup(v, sc, vars);
    } else if (name == "$unionWith") {
        union_with(v, sc, vars);
    } else {
        syntax(name);  // in a table but without a handler: never read
    }
}

void Walker::reset(Scope& sc, std::vector<std::string> fields) {
    sc.collections.clear();
    sc.unknown = false;
    sc.opaque = false;
    sc.created = std::move(fields);
}

void Walker::projection(const bson_iter_t& v, const std::string& prefix, const Scope& sc, const Vars& vars,
                        std::vector<std::string>& added) {
    each(v, [&](const bson_iter_t& c) {
        auto k = key_of(c);
        if (dollar(k)) return dollar_key(k);
        std::string path = prefix + std::string(k);
        if (BSON_ITER_HOLDS_BOOL(&c) || BSON_ITER_HOLDS_NUMBER(&c)) {
            if (bson_iter_as_bool(&c)) added.push_back(path);
        } else if (BSON_ITER_HOLDS_DECIMAL128(&c)) {
            added.push_back(path);
        } else if (is_doc(c) && !dollar(first_key(c))) {
            added.push_back(path);
            projection(c, path + ".", sc, vars, added);
        } else {
            expr(c, sc, vars);
            added.push_back(path);
        }
    });
}

void Walker::sort_spec(const bson_iter_t& v, const Scope& sc, const Vars& vars) {
    if (!is_doc(v)) return syntax("$sort");
    each(v, [&](const bson_iter_t& c) {
        auto k = key_of(c);
        if (dollar(k)) return dollar_key(k);
        field(k, sc);
        if (is_doc(c))
            expr(c, sc, vars);  // {$meta: ...}
        else if (!is_number(c))
            syntax("$sort");
    });
}

void Walker::unwind(const bson_iter_t& v, Scope& sc) {
    auto path_ok = [&](const bson_iter_t& p) {
        if (!is_string(p)) return false;
        auto s = utf8_of(p);
        if (!dollar(s) || s.starts_with("$$")) return false;
        field(s.substr(1), sc);
        return true;
    };
    if (is_string(v)) {
        if (!path_ok(v)) syntax("$unwind");
        return;
    }
    bson_iter_t p, idx, keep;
    if (!is_doc(v) || !only(v, {"path", "includeArrayIndex", "preserveNullAndEmptyArrays"}, "$unwind") ||
        !child(v, "path", p) || !path_ok(p))
        return syntax("$unwind");
    if (child(v, "includeArrayIndex", idx)) {
        if (!is_string(idx) || dollar(utf8_of(idx))) return syntax("$unwind");
        sc.created.emplace_back(utf8_of(idx));
    }
    if (child(v, "preserveNullAndEmptyArrays", keep) && !BSON_ITER_HOLDS_BOOL(&keep)) syntax("$unwind");
}

void Walker::bucket(std::string_view name, const bson_iter_t& v, Scope& sc, const Vars& vars) {
    if (!is_doc(v)) return syntax(name);
    bool auto_ = name == "$bucketAuto";
    if (auto_)
        only(v, {"groupBy", "buckets", "output", "granularity"}, name);
    else
        only(v, {"groupBy", "boundaries", "default", "output"}, name);
    std::vector<std::string> out{"_id", "count"};
    each(v, [&](const bson_iter_t& c) {
        auto k = key_of(c);
        if (k == "groupBy") {
            expr(c, sc, vars);
        } else if (k == "output") {
            if (!is_doc(c)) return syntax(name);
            each(c, [&](const bson_iter_t& o) {
                auto ok = key_of(o);
                if (dollar(ok)) return dollar_key(ok);
                accumulator(o, sc, vars, false);
                out.emplace_back(ok);
            });
        } else if (k == "boundaries" || k == "default" || k == "buckets" || k == "granularity") {
            literal(c);
        }
    });
    reset(sc, std::move(out));
}

void Walker::facet(const bson_iter_t& v, Scope& sc, const Vars& vars) {
    if (!is_doc(v)) return syntax("$facet");
    if (count(v) > static_cast<std::size_t>(kMaxStages)) add("B-TOO-MANY-STAGES", "$facet");
    std::vector<std::string> out;
    each(v, [&](const bson_iter_t& c) {
        auto k = key_of(c);
        if (dollar(k)) return dollar_key(k);
        Scope inner = sc;
        subpipeline(c, inner, vars, "$facet");
        out.emplace_back(k);
    });
    reset(sc, std::move(out));
}

void Walker::window_fields(const bson_iter_t& v, Scope& sc, const Vars& vars) {
    if (!is_doc(v)) return syntax("$setWindowFields");
    only(v, {"partitionBy", "sortBy", "output"}, "$setWindowFields");
    std::vector<std::string> added;
    each(v, [&](const bson_iter_t& c) {
        auto k = key_of(c);
        if (k == "partitionBy") {
            expr(c, sc, vars);
        } else if (k == "sortBy") {
            sort_spec(c, sc, vars);
        } else if (k == "output") {
            if (!is_doc(c)) return syntax("$setWindowFields");
            each(c, [&](const bson_iter_t& o) {
                auto ok = key_of(o);
                if (dollar(ok)) return dollar_key(ok);
                accumulator(o, sc, vars, true);
                added.emplace_back(ok);
            });
        }
    });
    sc.created.insert(sc.created.end(), added.begin(), added.end());
}

void Walker::densify(const bson_iter_t& v, const Scope& sc) {
    if (!is_doc(v)) return syntax("$densify");
    only(v, {"field", "partitionByFields", "range"}, "$densify");
    each(v, [&](const bson_iter_t& c) {
        auto k = key_of(c);
        if (k == "field") {
            if (!is_string(c)) return syntax("$densify");
            field(utf8_of(c), sc);
        } else if (k == "partitionByFields") {
            if (!is_array(c)) return syntax("$densify");
            each(c, [&](const bson_iter_t& f) {
                if (!is_string(f)) return syntax("$densify");
                field(utf8_of(f), sc);
            });
        } else if (k == "range") {
            literal(c);
        }
    });
}

void Walker::fill(const bson_iter_t& v, Scope& sc, const Vars& vars) {
    if (!is_doc(v)) return syntax("$fill");
    only(v, {"partitionBy", "partitionByFields", "sortBy", "output"}, "$fill");
    std::vector<std::string> added;
    each(v, [&](const bson_iter_t& c) {
        auto k = key_of(c);
        if (k == "partitionBy") {
            expr(c, sc, vars);
        } else if (k == "partitionByFields") {
            if (!is_array(c)) return syntax("$fill");
            each(c, [&](const bson_iter_t& f) {
                if (!is_string(f)) return syntax("$fill");
                field(utf8_of(f), sc);
            });
        } else if (k == "sortBy") {
            sort_spec(c, sc, vars);
        } else if (k == "output") {
            if (!is_doc(c)) return syntax("$fill");
            each(c, [&](const bson_iter_t& o) {
                auto ok = key_of(o);
                if (dollar(ok)) return dollar_key(ok);
                if (!is_doc(o) || !only(o, {"value", "method"}, "$fill")) return syntax("$fill");
                each(o, [&](const bson_iter_t& m) {
                    if (key_of(m) == "value")
                        expr(m, sc, vars);
                    else
                        literal(m);
                });
                added.emplace_back(ok);
            });
        }
    });
    sc.created.insert(sc.created.end(), added.begin(), added.end());
}

// from/coll: a string naming a collection of the same database.
Scope Walker::target(const bson_iter_t* v) {
    if (!v || !is_string(*v)) {
        add("B-JOIN-TARGET");
        Scope sc;
        sc.unknown = true;
        return sc;
    }
    return scope_for(utf8_of(*v));
}

void Walker::lookup(const bson_iter_t& v, Scope& sc, const Vars& vars) {
    add("W-JOIN", "$lookup");
    if (!is_doc(v)) return syntax("$lookup");
    only(v, {"from", "localField", "foreignField", "as", "let", "pipeline"}, "$lookup");
    bson_iter_t f;
    Scope foreign = target(child(v, "from", f) ? &f : nullptr);
    Vars inner = vars;
    bson_iter_t c;
    if (child(v, "let", c)) {
        if (!is_doc(c)) {
            syntax("$lookup let");
        } else {
            each(c, [&](const bson_iter_t& l) {
                auto k = key_of(l);
                if (dollar(k)) return dollar_key(k);
                if (!valid_variable_name(k)) add("B-BAD-VARIABLE", k);
                expr(l, sc, vars);
                inner.emplace_back(k);
            });
        }
    }
    if (child(v, "localField", c)) {
        if (is_string(c)) field(utf8_of(c), sc);
        else syntax("$lookup localField");
    }
    if (child(v, "foreignField", c)) {
        if (is_string(c)) field(utf8_of(c), foreign);
        else syntax("$lookup foreignField");
    }
    if (child(v, "pipeline", c)) {
        Scope sub = foreign;
        subpipeline(c, sub, inner, "$lookup pipeline");
    }
    if (!child(v, "as", c) || !is_string(c)) return syntax("$lookup as");
    sc.created.emplace_back(utf8_of(c));
}

void Walker::graph_lookup(const bson_iter_t& v, Scope& sc, const Vars& vars) {
    add("W-JOIN", "$graphLookup");
    if (!is_doc(v)) return syntax("$graphLookup");
    only(v, {"from", "startWith", "connectFromField", "connectToField", "as", "maxDepth", "depthField",
             "restrictSearchWithMatch"},
         "$graphLookup");
    bson_iter_t f;
    Scope foreign = target(child(v, "from", f) ? &f : nullptr);
    std::string as;
    each(v, [&](const bson_iter_t& c) {
        auto k = key_of(c);
        if (k == "startWith") {
            expr(c, sc, vars);
        } else if (k == "connectFromField" || k == "connectToField") {
            if (is_string(c)) field(utf8_of(c), foreign);
            else syntax("$graphLookup");
        } else if (k == "restrictSearchWithMatch") {
            if (is_doc(c)) query(c, "", foreign, vars);
            else syntax("$graphLookup");
        } else if (k == "maxDepth") {
            if (!is_number(c)) syntax("$graphLookup");
        } else if (k == "as" || k == "depthField") {
            if (!is_string(c)) return syntax("$graphLookup");
            if (k == "as") as = std::string(utf8_of(c));
        }
    });
    if (as.empty()) return syntax("$graphLookup as");
    sc.created.push_back(as);
}

void Walker::union_with(const bson_iter_t& v, Scope& sc, const Vars& vars) {
    add("W-JOIN", "$unionWith");
    Scope foreign;
    if (is_string(v)) {
        foreign = target(&v);
    } else if (is_doc(v)) {
        only(v, {"coll", "pipeline"}, "$unionWith");
        bson_iter_t c;
        foreign = target(child(v, "coll", c) ? &c : nullptr);
        if (child(v, "pipeline", c)) subpipeline(c, foreign, vars, "$unionWith pipeline");
    } else {
        add("B-JOIN-TARGET");
        return;
    }
    // From here on the documents of both collections flow.
    sc.unknown = sc.unknown || foreign.unknown;
    sc.opaque = sc.opaque || foreign.opaque;
    sc.collections.insert(sc.collections.end(), foreign.collections.begin(), foreign.collections.end());
    sc.created.insert(sc.created.end(), foreign.created.begin(), foreign.created.end());
}

// --- Queries ($match, $elemMatch, restrictSearchWithMatch) --------------------------------------

void Walker::query(const bson_iter_t& v, const std::string& prefix, const Scope& sc, const Vars& vars) {
    each(v, [&](const bson_iter_t& c) {
        auto k = key_of(c);
        if (!dollar(k)) {
            std::string path = join(prefix, k);
            field(path, sc);
            if (is_doc(c) && dollar(first_key(c)))
                conditions(c, path, sc, vars);
            else
                literal(c);
            return;
        }
        if (k == "$and" || k == "$or" || k == "$nor") {
            if (!is_array(c)) return syntax(k);
            each(c, [&](const bson_iter_t& q) {
                if (is_doc(q)) query(q, prefix, sc, vars);
                else syntax(k);
            });
        } else if (k == "$expr") {
            expr(c, sc, vars);
        } else if (k == "$text") {
            if (!is_doc(c) || !only(c, {"$search", "$language", "$caseSensitive", "$diacriticSensitive"}, k))
                return syntax(k);
            literal_values(c);
        } else if (k == "$comment") {
            literal(c);
        } else if (k == "$sampleRate") {
            add("W-RANDOM", k);
            literal(c);
        } else {
            dollar_key(k);
        }
    });
}

void Walker::conditions(const bson_iter_t& v, const std::string& path, const Scope& sc, const Vars& vars) {
    std::optional<std::string_view> pattern, options;
    each(v, [&](const bson_iter_t& c) {
        auto k = key_of(c);
        if (!dollar(k)) return syntax(path);
        if (!is_query_operator(k)) return dollar_key(k);
        if (k == "$regex") {
            if (is_string(c)) pattern = utf8_of(c);
            else if (!BSON_ITER_HOLDS_REGEX(&c)) syntax(k);
        } else if (k == "$options") {
            if (is_string(c)) options = utf8_of(c);
            else syntax(k);
        } else if (k == "$not") {
            if (is_doc(c)) conditions(c, path, sc, vars);
            else if (!BSON_ITER_HOLDS_REGEX(&c)) syntax(k);
        } else if (k == "$elemMatch") {
            if (!is_doc(c)) return syntax(k);
            bool ops = count(c) > 0;
            each(c, [&](const bson_iter_t& e) {
                auto ek = key_of(e);
                if (!dollar(ek) || is_query_logical(ek)) ops = false;
            });
            if (ops) conditions(c, path, sc, vars);
            else query(c, path, sc, vars);
        } else if (k == "$all") {
            if (!is_array(c)) return literal(c);
            each(c, [&](const bson_iter_t& e) {
                if (is_doc(e) && first_key(e) == "$elemMatch") conditions(e, path, sc, vars);
                else literal(e);
            });
        } else if (k == "$geoWithin" || k == "$geoIntersects") {
            if (!is_doc(c)) return syntax(k);
            each(c, [&](const bson_iter_t& g) {
                auto gk = key_of(g);
                if (gk == "$geometry" || gk == "$box" || gk == "$polygon" || gk == "$center" || gk == "$centerSphere")
                    literal(g);
                else if (dollar(gk))
                    dollar_key(gk);
                else
                    syntax(k);
            });
        } else {
            literal(c);
        }
    });
    if (pattern) check_regex(*pattern, options.value_or(""));
    else if (options) check_regex("", *options);
}

// --- Expressions -------------------------------------------------------------------------------

void Walker::variable(std::string_view name_path, const Scope& sc, const Vars& vars) {
    auto dot = name_path.find('.');
    auto name = name_path.substr(0, dot);
    if (is_system_variable(name)) {
        if ((name == "ROOT" || name == "CURRENT") && dot != std::string_view::npos) field(name_path.substr(dot + 1), sc);
        return;
    }
    if (std::find(vars.begin(), vars.end(), name) != vars.end()) return;
    add("B-UNKNOWN-VARIABLE", "$$" + std::string(name));
}

void Walker::expr(const bson_iter_t& v, const Scope& sc, const Vars& vars) {
    if (is_string(v)) {
        auto s = utf8_of(v);
        if (s.starts_with("$$"))
            variable(s.substr(2), sc, vars);
        else if (dollar(s))
            field(s.substr(1), sc);
    } else if (is_array(v)) {
        each(v, [&](const bson_iter_t& c) { expr(c, sc, vars); });
    } else if (is_doc(v)) {
        if (dollar(first_key(v))) {
            if (count(v) != 1) syntax(first_key(v));
            each(v, [&](const bson_iter_t& c) {
                auto k = key_of(c);
                if (dollar(k)) op(k, c, sc, vars);
                else syntax(k);
            });
        } else {
            each(v, [&](const bson_iter_t& c) {
                auto k = key_of(c);
                if (dollar(k)) dollar_key(k);
                else expr(c, sc, vars);
            });
        }
    }
}

void Walker::op(std::string_view name, const bson_iter_t& arg, const Scope& sc, const Vars& vars) {
    if (is_js_operator(name) || !is_expression_operator(name)) return dollar_key(name);
    if (name == "$rand" || name == "$sampleRate") add("W-RANDOM", name);

    if (name == "$getField") {
        getfield(arg, sc);
        expr(arg, sc, vars);
    } else if (name == "$literal") {
        literal(arg);
    } else if (name == "$let") {
        bson_iter_t defs, in;
        if (!is_doc(arg) || !only(arg, {"vars", "in"}, name) || !child(arg, "vars", defs) || !is_doc(defs) ||
            !child(arg, "in", in))
            return syntax(name);
        Vars inner = vars;
        each(defs, [&](const bson_iter_t& d) {
            auto k = key_of(d);
            if (dollar(k)) return dollar_key(k);
            if (!valid_variable_name(k)) add("B-BAD-VARIABLE", k);
            expr(d, sc, vars);
            inner.emplace_back(k);
        });
        expr(in, sc, inner);
    } else if (name == "$map" || name == "$filter" || name == "$reduce") {
        if (!is_doc(arg)) return syntax(name);
        if (name == "$map") only(arg, {"input", "as", "in"}, name);
        else if (name == "$filter") only(arg, {"input", "as", "cond", "limit"}, name);
        else only(arg, {"input", "initialValue", "in"}, name);
        Vars inner = vars;
        bson_iter_t as;
        if (name == "$reduce") {
            inner.emplace_back("this");
            inner.emplace_back("value");
        } else if (child(arg, "as", as)) {
            if (!is_string(as)) return syntax(name);
            if (!valid_variable_name(utf8_of(as))) add("B-BAD-VARIABLE", utf8_of(as));
            inner.emplace_back(utf8_of(as));
        } else {
            inner.emplace_back("this");
        }
        each(arg, [&](const bson_iter_t& c) {
            auto k = key_of(c);
            if (k == "in" || k == "cond") expr(c, sc, inner);
            else if (k != "as") expr(c, sc, vars);
        });
    } else if (name == "$regexMatch" || name == "$regexFind" || name == "$regexFindAll") {
        if (!is_doc(arg)) return syntax(name);
        only(arg, {"input", "regex", "options"}, name);
        std::optional<std::string_view> pattern, options;
        each(arg, [&](const bson_iter_t& c) {
            auto k = key_of(c);
            bool literal_string = is_string(c) && !dollar(utf8_of(c));
            if (k == "regex") {
                if (literal_string) pattern = utf8_of(c);
                else if (!BSON_ITER_HOLDS_REGEX(&c)) add("W-REGEX-DYNAMIC", name);
            } else if (k == "options") {
                if (literal_string) options = utf8_of(c);
                else add("B-REGEX-OPTIONS", name);
            }
            if (!literal_string) expr(c, sc, vars);
        });
        if (pattern) check_regex(*pattern, options.value_or(""));
        else if (options) check_regex("", *options);
    } else {
        expr(arg, sc, vars);
    }
}

// $getField of a literal name over the current document is a field reference.
void Walker::getfield(const bson_iter_t& arg, const Scope& sc) {
    if (is_string(arg)) {
        if (!dollar(utf8_of(arg))) field(utf8_of(arg), sc);
        return;
    }
    bson_iter_t f, in;
    if (!is_doc(arg) || !child(arg, "field", f) || !is_string(f) || dollar(utf8_of(f))) return;
    if (child(arg, "input", in) &&
        !(is_string(in) && (utf8_of(in) == "$$CURRENT" || utf8_of(in) == "$$ROOT")))
        return;
    field(utf8_of(f), sc);
}

void Walker::accumulator(const bson_iter_t& v, const Scope& sc, const Vars& vars, bool window) {
    if (!is_doc(v)) return syntax("accumulator");
    std::size_t ops = 0;
    each(v, [&](const bson_iter_t& c) {
        auto k = key_of(c);
        if (window && k == "window") {
            if (is_doc(c)) literal(c);
            else syntax(k);
            return;
        }
        if (!dollar(k)) return syntax(k);
        ++ops;
        bool known = window ? is_window_operator(k) : is_accumulator(k);
        if (is_js_operator(k) || !known) return dollar_key(k);
        expr(c, sc, vars);
    });
    if (ops != 1) syntax("accumulator");
}

// Data, not expressions: strings are not field paths and no key may start with '$'.
void Walker::literal(const bson_iter_t& v) {
    if (!is_doc(v) && !is_array(v)) return;
    bool doc = is_doc(v);
    each(v, [&](const bson_iter_t& c) {
        if (doc && dollar(key_of(c))) dollar_key(key_of(c));
        literal(c);
    });
}

void Walker::literal_values(const bson_iter_t& v) {
    each(v, [&](const bson_iter_t& c) { literal(c); });
}

}  // namespace askmdb::verify::detail
