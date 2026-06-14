#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "nrpc/dispatcher.hpp"
#include "nrpc/error_codes.hpp"

namespace nrpc {

namespace tcp_function {
inline constexpr std::uint8_t open = 16;
inline constexpr std::uint8_t write = 17;
inline constexpr std::uint8_t read = 18;
inline constexpr std::uint8_t close = 19;
}  // namespace tcp_function

namespace tcp_error {
inline constexpr std::uint32_t invalid_arguments = 2001;
inline constexpr std::uint32_t resolve_failed = 2002;
inline constexpr std::uint32_t connect_failed = 2003;
inline constexpr std::uint32_t invalid_handle = 2004;
inline constexpr std::uint32_t write_failed = 2005;
inline constexpr std::uint32_t read_failed = 2006;
inline constexpr std::uint32_t operation_timeout = 2007;
}  // namespace tcp_error

class TcpSocket {
public:
    TcpSocket() noexcept = default;
    ~TcpSocket();
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    static std::expected<std::unique_ptr<TcpSocket>, BackendError> connect(
        std::string_view host, std::uint16_t port, std::uint32_t timeout_ms);
    std::expected<std::size_t, BackendError> write(
        std::span<const std::byte> data, std::uint32_t timeout_ms);
    std::expected<std::size_t, BackendError> read(
        std::span<std::byte> output, std::uint32_t timeout_ms);
    void close() noexcept;

private:
#ifdef _WIN32
    using NativeHandle = std::uintptr_t;
    static constexpr NativeHandle invalid_handle = ~NativeHandle{0};
#else
    using NativeHandle = int;
    static constexpr NativeHandle invalid_handle = -1;
#endif

    explicit TcpSocket(NativeHandle handle) noexcept : handle_(handle) {}
    NativeHandle handle_{invalid_handle};
};

void register_tcp_backend(Dispatcher& dispatcher);

}  // namespace nrpc
