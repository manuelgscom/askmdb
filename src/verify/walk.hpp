#pragma once
// The walker behind verify(): collects findings over one pipeline.

#include <bson/bson.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "verify/internal.hpp"

namespace askmdb::verify::detail {

// What the documents flowing at one point of a pipeline are known to contain.
struct Scope {
    std::vector<const schema::Collection*> collections;  // whose schema paths exist
    std::vector<std::string> created;                    // fields created by earlier stages
    bool unknown = false;  // a source collection is not in the schema: fields are not checked
    bool opaque = false;   // after $replaceRoot/$replaceWith: fields are not checked
};

using Vars = std::vector<std::string>;  // user variables in scope (without "$$")

class Walker {
public:
    Walker(const schema::Schema& schema, std::string_view db);

    void add(std::string_view id, std::string_view detail = {});
    void add(Reason reason);
    Assessment result() const;

    Scope scope_for(std::string_view collection);
    void scan(const bson_t* pipeline);
    void pipeline(const bson_t* pipeline, Scope& sc);

private:
    struct Finding {
        Level level;
        Reason reason;
    };

    const schema::Schema& schema_;
    bool same_db_;
    std::vector<std::pair<const schema::Collection*, std::vector<std::string>>> normalized_;
    std::vector<Finding> findings_;

    void field(std::string_view path, const Scope& sc);
    void check_text(std::string_view s);
    void check_regex(std::string_view pattern, std::string_view options);
    void scan_children(bson_iter_t it, int depth);

    void dollar_key(std::string_view key);
    void syntax(std::string_view where);
    bool only(const bson_iter_t& v, std::initializer_list<std::string_view> allowed, std::string_view where);

    void stages(bson_iter_t it, Scope& sc, const Vars& vars);
    void subpipeline(const bson_iter_t& v, Scope& sc, const Vars& vars, std::string_view where);
    void stage(std::string_view name, const bson_iter_t& v, Scope& sc, const Vars& vars);
    static void reset(Scope& sc, std::vector<std::string> fields);
    void projection(const bson_iter_t& v, const std::string& prefix, const Scope& sc, const Vars& vars,
                    std::vector<std::string>& added);
    void sort_spec(const bson_iter_t& v, const Scope& sc, const Vars& vars);
    void unwind(const bson_iter_t& v, Scope& sc);
    void bucket(std::string_view name, const bson_iter_t& v, Scope& sc, const Vars& vars);
    void facet(const bson_iter_t& v, Scope& sc, const Vars& vars);
    void window_fields(const bson_iter_t& v, Scope& sc, const Vars& vars);
    void densify(const bson_iter_t& v, const Scope& sc);
    void fill(const bson_iter_t& v, Scope& sc, const Vars& vars);
    Scope target(const bson_iter_t* from);
    void lookup(const bson_iter_t& v, Scope& sc, const Vars& vars);
    void graph_lookup(const bson_iter_t& v, Scope& sc, const Vars& vars);
    void union_with(const bson_iter_t& v, Scope& sc, const Vars& vars);

    void query(const bson_iter_t& v, const std::string& prefix, const Scope& sc, const Vars& vars);
    void conditions(const bson_iter_t& v, const std::string& path, const Scope& sc, const Vars& vars);

    void variable(std::string_view name_path, const Scope& sc, const Vars& vars);
    void getfield(const bson_iter_t& arg, const Scope& sc);
    void expr(const bson_iter_t& v, const Scope& sc, const Vars& vars);
    void op(std::string_view name, const bson_iter_t& arg, const Scope& sc, const Vars& vars);
    void accumulator(const bson_iter_t& v, const Scope& sc, const Vars& vars, bool window);
    void literal(const bson_iter_t& v);
    void literal_values(const bson_iter_t& v);
};

}  // namespace askmdb::verify::detail
