#include "tls_certificate.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace wfa {

namespace {

constexpr int key_bits = 3072;
constexpr long validity_days = 3650;

// RAII for OpenSSL objects.
template <auto Free>
struct Deleter {
    template <typename T>
    void operator()(T* p) const { Free(p); }
};
using PKey = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY_free>>;
using Cert = std::unique_ptr<X509, Deleter<X509_free>>;
using Bio = std::unique_ptr<BIO, Deleter<BIO_free_all>>;
using BigNum = std::unique_ptr<BIGNUM, Deleter<BN_free>>;
using Extension = std::unique_ptr<X509_EXTENSION, Deleter<X509_EXTENSION_free>>;

std::string openssl_error(std::string_view what)
{
    char buffer[256]{};
    ERR_error_string_n(ERR_get_error(), buffer, sizeof(buffer));
    return std::format("{}: {}", what, buffer);
}

std::string host_name()
{
    std::array<char, 256> name{};
    if (gethostname(name.data(), name.size() - 1) != 0 || name[0] == '\0')
        return "localhost";
    return name.data();
}

bool add_extension(X509* cert, X509V3_CTX* ctx, int nid, const std::string& value)
{
    Extension ext(X509V3_EXT_conf_nid(nullptr, ctx, nid, value.c_str()));
    return ext && X509_add_ext(cert, ext.get(), -1) == 1;
}

std::expected<std::string, std::string> to_pem(auto&& write)
{
    Bio bio(BIO_new(BIO_s_mem()));
    if (!bio || !write(bio.get()))
        return std::unexpected(openssl_error("PEM encoding failed"));
    char* data = nullptr;
    long length = BIO_get_mem_data(bio.get(), &data);
    return std::string(data, static_cast<std::size_t>(length));
}

// Writes `contents` to a temporary file next to `path`, then renames it into
// place, so a crash never leaves a truncated key or certificate behind.
std::expected<void, std::string> write_atomically(const std::filesystem::path& path, const std::string& contents,
    mode_t mode)
{
    const std::filesystem::path temp = path.string() + ".tmp";
    std::error_code ec;
    std::filesystem::remove(temp, ec);

    int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (fd < 0)
        return std::unexpected(std::format("cannot create {}: {}", temp.string(), std::strerror(errno)));

    std::size_t written = 0;
    while (written < contents.size()) {
        ssize_t n = write(fd, contents.data() + written, contents.size() - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            const int saved = errno;
            close(fd);
            std::filesystem::remove(temp, ec);
            return std::unexpected(std::format("cannot write {}: {}", temp.string(), std::strerror(saved)));
        }
        written += static_cast<std::size_t>(n);
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(std::format("cannot write {}: {}", temp.string(), std::strerror(errno)));
    }

    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(std::format("cannot create {}: {}", path.string(), ec.message()));
    }
    return {};
}

std::expected<void, std::string> generate(const std::filesystem::path& cert_path,
    const std::filesystem::path& key_path)
{
    PKey key(EVP_RSA_gen(key_bits));
    if (!key)
        return std::unexpected(openssl_error("key generation failed"));

    Cert cert(X509_new());
    if (!cert || X509_set_version(cert.get(), X509_VERSION_3) != 1)
        return std::unexpected(openssl_error("certificate setup failed"));

    // A random 159-bit serial, as RFC 5280 recommends.
    BigNum serial(BN_new());
    if (!serial || BN_rand(serial.get(), 159, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1
        || !BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(cert.get())))
        return std::unexpected(openssl_error("serial number generation failed"));

    // Backdated an hour to tolerate clock skew between client and server.
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60 * 60);
    X509_time_adj_ex(X509_getm_notAfter(cert.get()), validity_days, 0, nullptr);

    const std::string host = host_name();
    X509_NAME* name = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>("wayfaraway"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(host.c_str()), -1, -1, 0);
    X509_set_issuer_name(cert.get(), name);
    X509_set_pubkey(cert.get(), key.get());

    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert.get(), cert.get(), nullptr, nullptr, 0);
    std::string san = std::format("DNS:{}", host);
    if (host != "localhost")
        san += ",DNS:localhost";
    san += ",IP:127.0.0.1,IP:::1";
    const bool extensions = add_extension(cert.get(), &ctx, NID_basic_constraints, "critical,CA:FALSE")
        && add_extension(cert.get(), &ctx, NID_key_usage, "critical,digitalSignature,keyEncipherment")
        && add_extension(cert.get(), &ctx, NID_ext_key_usage, "serverAuth")
        && add_extension(cert.get(), &ctx, NID_subject_key_identifier, "hash")
        && add_extension(cert.get(), &ctx, NID_subject_alt_name, san);
    if (!extensions)
        return std::unexpected(openssl_error("certificate extensions failed"));

    if (X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0)
        return std::unexpected(openssl_error("certificate signing failed"));

    auto key_pem = to_pem([&](BIO* bio) {
        return PEM_write_bio_PrivateKey(bio, key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1;
    });
    auto cert_pem = to_pem([&](BIO* bio) { return PEM_write_bio_X509(bio, cert.get()) == 1; });
    if (!key_pem)
        return std::unexpected(key_pem.error());
    if (!cert_pem)
        return std::unexpected(cert_pem.error());

    // Key first: a certificate without its key would block regeneration.
    if (auto written = write_atomically(key_path, *key_pem, 0600); !written)
        return written;
    if (auto written = write_atomically(cert_path, *cert_pem, 0644); !written) {
        std::error_code ec;
        std::filesystem::remove(key_path, ec);
        return written;
    }
    return {};
}

void create_private_directory(const std::filesystem::path& file)
{
    const std::filesystem::path dir = file.parent_path();
    if (dir.empty())
        return;
    std::error_code ec;
    if (std::filesystem::create_directories(dir, ec))
        std::filesystem::permissions(dir, std::filesystem::perms::owner_all, ec);
}

} // namespace

std::expected<bool, std::string> ensure_tls_certificate(const std::filesystem::path& cert_path,
    const std::filesystem::path& key_path)
{
    std::error_code ec;
    const bool have_cert = std::filesystem::exists(cert_path, ec);
    const bool have_key = std::filesystem::exists(key_path, ec);
    if (have_cert && have_key)
        return false;
    if (have_cert != have_key) {
        return std::unexpected(std::format("{} exists but {} does not; restore it, or remove both to have a new "
                                           "certificate generated",
            (have_cert ? cert_path : key_path).string(), (have_cert ? key_path : cert_path).string()));
    }

    create_private_directory(key_path);
    create_private_directory(cert_path);
    if (auto generated = generate(cert_path, key_path); !generated)
        return std::unexpected(generated.error());
    return true;
}

std::expected<std::string, std::string> tls_certificate_fingerprint(const std::filesystem::path& cert_path)
{
    Bio bio(BIO_new_file(cert_path.c_str(), "r"));
    if (!bio)
        return std::unexpected(openssl_error(std::format("cannot open {}", cert_path.string())));
    Cert cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
    if (!cert)
        return std::unexpected(openssl_error(std::format("cannot parse {}", cert_path.string())));

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (X509_digest(cert.get(), EVP_sha256(), digest, &length) != 1)
        return std::unexpected(openssl_error("fingerprint failed"));

    std::string hex;
    for (unsigned int i = 0; i < length; ++i)
        hex += std::format("{}{:02X}", i ? ":" : "", digest[i]);
    return hex;
}

} // namespace wfa
