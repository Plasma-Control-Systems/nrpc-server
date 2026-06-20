#include "tls_transport.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace nrpc {
namespace {

[[noreturn]] void throw_tls(const char* operation, int status) {
    std::array<char, 256> message{};
    mbedtls_strerror(status, message.data(), message.size());
    throw std::runtime_error(std::string(operation) + " failed: " + message.data());
}

}  // namespace

class TlsContext::Impl {
public:
    Impl(const std::string& certificate_path, const std::string& private_key_path) {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&random);
        mbedtls_x509_crt_init(&certificate);
        mbedtls_pk_init(&key);
        mbedtls_ssl_config_init(&config);

        static constexpr char personalization[] = "nrpc-server";
        int status = mbedtls_ctr_drbg_seed(
            &random, mbedtls_entropy_func, &entropy,
            reinterpret_cast<const unsigned char*>(personalization),
            sizeof(personalization) - 1);
        if (status != 0) throw_tls("mbedtls_ctr_drbg_seed", status);
        status = mbedtls_x509_crt_parse_file(&certificate, certificate_path.c_str());
        if (status != 0) throw_tls("loading TLS certificate", status);
#if MBEDTLS_VERSION_MAJOR >= 3
        status = mbedtls_pk_parse_keyfile(&key, private_key_path.c_str(), nullptr,
                                          mbedtls_ctr_drbg_random, &random);
#else
        status = mbedtls_pk_parse_keyfile(&key, private_key_path.c_str(), nullptr);
#endif
        if (status != 0) throw_tls("loading TLS private key", status);
        status = mbedtls_ssl_config_defaults(
            &config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT);
        if (status != 0) throw_tls("mbedtls_ssl_config_defaults", status);
        mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &random);
        status = mbedtls_ssl_conf_own_cert(&config, &certificate, &key);
        if (status != 0) throw_tls("mbedtls_ssl_conf_own_cert", status);
    }

    ~Impl() {
        mbedtls_ssl_config_free(&config);
        mbedtls_pk_free(&key);
        mbedtls_x509_crt_free(&certificate);
        mbedtls_ctr_drbg_free(&random);
        mbedtls_entropy_free(&entropy);
    }

    mbedtls_entropy_context entropy{};
    mbedtls_ctr_drbg_context random{};
    mbedtls_x509_crt certificate{};
    mbedtls_pk_context key{};
    mbedtls_ssl_config config{};
};

class TlsSession::Impl {
public:
    explicit Impl(mbedtls_ssl_config& config) {
        mbedtls_ssl_init(&ssl);
        const int status = mbedtls_ssl_setup(&ssl, &config);
        if (status != 0) throw_tls("mbedtls_ssl_setup", status);
        mbedtls_ssl_set_bio(&ssl, this, &Impl::send, &Impl::receive, nullptr);
    }

    ~Impl() { mbedtls_ssl_free(&ssl); }

    static int send(void* context, const unsigned char* data, std::size_t size) {
        auto& self = *static_cast<Impl*>(context);
        const auto* begin = reinterpret_cast<const std::byte*>(data);
        self.output.insert(self.output.end(), begin, begin + size);
        return static_cast<int>(size);
    }

    static int receive(void* context, unsigned char* output, std::size_t size) {
        auto& self = *static_cast<Impl*>(context);
        const std::size_t available = self.input.size() - self.input_offset;
        if (available == 0) return MBEDTLS_ERR_SSL_WANT_READ;
        const std::size_t count = std::min(size, available);
        std::memcpy(output, self.input.data() + self.input_offset, count);
        self.input_offset += count;
        if (self.input_offset == self.input.size()) {
            self.input.clear();
            self.input_offset = 0;
        }
        return static_cast<int>(count);
    }

    mbedtls_ssl_context ssl{};
    std::vector<std::byte> input;
    std::size_t input_offset{0};
    std::vector<std::byte> output;
    bool established{false};
};

TlsContext::TlsContext(const std::string& certificate_path,
                       const std::string& private_key_path)
    : impl_(std::make_unique<Impl>(certificate_path, private_key_path)) {}
TlsContext::~TlsContext() = default;

std::unique_ptr<TlsSession> TlsContext::create_session() {
    return std::unique_ptr<TlsSession>(
        new TlsSession(std::make_unique<TlsSession::Impl>(impl_->config)));
}

TlsSession::TlsSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TlsSession::~TlsSession() = default;

void TlsSession::receive(std::span<const std::byte> encrypted) {
    impl_->input.insert(impl_->input.end(), encrypted.begin(), encrypted.end());
}

bool TlsSession::handshake() {
    if (impl_->established) return true;
    const int status = mbedtls_ssl_handshake(&impl_->ssl);
    if (status == 0) {
        impl_->established = true;
        return true;
    }
    if (status == MBEDTLS_ERR_SSL_WANT_READ || status == MBEDTLS_ERR_SSL_WANT_WRITE)
        return false;
    throw_tls("TLS handshake", status);
}

bool TlsSession::established() const noexcept { return impl_->established; }

std::vector<std::byte> TlsSession::read_plaintext() {
    std::vector<std::byte> result;
    std::array<std::byte, 16U * 1024U> buffer{};
    for (;;) {
        const int status = mbedtls_ssl_read(
            &impl_->ssl, reinterpret_cast<unsigned char*>(buffer.data()), buffer.size());
        if (status > 0) {
            result.insert(result.end(), buffer.begin(), buffer.begin() + status);
            continue;
        }
        if (status == MBEDTLS_ERR_SSL_WANT_READ || status == MBEDTLS_ERR_SSL_WANT_WRITE)
            return result;
        if (status == 0 || status == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            throw std::runtime_error("TLS peer closed the connection");
        throw_tls("TLS read", status);
    }
}

void TlsSession::write_plaintext(std::span<const std::byte> plaintext) {
    std::size_t offset = 0;
    while (offset < plaintext.size()) {
        const int status = mbedtls_ssl_write(
            &impl_->ssl,
            reinterpret_cast<const unsigned char*>(plaintext.data() + offset),
            plaintext.size() - offset);
        if (status > 0) {
            offset += static_cast<std::size_t>(status);
        } else if (status != MBEDTLS_ERR_SSL_WANT_WRITE) {
            throw_tls("TLS write", status);
        }
    }
}

std::vector<std::byte> TlsSession::take_encrypted_output() {
    std::vector<std::byte> result;
    result.swap(impl_->output);
    return result;
}

}  // namespace nrpc
