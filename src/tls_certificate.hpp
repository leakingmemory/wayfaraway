#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace wfa {

// Makes sure a TLS certificate and private key exist at the given paths.
// When both are missing, generates a self-signed RSA certificate for this
// host (key file mode 0600, written atomically). When only one exists, fails
// rather than overwrite anything. Returns whether a new pair was generated.
std::expected<bool, std::string> ensure_tls_certificate(const std::filesystem::path& cert_path,
    const std::filesystem::path& key_path);

// SHA-256 fingerprint of a PEM certificate, as colon-separated hex (the form
// RDP clients show when asking whether to trust a server).
std::expected<std::string, std::string> tls_certificate_fingerprint(const std::filesystem::path& cert_path);

} // namespace wfa
