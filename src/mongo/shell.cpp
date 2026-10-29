// mongosh syntax of a pipeline: what is shown and what is copied.
//
// Keys are unquoted when they are identifiers ($match, total), quoted otherwise ("cliente.ciudad").
// Doubles keep a decimal point when integral (1.0), so a reader sees that the value is a double;
// int32 is written plain and int64 as NumberLong("…"). Regex options outside i, m, s and u (not
// valid in a JavaScript literal) use BSONRegExp("…", "…").
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "exec/base64.hpp"
#include "mongo/format.hpp"
#include "mongo/mongo.hpp"

namespace askmdb::mongo {

namespace {

bool is_identifier(std::string_view s, bool allow_dollar) {
    if (s.empty()) return false;
    auto ok_first = [&](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || (allow_dollar && c == '$');
    };
    if (!ok_first(s.front())) return false;
    for (char c : s.substr(1)) {
        if (!ok_first(c) && !(c >= '0' && c <= '9')) return false;
    }
    return true;
}

// Properties of mongosh's db object (and of any JavaScript object) that would shadow a collection.
const std::set<std::string_view>& db_members() {
    static const std::set<std::string_view> members = {
        "adminCommand", "aggregate", "auth", "changeUserPassword", "checkMetadataConsistency",
        "cloneCollection", "cloneDatabase", "commandHelp", "constructor", "copyDatabase", "createCollection",
        "createEncryptedCollection", "createRole", "createUser", "createView", "currentOp", "dropAllRoles",
        "dropAllUsers", "dropDatabase", "dropRole", "dropUser", "fsyncLock", "fsyncUnlock", "getCollection",
        "getCollectionInfos", "getCollectionNames", "getLastError", "getLastErrorObj", "getLogComponents",
        "getMongo", "getName", "getProfilingLevel", "getProfilingStatus", "getQueryOptions",
        "getReplicationInfo", "getRole", "getRoles", "getSecondaryReplicationInfo", "getSiblingDB", "getUser",
        "getUsers", "grantPrivilegesToRole", "grantRolesToRole", "grantRolesToUser", "hasOwnProperty", "hello",
        "help", "hostInfo", "isMaster", "isPrototypeOf", "killOp", "listCommands", "logout",
        "printCollectionStats", "printReplicationInfo", "printSecondaryReplicationInfo", "printShardingStatus",
        "printSlaveReplicationInfo", "propertyIsEnumerable", "revokePrivilegesFromRole", "revokeRolesFromRole",
        "revokeRolesFromUser", "rotateCertificates", "runCommand", "serverBits", "serverBuildInfo",
        "serverCmdLineOpts", "serverStatus", "setLogLevel", "setProfilingLevel", "setQueryOptions",
        "setSecondaryOk", "shutdownServer", "sql", "stats", "toLocaleString", "toString", "updateRole",
        "updateUser", "valueOf", "version", "watch",
    };
    return members;
}

std::string format_double(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    char buf[64];
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof buf, "%.*g", precision, value);
        if (std::strtod(buf, nullptr) == value) break;
    }
    std::string out(buf);
    if (out.find_first_of(".e") == std::string::npos) out += ".0";
    return out;
}

std::string regex_body(std::string_view pattern) {
    if (pattern.empty()) return "(?:)";
    std::string out;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        auto c = static_cast<unsigned char>(pattern[i]);
        if (c == '\\' && i + 1 < pattern.size() && static_cast<unsigned char>(pattern[i + 1]) >= 0x20) {
            out += pattern[i];
            out += pattern[++i];
        } else if (c == '/') {
            out += "\\/";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c < 0x20 || c == 0x7F || c == '\\') {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02X", c);
            out += buf;
        } else if (c == 0xC2 && i + 1 < pattern.size() && static_cast<unsigned char>(pattern[i + 1]) >= 0x80 &&
                   static_cast<unsigned char>(pattern[i + 1]) <= 0x9F) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02X", static_cast<unsigned char>(pattern[++i]));
            out += buf;
        } else if (c == 0xE2 && pattern.substr(i, 3) == " ") {
            out += "\\u2028";
            i += 2;
        } else if (c == 0xE2 && pattern.substr(i, 3) == " ") {
            out += "\\u2029";
            i += 2;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

void append_value(std::string& out, const bson_iter_t& it);

void append_document(std::string& out, const bson_iter_t& it, bool array) {
    bson_iter_t child;
    if (!bson_iter_recurse(&it, &child)) {
        out += array ? "[]" : "{}";
        return;
    }
    std::string body;
    bool first = true;
    while (bson_iter_next(&child)) {
        if (!first) body += ", ";
        first = false;
        if (!array) {
            std::string_view key(bson_iter_key(&child), bson_iter_key_len(&child));
            body += is_identifier(key, true) ? std::string(key) : json_quote(key);
            body += ": ";
        }
        append_value(body, child);
    }
    if (array) {
        out += "[" + body + "]";
    } else {
        out += body.empty() ? "{}" : "{ " + body + " }";
    }
}

void append_value(std::string& out, const bson_iter_t& it) {
    char buf[64];
    switch (bson_iter_type(&it)) {
        case BSON_TYPE_DOCUMENT:
            append_document(out, it, false);
            break;
        case BSON_TYPE_ARRAY:
            append_document(out, it, true);
            break;
        case BSON_TYPE_UTF8: {
            std::uint32_t len = 0;
            const char* s = bson_iter_utf8(&it, &len);
            out += json_quote(std::string_view(s, len));
            break;
        }
        case BSON_TYPE_SYMBOL: {
            std::uint32_t len = 0;
            const char* s = bson_iter_symbol(&it, &len);
            out += json_quote(std::string_view(s, len));
            break;
        }
        case BSON_TYPE_INT32:
            out += std::to_string(bson_iter_int32(&it));
            break;
        case BSON_TYPE_INT64:
            out += "NumberLong(\"" + std::to_string(bson_iter_int64(&it)) + "\")";
            break;
        case BSON_TYPE_DOUBLE:
            out += format_double(bson_iter_double(&it));
            break;
        case BSON_TYPE_DECIMAL128: {
            bson_decimal128_t dec;
            bson_iter_decimal128(&it, &dec);
            char dbuf[BSON_DECIMAL128_STRING];
            bson_decimal128_to_string(&dec, dbuf);
            out += "NumberDecimal(\"" + std::string(dbuf) + "\")";
            break;
        }
        case BSON_TYPE_DATE_TIME:
            out += "ISODate(\"" + iso_date(bson_iter_date_time(&it)) + "\")";
            break;
        case BSON_TYPE_OID:
            bson_oid_to_string(bson_iter_oid(&it), buf);
            out += "ObjectId(\"" + std::string(buf) + "\")";
            break;
        case BSON_TYPE_REGEX: {
            const char* options = nullptr;
            const char* pattern = bson_iter_regex(&it, &options);
            std::string_view opts = options ? options : "";
            if (opts.find_first_not_of("imsu") == std::string_view::npos) {
                out += "/" + regex_body(pattern) + "/" + std::string(opts);
            } else {
                out += "BSONRegExp(" + json_quote(pattern) + ", " + json_quote(opts) + ")";
            }
            break;
        }
        case BSON_TYPE_BOOL:
            out += bson_iter_bool(&it) ? "true" : "false";
            break;
        case BSON_TYPE_NULL:
            out += "null";
            break;
        case BSON_TYPE_UNDEFINED:
            out += "undefined";
            break;
        case BSON_TYPE_MINKEY:
            out += "MinKey()";
            break;
        case BSON_TYPE_MAXKEY:
            out += "MaxKey()";
            break;
        case BSON_TYPE_TIMESTAMP: {
            std::uint32_t t = 0;
            std::uint32_t i = 0;
            bson_iter_timestamp(&it, &t, &i);
            out += "Timestamp({ t: " + std::to_string(t) + ", i: " + std::to_string(i) + " })";
            break;
        }
        case BSON_TYPE_BINARY: {
            bson_subtype_t subtype;
            std::uint32_t len = 0;
            const std::uint8_t* data = nullptr;
            bson_iter_binary(&it, &subtype, &len, &data);
            std::string_view bytes(reinterpret_cast<const char*>(data), len);
            out += "BinData(" + std::to_string(static_cast<int>(subtype)) + ", \"" + exec::base64_encode(bytes) +
                   "\")";
            break;
        }
        case BSON_TYPE_CODE: {
            std::uint32_t len = 0;
            const char* code = bson_iter_code(&it, &len);
            out += "Code(" + json_quote(std::string_view(code, len)) + ")";
            break;
        }
        case BSON_TYPE_CODEWSCOPE: {
            std::uint32_t len = 0;
            std::uint32_t scope_len = 0;
            const std::uint8_t* scope_data = nullptr;
            const char* code = bson_iter_codewscope(&it, &len, &scope_len, &scope_data);
            out += "Code(" + json_quote(std::string_view(code, len));
            bson_t scope;
            bson_iter_t scope_it;
            if (bson_init_static(&scope, scope_data, scope_len)) {
                bson_t wrap;
                bson_init(&wrap);
                bson_append_document(&wrap, "s", 1, &scope);
                if (bson_iter_init_find(&scope_it, &wrap, "s")) {
                    out += ", ";
                    append_document(out, scope_it, false);
                }
                bson_destroy(&wrap);
            }
            out += ")";
            break;
        }
        default:
            out += compact_json(it);
            break;
    }
}

}  // namespace

std::string json_quote(std::string_view text) {
    std::string out = "\"";
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto c = static_cast<unsigned char>(text[i]);
        char buf[8];
        switch (c) {
            case '"': out += "\\\""; continue;
            case '\\': out += "\\\\"; continue;
            case '\n': out += "\\n"; continue;
            case '\r': out += "\\r"; continue;
            case '\t': out += "\\t"; continue;
            case '\b': out += "\\b"; continue;
            case '\f': out += "\\f"; continue;
            default: break;
        }
        if (c < 0x20 || c == 0x7F) {
            std::snprintf(buf, sizeof buf, "\\u%04X", c);
            out += buf;
        } else if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) >= 0x80 &&
                   static_cast<unsigned char>(text[i + 1]) <= 0x9F) {
            std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned char>(text[++i]));
            out += buf;
        } else if (c == 0xE2 && (text.substr(i, 3) == " " || text.substr(i, 3) == " ")) {
            out += text.substr(i, 3) == " " ? "\\u2028" : "\\u2029";
            i += 2;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += "\"";
    return out;
}

std::string to_mongosh(std::string_view collection, const bson_t& pipeline) {
    std::string out = "db.";
    if (is_identifier(collection, false) && collection.front() != '_' && !db_members().contains(collection)) {
        out += collection;
    } else {
        out = "db.getCollection(" + json_quote(collection) + ")";
    }
    out += ".aggregate([";
    bson_iter_t it;
    bool first = true;
    if (bson_iter_init(&it, &pipeline)) {
        while (bson_iter_next(&it)) {
            out += first ? "\n  " : ",\n  ";
            first = false;
            append_value(out, it);
        }
    }
    out += first ? "])" : "\n])";
    return out;
}

}  // namespace askmdb::mongo
