#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nrpc {

class TlsSession;

class TlsContext {
public:
    TlsContext(const std::string& certificate_path, const std::string& private_key_path);
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    [[nodiscard]] std::unique_ptr<TlsSession> create_session();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class TlsSession {
public:
    ~TlsSession();
    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;

    void receive(std::span<const std::byte> encrypted);
    // Advances the handshake and returns false when more network input is needed.
    bool handshake();
    [[nodiscard]] bool established() const noexcept;
    [[nodiscard]] std::vector<std::byte> read_plaintext();
    void write_plaintext(std::span<const std::byte> plaintext);
    [[nodiscard]] std::vector<std::byte> take_encrypted_output();

private:
    friend class TlsContext;
    class Impl;
    explicit TlsSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace nrpc
