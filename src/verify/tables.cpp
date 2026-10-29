// Whitelists of the verifier (MongoDB >= 5.0). Whatever is not here is blocked.
//
// Adding a name: first the case that motivates it in tests/verify/*.jsonl, then the table.

#include <algorithm>
#include <iterator>

#include "verify/internal.hpp"

namespace askmdb::verify {

namespace {

template <std::size_t N>
bool in(const std::string_view (&table)[N], std::string_view name) {
    return std::find(std::begin(table), std::end(table), name) != std::end(table);
}

constexpr std::string_view kReadStages[] = {
    "$match",  "$project",   "$addFields",   "$set",         "$unset",      "$group",
    "$sort",   "$limit",     "$skip",        "$count",       "$unwind",     "$sortByCount",
    "$bucket", "$bucketAuto", "$facet",      "$replaceRoot", "$replaceWith", "$redact",
    "$setWindowFields", "$densify", "$fill",
};

constexpr std::string_view kWarnStages[] = {"$lookup", "$graphLookup", "$unionWith", "$sample"};

constexpr std::string_view kWriteStages[] = {"$out", "$merge"};

constexpr std::string_view kServerStages[] = {
    "$documents",           "$currentOp",       "$listSessions",
    "$listLocalSessions",   "$changeStream",    "$changeStreamSplitLargeEvent",
    "$collStats",           "$indexStats",      "$planCacheStats",
    "$listCatalog",         "$listSampledQueries", "$querySettings",
    "$shardedDataDistribution", "$listSearchIndexes",
};

constexpr std::string_view kUnsupportedStages[] = {"$search", "$searchMeta", "$vectorSearch", "$geoNear"};

constexpr std::string_view kJsOperators[] = {"$where", "$function", "$accumulator"};

constexpr std::string_view kQueryLogical[] = {"$and", "$or", "$nor", "$expr", "$text", "$comment", "$sampleRate"};

constexpr std::string_view kQueryOperators[] = {
    "$eq",           "$ne",          "$gt",          "$gte",         "$lt",        "$lte",
    "$in",           "$nin",         "$not",         "$exists",      "$type",      "$size",
    "$mod",          "$regex",       "$options",     "$all",         "$elemMatch", "$bitsAllClear",
    "$bitsAllSet",   "$bitsAnyClear", "$bitsAnySet", "$geoWithin",   "$geoIntersects",
};

constexpr std::string_view kExpressionOperators[] = {
    "$abs", "$acos", "$acosh", "$add", "$allElementsTrue", "$and", "$anyElementTrue",
    "$arrayElemAt", "$arrayToObject", "$asin", "$asinh", "$atan", "$atan2", "$atanh", "$avg",
    "$binarySize", "$bitAnd", "$bitNot", "$bitOr", "$bitXor", "$bsonSize", "$ceil", "$cmp",
    "$concat", "$concatArrays", "$cond", "$convert", "$cos", "$cosh", "$dateAdd", "$dateDiff",
    "$dateFromParts", "$dateFromString", "$dateSubtract", "$dateToParts", "$dateToString",
    "$dateTrunc", "$dayOfMonth", "$dayOfWeek", "$dayOfYear", "$degreesToRadians", "$divide", "$eq",
    "$exp", "$filter", "$first", "$firstN", "$floor", "$getField", "$gt", "$gte", "$hour",
    "$ifNull", "$in", "$indexOfArray", "$indexOfBytes", "$indexOfCP", "$isArray", "$isNumber",
    "$isoDayOfWeek", "$isoWeek", "$isoWeekYear", "$last", "$lastN", "$let", "$literal", "$ln",
    "$log", "$log10", "$lt", "$lte", "$ltrim", "$map", "$max", "$maxN", "$median",
    "$mergeObjects", "$meta", "$millisecond", "$min", "$minN", "$minute", "$mod", "$month",
    "$multiply", "$ne", "$not", "$objectToArray", "$or", "$percentile", "$pow",
    "$radiansToDegrees", "$rand", "$range", "$reduce", "$regexFind", "$regexFindAll",
    "$regexMatch", "$replaceAll", "$replaceOne", "$reverseArray", "$round", "$rtrim",
    "$sampleRate", "$second", "$setDifference", "$setEquals", "$setField", "$setIntersection",
    "$setIsSubset", "$setUnion", "$sin", "$sinh", "$size", "$slice", "$sortArray", "$split",
    "$sqrt", "$stdDevPop", "$stdDevSamp", "$strcasecmp", "$strLenBytes", "$strLenCP", "$substr",
    "$substrBytes", "$substrCP", "$subtract", "$sum", "$switch", "$tan", "$tanh", "$toBool",
    "$toDate", "$toDecimal", "$toDouble", "$toHashedIndexKey", "$toInt", "$toLong", "$toLower",
    "$toObjectId", "$toString", "$toUpper", "$toUUID", "$trim", "$trunc", "$tsIncrement",
    "$tsSecond", "$type", "$unsetField", "$week", "$year", "$zip",
};

constexpr std::string_view kAccumulators[] = {
    "$addToSet", "$avg",    "$bottom", "$bottomN", "$count", "$first",  "$firstN",
    "$last",     "$lastN",  "$max",    "$maxN",    "$median", "$mergeObjects", "$min",
    "$minN",     "$percentile", "$push", "$stdDevPop", "$stdDevSamp", "$sum", "$top", "$topN",
};

constexpr std::string_view kWindowOnly[] = {
    "$covariancePop", "$covarianceSamp", "$denseRank", "$derivative", "$documentNumber",
    "$expMovingAvg",  "$integral",       "$linearFill", "$locf",      "$rank", "$shift",
};

constexpr std::string_view kSystemVariables[] = {"NOW", "ROOT", "CURRENT", "REMOVE", "DESCEND", "PRUNE", "KEEP"};

}  // namespace

namespace detail {

StageKind stage_kind(std::string_view name) {
    if (in(kReadStages, name)) return StageKind::Read;
    if (in(kWarnStages, name)) return StageKind::Warn;
    if (in(kWriteStages, name)) return StageKind::Write;
    if (in(kServerStages, name)) return StageKind::Server;
    if (in(kUnsupportedStages, name)) return StageKind::Unsupported;
    return StageKind::Unknown;
}

bool is_js_operator(std::string_view name) { return in(kJsOperators, name); }
bool is_query_logical(std::string_view name) { return in(kQueryLogical, name); }
bool is_query_operator(std::string_view name) { return in(kQueryOperators, name); }
bool is_expression_operator(std::string_view name) { return in(kExpressionOperators, name); }
bool is_accumulator(std::string_view name) { return in(kAccumulators, name); }
bool is_window_operator(std::string_view name) {
    // $mergeObjects is a $group accumulator but not a window function.
    return name != "$mergeObjects" && (in(kAccumulators, name) || in(kWindowOnly, name));
}
bool is_system_variable(std::string_view name) { return in(kSystemVariables, name); }

}  // namespace detail

std::vector<std::string_view> blocked_stage_names() {
    std::vector<std::string_view> v;
    for (auto s : kWriteStages) v.push_back(s);
    for (auto s : kServerStages) v.push_back(s);
    for (auto s : kUnsupportedStages) v.push_back(s);
    return v;
}

std::vector<std::string_view> js_operator_names() {
    std::vector<std::string_view> v;
    for (auto s : kJsOperators) v.push_back(s);
    return v;
}

}  // namespace askmdb::verify
