// Lazy libmongoc client: global initialisation, URI limits, TLS trust and error classification.
#include <sys/stat.h>

#include <cstdlib>
#include <mutex>

#if ASKMDB_HAVE_TLS
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#endif

#include "mongo/client_impl.hpp"

namespace askmdb::mongo {

namespace {

void global_init() {
    static std::once_flag once;
    std::call_once(once, [] {
#if ASKMDB_HAVE_TLS
        // First OpenSSL initialisation of the process: never read the system openssl.cnf.
        OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr);
#endif
        silence_driver_logs();
        mongoc_init();
    });
}

#if ASKMDB_HAVE_TLS
std::optional<detail::CaLocation> system_ca() {
    auto getenv = [](const char* name) -> std::optional<std::string> {
        const char* value = std::getenv(name);
        if (!value) return std::nullopt;
        return std::string(value);
    };
    auto exists = [](const std::string& path, bool is_dir) {
        struct stat st{};
        if (stat(path.c_str(), &st) != 0) return false;
        return is_dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode);
    };
    return detail::find_ca(getenv, exists);
}

// mongoc_client_set_ssl_opts replaces what libmongoc took from the URI, so those options are copied.
void set_trust(mongoc_client_t* client, const mongoc_uri_t* uri) {
    if (mongoc_uri_get_option_as_utf8(uri, MONGOC_URI_TLSCAFILE, nullptr)) return;
    auto ca = system_ca();
    if (!ca) return;
    bool insecure = mongoc_uri_get_option_as_bool(uri, MONGOC_URI_TLSINSECURE, false);
    mongoc_ssl_opt_t opts = *mongoc_ssl_opt_get_default();
    opts.pem_file = mongoc_uri_get_option_as_utf8(uri, MONGOC_URI_TLSCERTIFICATEKEYFILE, nullptr);
    opts.pem_pwd = mongoc_uri_get_option_as_utf8(uri, MONGOC_URI_TLSCERTIFICATEKEYFILEPASSWORD, nullptr);
    opts.weak_cert_validation =
        mongoc_uri_get_option_as_bool(uri, MONGOC_URI_TLSALLOWINVALIDCERTIFICATES, insecure);
    opts.allow_invalid_hostname = mongoc_uri_get_option_as_bool(uri, MONGOC_URI_TLSALLOWINVALIDHOSTNAMES, insecure);
    opts.ca_file = ca->file.empty() ? nullptr : ca->file.c_str();
    opts.ca_dir = ca->dir.empty() ? nullptr : ca->dir.c_str();
    mongoc_client_set_ssl_opts(client, &opts);
}
#endif

}  // namespace

std::string library_versions() {
    std::string out = std::string("libmongoc ") + mongoc_get_version();
#if ASKMDB_HAVE_TLS
    // OpenSSL_version() returns static text: no library initialisation, no openssl.cnf.
    out += std::string(", OpenSSL ") + OpenSSL_version(OPENSSL_VERSION_STRING);
#else
    out += ", OpenSSL: none";
#endif
    return out;
}

void silence_driver_logs() {
    static std::once_flag once;
    std::call_once(once, [] { mongoc_log_set_handler(nullptr, nullptr); });
}

namespace detail {

Error classify(std::uint32_t domain, std::uint32_t code, std::string_view message, const Uri& uri) {
    Error out;
    out.message = uri.scrub(message);
    if (domain == MONGOC_ERROR_SERVER_SELECTION || domain == MONGOC_ERROR_STREAM) {
        out.kind = Error::Kind::Connect;
    } else if ((domain == MONGOC_ERROR_CLIENT && code == MONGOC_ERROR_CLIENT_AUTHENTICATE) ||
               domain == MONGOC_ERROR_SCRAM) {
        out.kind = Error::Kind::Auth;
    } else if (domain == MONGOC_ERROR_SERVER || domain == MONGOC_ERROR_QUERY) {
        if (code == 18 || code == 13) {
            out.kind = Error::Kind::Auth;  // AuthenticationFailed, Unauthorized
        } else if (code == 50) {
            out.kind = Error::Kind::Timeout;  // MaxTimeMSExpired
        }
    }
    return out;
}

}  // namespace detail

Client::Impl::~Impl() {
    if (client) mongoc_client_destroy(client);
}

Error Client::Impl::error_from(const bson_error_t& err) const {
    return detail::classify(err.domain, err.code, err.message, uri);
}

mongoc_client_t* Client::Impl::get(Error& error) {
    if (client) return client;
#if !ASKMDB_HAVE_TLS
    if (uri.tls) {
        error = {Error::Kind::Connect, "this askmdb was built without TLS support and the URI requires TLS"};
        return nullptr;
    }
#endif
    global_init();
    bson_error_t err{};
    mongoc_uri_t* parsed = mongoc_uri_new_with_error(uri.text.c_str(), &err);
    if (!parsed) {
        error = {Error::Kind::Connect, uri.scrub(err.message)};
        return nullptr;
    }
    mongoc_uri_set_option_as_int32(parsed, MONGOC_URI_SERVERSELECTIONTIMEOUTMS,
                                   static_cast<int32_t>(kServerSelectionTimeoutMs));
    mongoc_uri_set_option_as_int32(parsed, MONGOC_URI_CONNECTTIMEOUTMS, static_cast<int32_t>(kConnectTimeoutMs));
    mongoc_uri_set_appname(parsed, "askmdb/" ASKMDB_VERSION);
    // For mongodb+srv this resolves the SRV record: the first network access, still on demand.
    client = mongoc_client_new_from_uri_with_error(parsed, &err);
    if (!client) {
        mongoc_uri_destroy(parsed);
        error = {Error::Kind::Connect, uri.scrub(err.message)};
        return nullptr;
    }
    mongoc_client_set_error_api(client, MONGOC_ERROR_API_VERSION_2);
#if ASKMDB_HAVE_TLS
    if (uri.tls) set_trust(client, parsed);
#endif
    mongoc_uri_destroy(parsed);
    return client;
}

Client::Client(Uri uri, std::string db) : impl_(std::make_unique<Impl>(std::move(uri), std::move(db))) {}

Client::~Client() = default;

}  // namespace askmdb::mongo
