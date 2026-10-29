// Trusted CAs for TLS. A static OpenSSL does not know where each distro keeps its bundle, so it is
// searched here, in a fixed order, unless the URI names one with tlsCAFile.
#include <cstddef>

#include "mongo/mongo.hpp"

namespace askmdb::mongo::detail {

std::optional<CaLocation> find_ca(const GetEnv& getenv, const PathExists& exists) {
    if (auto file = getenv("SSL_CERT_FILE"); file && !file->empty() && exists(*file, false)) {
        return CaLocation{*file, {}};
    }
    if (auto dir = getenv("SSL_CERT_DIR"); dir && !dir->empty()) {
        // OpenSSL accepts a ':'-separated list: usable if any entry exists.
        std::size_t start = 0;
        while (start <= dir->size()) {
            std::size_t end = dir->find(':', start);
            if (end == std::string::npos) end = dir->size();
            std::string entry = dir->substr(start, end - start);
            if (!entry.empty() && exists(entry, true)) return CaLocation{{}, *dir};
            start = end + 1;
        }
    }
    for (const char* file : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                             "/etc/ssl/ca-bundle.pem", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"}) {
        if (exists(file, false)) return CaLocation{file, {}};
    }
    if (exists("/etc/ssl/certs/", true)) return CaLocation{{}, "/etc/ssl/certs/"};
    return std::nullopt;
}

}  // namespace askmdb::mongo::detail
