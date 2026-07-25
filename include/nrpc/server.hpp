#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "nrpc/dispatcher.hpp"
#include "nrpc/error_codes.hpp"
#include "nrpc/logger.hpp"
#include "nrpc/resource_manager.hpp"

namespace nrpc {

struct ServerConfig {
    std::string host{"0.0.0.0"};
    std::uint16_t port{9000};
    std::uint32_t max_payload_size{protocol::default_max_payload_size};
    std::uint32_t default_timeout_ms{0};
    std::size_t arena_size{1024U * 1024U};
    std::size_t max_pending_requests{256};
    std::size_t max_pending_request_bytes{256U * 1024U * 1024U};
    int backlog{128};
    // TLS is enabled when both paths are set. PEM and DER files are accepted.
    std::string tls_certificate_path;
    std::string tls_private_key_path;
};

class Server {
public:
    Server(ServerConfig config, Dispatcher& dispatcher, ResourceManager& resources,
           Logger& logger);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void start();
    void run();
    void stop();
    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nrpc
