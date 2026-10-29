// Connection string: parsing with libmongoc, redaction and credential scrubbing.
#include <algorithm>
#include <cctype>

#include "mongo/client_impl.hpp"

namespace askmdb::mongo {

namespace {

constexpr std::string_view kMask = "***";

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Percent-encodes everything except RFC 3986 unreserved characters.
std::string percent_encode(std::string_view text, bool lowercase_hex) {
    const char* hex = lowercase_hex ? "0123456789abcdef" : "0123456789ABCDEF";
    std::string out;
    for (char ch : text) {
        auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += ch;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// The user info as typed ("user:pass" before the last '@' of the authority), or empty.
std::string_view raw_userinfo(std::string_view text) {
    auto scheme = text.find("://");
    if (scheme == std::string_view::npos) return {};
    // Options start at the first '?' and may hold an '@' of their own; a raw '/' in a password is
    // invalid but possible, so the last '@' before the options ends the user info.
    std::string_view rest = text.substr(scheme + 3);
    rest = rest.substr(0, rest.find('?'));
    auto at = rest.rfind('@');
    if (at == std::string_view::npos) return {};
    return rest.substr(0, at);
}

bool word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Replaces `needle` with the mask; with `token`, only where it is not part of a longer word.
std::string replace_all(std::string text, std::string_view needle, bool token) {
    if (needle.empty()) return text;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        std::size_t end = pos + needle.size();
        bool whole = (pos == 0 || !word_char(text[pos - 1])) && (end == text.size() || !word_char(text[end]));
        if (token && !whole) {
            ++pos;
            continue;
        }
        text.replace(pos, needle.size(), kMask);
        pos += kMask.size();
    }
    return text;
}

struct Needle {
    std::string text;
    bool token;  // only as a whole word
};

// User names shorter than this are replaced only as whole words, so that a user "u" does not
// garble every message; the URI, the user info and the password are replaced anywhere.
constexpr std::size_t kUserAnywhereMin = 4;

// Every spelling of the secrets that may show up in a message, longest first.
std::vector<Needle> secrets(std::string_view text, std::string_view user, std::string_view password) {
    std::vector<Needle> out;
    auto anywhere = [&out](std::string_view s) {
        if (!s.empty()) out.push_back({std::string(s), false});
    };
    auto user_name = [&out](std::string_view s) {
        if (!s.empty()) out.push_back({std::string(s), s.size() < kUserAnywhereMin});
    };
    anywhere(text);
    std::string_view info = raw_userinfo(text);
    if (!info.empty()) {
        anywhere(std::string(info) + "@");
        auto colon = info.find(':');
        user_name(info.substr(0, colon));
        if (colon != std::string_view::npos) anywhere(info.substr(colon + 1));
    }
    for (bool lower_hex : {false, true}) {
        std::string u = percent_encode(user, lower_hex);
        std::string p = percent_encode(password, lower_hex);
        user_name(user);
        user_name(u);
        anywhere(password);
        anywhere(p);
        if (!user.empty()) {
            anywhere(u + "@");
            if (!password.empty()) anywhere(u + ":" + p + "@");
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Needle& a, const Needle& b) { return a.text.size() > b.text.size(); });
    return out;
}

std::string scrub_with(std::string_view message, const std::vector<Needle>& needles) {
    std::string out(message);
    for (const auto& needle : needles) out = replace_all(std::move(out), needle.text, needle.token);
    return out;
}

}  // namespace

std::optional<Uri> Uri::parse(std::string_view uri, std::string& error) {
    Uri out;
    out.text = std::string(uri);
    silence_driver_logs();
    bson_error_t err{};
    mongoc_uri_t* parsed = mongoc_uri_new_with_error(out.text.c_str(), &err);
    if (!parsed) {
        error = scrub_with(err.message, secrets(out.text, {}, {}));
        return std::nullopt;
    }
    if (const char* srv = mongoc_uri_get_srv_hostname(parsed)) {
        out.srv = true;
        out.hosts.emplace_back(srv);
    } else {
        for (const mongoc_host_list_t* h = mongoc_uri_get_hosts(parsed); h; h = h->next) {
            out.hosts.emplace_back(h->host_and_port);
        }
    }
    if (const char* db = mongoc_uri_get_database(parsed)) out.db = db;
    if (const char* user = mongoc_uri_get_username(parsed)) out.user = user;
    if (const char* password = mongoc_uri_get_password(parsed)) out.password = password;
    out.tls = mongoc_uri_get_tls(parsed);
    mongoc_uri_destroy(parsed);
    error.clear();
    return out;
}

std::string Uri::redacted(std::string_view db_override) const {
    std::string out = srv ? "mongodb+srv://" : "mongodb://";
    for (std::size_t i = 0; i < hosts.size(); ++i) {
        if (i) out += ',';
        out += hosts[i];
    }
    std::string_view name = db_override.empty() ? std::string_view(db) : db_override;
    if (!name.empty()) {
        out += '/';
        out += name;
    }
    return out;
}

std::string Uri::host_key() const {
    std::vector<std::string> keys;
    for (const auto& h : hosts) keys.push_back(lower(h));
    std::sort(keys.begin(), keys.end());
    std::string out = srv ? "srv:" : "";
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i) out += ',';
        out += keys[i];
    }
    return out;
}

std::string Uri::scrub(std::string_view message) const {
    return scrub_with(message, secrets(text, user, password));
}

}  // namespace askmdb::mongo
