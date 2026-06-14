#include "nrpc/tcp_backend.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace nrpc {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle invalid_socket = INVALID_SOCKET;
using SocketLength = int;

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        error_ = WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockRuntime() {
        if (error_ == 0) {
            WSACleanup();
        }
    }
    int error() const noexcept { return error_; }

private:
    int error_;
};

int last_socket_error() { return WSAGetLastError(); }
void close_socket(SocketHandle socket) { closesocket(socket); }
bool interrupted(int error) { return error == WSAEINTR; }
bool would_block(int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}
bool timed_out(int error) { return error == WSAETIMEDOUT; }

std::expected<void, BackendError> ensure_socket_runtime() {
    static WinsockRuntime runtime;
    if (runtime.error() != 0) {
        return std::unexpected(
            BackendError{tcp_error::connect_failed, "WSAStartup failed"});
    }
    return {};
}
#else
using SocketHandle = int;
constexpr SocketHandle invalid_socket = -1;
using SocketLength = socklen_t;

int last_socket_error() { return errno; }
void close_socket(SocketHandle socket) { ::close(socket); }
bool interrupted(int error) { return error == EINTR; }
bool would_block(int error) { return error == EINPROGRESS || error == EWOULDBLOCK; }
bool timed_out(int error) { return error == EAGAIN || error == EWOULDBLOCK; }

std::expected<void, BackendError> ensure_socket_runtime() { return {}; }
#endif

BackendError socket_error(std::uint32_t code, std::string_view operation, int error) {
#ifdef _WIN32
    return BackendError{code, std::string(operation) + " failed with socket error " +
                                  std::to_string(error)};
#else
    return BackendError{code, std::string(operation) + " failed: " +
                                  std::strerror(error)};
#endif
}

bool set_nonblocking(SocketHandle socket, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1UL : 0UL;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 &&
           fcntl(socket, F_SETFL, enabled ? flags | O_NONBLOCK
                                         : flags & ~O_NONBLOCK) == 0;
#endif
}

bool wait_writable(SocketHandle socket, std::uint32_t timeout_ms) {
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(socket, &write_set);
    timeval timeout{
        .tv_sec = static_cast<long>(timeout_ms / 1000U),
        .tv_usec = static_cast<long>((timeout_ms % 1000U) * 1000U),
    };
#ifdef _WIN32
    return select(0, nullptr, &write_set, nullptr, &timeout) > 0;
#else
    return select(socket + 1, nullptr, &write_set, nullptr, &timeout) > 0;
#endif
}

bool set_socket_timeout(SocketHandle socket, int option, std::uint32_t timeout_ms) {
#ifdef _WIN32
    const DWORD value = timeout_ms;
    return setsockopt(socket, SOL_SOCKET, option,
                      reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
#else
    const timeval value{
        .tv_sec = static_cast<time_t>(timeout_ms / 1000U),
        .tv_usec = static_cast<suseconds_t>((timeout_ms % 1000U) * 1000U),
    };
    return setsockopt(socket, SOL_SOCKET, option, &value, sizeof(value)) == 0;
#endif
}

bool configure_connected_socket(SocketHandle socket) {
#if defined(SO_NOSIGPIPE) && !defined(_WIN32)
    const int enabled = 1;
    return setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                      sizeof(enabled)) == 0;
#else
    (void)socket;
    return true;
#endif
}

std::uint64_t read_u64(std::span<const std::byte> data) {
    std::uint64_t value = 0;
    for (const auto byte : data) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
    }
    return value;
}

std::uint32_t read_u32(std::span<const std::byte> data) {
    std::uint32_t value = 0;
    for (const auto byte : data) {
        value = (value << 8U) | std::to_integer<std::uint32_t>(byte);
    }
    return value;
}

void write_u64(std::span<std::byte> output, std::uint64_t value) {
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto shift = static_cast<unsigned>((output.size() - index - 1U) * 8U);
        output[index] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

void write_u32(std::span<std::byte> output, std::uint32_t value) {
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto shift = static_cast<unsigned>((output.size() - index - 1U) * 8U);
        output[index] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

bool is_scalar(const protocol::Variable& variable, protocol::DataType type,
               std::size_t size) {
    return variable.type == type && variable.element_count == 1 &&
           variable.data.size() == size;
}

std::expected<std::string, BackendError> parse_string(
    const protocol::Variable& variable) {
    if (variable.type != protocol::DataType::string || variable.data.empty() ||
        variable.data.back() != std::byte{0}) {
        return std::unexpected(
            BackendError{tcp_error::invalid_arguments, "expected a string"});
    }
    return std::string(reinterpret_cast<const char*>(variable.data.data()),
                       variable.data.size() - 1U);
}

BackendResult scalar_u64(Arena& arena, std::uint64_t value) {
    auto memory = arena.allocate(8, 8);
    if (!memory) {
        return std::unexpected(BackendError{
            error_code::arena_exhausted, "response arena exhausted"});
    }
    write_u64(*memory, value);
    return std::vector<protocol::VariableView>{
        {protocol::DataType::uint64, 1, std::as_const(*memory)}};
}

BackendResult scalar_u32(Arena& arena, std::uint32_t value) {
    auto memory = arena.allocate(4, 4);
    if (!memory) {
        return std::unexpected(BackendError{
            error_code::arena_exhausted, "response arena exhausted"});
    }
    write_u32(*memory, value);
    return std::vector<protocol::VariableView>{
        {protocol::DataType::uint32, 1, std::as_const(*memory)}};
}

std::atomic<std::uint64_t> next_handle_id{1};

}  // namespace

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : handle_(std::exchange(other.handle_, invalid_handle)) {}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, invalid_handle);
    }
    return *this;
}

std::expected<std::unique_ptr<TcpSocket>, BackendError> TcpSocket::connect(
    std::string_view host, std::uint16_t port, std::uint32_t timeout_ms) {
    if (const auto runtime = ensure_socket_runtime(); !runtime) {
        return std::unexpected(runtime.error());
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    const std::string host_string(host);
    const int resolved =
        getaddrinfo(host_string.c_str(), service.c_str(), &hints, &addresses);
    if (resolved != 0) {
        return std::unexpected(
            BackendError{tcp_error::resolve_failed, "TCP host resolution failed"});
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> address_guard(
        addresses, freeaddrinfo);
    BackendError last_error{tcp_error::connect_failed, "TCP connection failed"};
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        const auto socket =
            ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket == invalid_socket) {
            last_error = socket_error(tcp_error::connect_failed, "socket",
                                      last_socket_error());
            continue;
        }
        bool connected = false;
        if (timeout_ms == 0) {
            connected = ::connect(socket, address->ai_addr,
                                  static_cast<SocketLength>(address->ai_addrlen)) == 0;
        } else if (set_nonblocking(socket, true)) {
            const int status = ::connect(
                socket, address->ai_addr,
                static_cast<SocketLength>(address->ai_addrlen));
            if (status == 0) {
                connected = true;
            } else if (would_block(last_socket_error()) &&
                       wait_writable(socket, timeout_ms)) {
                int error = 0;
                SocketLength length = sizeof(error);
#ifdef _WIN32
                connected = getsockopt(socket, SOL_SOCKET, SO_ERROR,
                                       reinterpret_cast<char*>(&error), &length) == 0 &&
                            error == 0;
#else
                connected =
                    getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
                    error == 0;
#endif
            }
            set_nonblocking(socket, false);
        }
        if (connected) {
            if (!configure_connected_socket(socket)) {
                last_error = socket_error(tcp_error::connect_failed,
                                          "setsockopt(SO_NOSIGPIPE)",
                                          last_socket_error());
                close_socket(socket);
                continue;
            }
            return std::unique_ptr<TcpSocket>(
                new TcpSocket(static_cast<NativeHandle>(socket)));
        }
        last_error = socket_error(tcp_error::connect_failed, "connect",
                                  last_socket_error());
        close_socket(socket);
    }
    return std::unexpected(std::move(last_error));
}

std::expected<std::size_t, BackendError> TcpSocket::write(
    std::span<const std::byte> data, std::uint32_t timeout_ms) {
    const auto socket = static_cast<SocketHandle>(handle_);
    if (!set_socket_timeout(socket, SO_SNDTIMEO, timeout_ms)) {
        return std::unexpected(socket_error(
            tcp_error::write_failed, "setsockopt(SO_SNDTIMEO)", last_socket_error()));
    }
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto remaining = std::min<std::size_t>(
            data.size() - sent, static_cast<std::size_t>(std::numeric_limits<int>::max()));
#ifdef _WIN32
        const int result =
            ::send(socket, reinterpret_cast<const char*>(data.data() + sent),
                   static_cast<int>(remaining), 0);
#elif defined(MSG_NOSIGNAL)
        const auto result =
            ::send(socket, data.data() + sent, remaining, MSG_NOSIGNAL);
#else
        const auto result = ::send(socket, data.data() + sent, remaining, 0);
#endif
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        const int error = last_socket_error();
        if (interrupted(error)) {
            continue;
        }
        return std::unexpected(socket_error(
            timed_out(error) ? tcp_error::operation_timeout : tcp_error::write_failed,
            "send", error));
    }
    return sent;
}

std::expected<std::size_t, BackendError> TcpSocket::read(
    std::span<std::byte> output, std::uint32_t timeout_ms) {
    const auto socket = static_cast<SocketHandle>(handle_);
    if (!set_socket_timeout(socket, SO_RCVTIMEO, timeout_ms)) {
        return std::unexpected(socket_error(
            tcp_error::read_failed, "setsockopt(SO_RCVTIMEO)", last_socket_error()));
    }
#ifdef _WIN32
    const int result =
        ::recv(socket, reinterpret_cast<char*>(output.data()),
               static_cast<int>(std::min<std::size_t>(
                   output.size(), static_cast<std::size_t>(std::numeric_limits<int>::max()))),
               0);
#else
    const auto result = ::recv(socket, output.data(), output.size(), 0);
#endif
    if (result > 0) {
        return static_cast<std::size_t>(result);
    }
    if (result == 0) {
        return std::unexpected(
            BackendError{tcp_error::read_failed, "TCP peer closed the connection"});
    }
    const int error = last_socket_error();
    return std::unexpected(socket_error(
        timed_out(error) ? tcp_error::operation_timeout : tcp_error::read_failed,
        "recv", error));
}

void TcpSocket::close() noexcept {
    if (handle_ != invalid_handle) {
        close_socket(static_cast<SocketHandle>(handle_));
        handle_ = invalid_handle;
    }
}

void register_tcp_backend(Dispatcher& dispatcher) {
    dispatcher.register_handler(
        tcp_function::open,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena& arena) -> BackendResult {
            if (arguments.size() != 2 ||
                !is_scalar(arguments[1], protocol::DataType::uint16, 2)) {
                return std::unexpected(BackendError{
                    tcp_error::invalid_arguments, "TCP open expects host and uint16 port"});
            }
            auto host = parse_string(arguments[0]);
            if (!host) {
                return std::unexpected(host.error());
            }
            const auto port = static_cast<std::uint16_t>(read_u32(
                std::array{std::byte{0}, std::byte{0}, arguments[1].data[0],
                           arguments[1].data[1]}));
            auto socket = TcpSocket::connect(*host, port, context.timeout_ms);
            if (!socket) {
                return std::unexpected(socket.error());
            }
            const auto handle_id = next_handle_id.fetch_add(1);
            if (!context.resources.register_handle(
                    context.client_id, handle_id, std::move(*socket))) {
                return std::unexpected(
                    BackendError{tcp_error::connect_failed, "handle registration failed"});
            }
            return scalar_u64(arena, handle_id);
        });

    dispatcher.register_handler(
        tcp_function::write,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena& arena) -> BackendResult {
            if (arguments.size() != 2 ||
                !is_scalar(arguments[0], protocol::DataType::uint64, 8) ||
                arguments[1].type != protocol::DataType::bytes) {
                return std::unexpected(BackendError{
                    tcp_error::invalid_arguments, "TCP write expects handle and bytes"});
            }
            const auto handle_id = read_u64(arguments[0].data);
            auto result = context.resources.with_handle<TcpSocket>(
                context.client_id, handle_id,
                [&](TcpSocket& socket) {
                    return socket.write(arguments[1].data, context.timeout_ms);
                });
            if (!result) {
                return std::unexpected(
                    BackendError{tcp_error::invalid_handle, "unknown TCP handle"});
            }
            if (!*result) {
                return std::unexpected(result->error());
            }
            return scalar_u32(arena, static_cast<std::uint32_t>(**result));
        });

    dispatcher.register_handler(
        tcp_function::read,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena& arena) -> BackendResult {
            if (arguments.size() != 2 ||
                !is_scalar(arguments[0], protocol::DataType::uint64, 8) ||
                !is_scalar(arguments[1], protocol::DataType::uint32, 4)) {
                return std::unexpected(BackendError{
                    tcp_error::invalid_arguments, "TCP read expects handle and max size"});
            }
            const auto handle_id = read_u64(arguments[0].data);
            const auto max_size = read_u32(arguments[1].data);
            if (max_size == 0) {
                return std::unexpected(BackendError{
                    tcp_error::invalid_arguments, "TCP read size must be nonzero"});
            }
            auto memory = arena.allocate(max_size, 1);
            if (!memory) {
                return std::unexpected(BackendError{
                    error_code::arena_exhausted, "response arena exhausted"});
            }
            auto result = context.resources.with_handle<TcpSocket>(
                context.client_id, handle_id,
                [&](TcpSocket& socket) {
                    return socket.read(*memory, context.timeout_ms);
                });
            if (!result) {
                return std::unexpected(
                    BackendError{tcp_error::invalid_handle, "unknown TCP handle"});
            }
            if (!*result) {
                return std::unexpected(result->error());
            }
            return std::vector<protocol::VariableView>{
                {protocol::DataType::bytes, static_cast<std::uint32_t>(**result),
                 std::as_const(*memory).first(**result)}};
        });

    dispatcher.register_handler(
        tcp_function::close,
        [](const RequestContext& context,
           std::span<const protocol::Variable> arguments,
           Arena&) -> BackendResult {
            if (arguments.size() != 1 ||
                !is_scalar(arguments[0], protocol::DataType::uint64, 8)) {
                return std::unexpected(BackendError{
                    tcp_error::invalid_arguments, "TCP close expects a handle"});
            }
            if (!context.resources.remove(context.client_id,
                                          read_u64(arguments[0].data))) {
                return std::unexpected(
                    BackendError{tcp_error::invalid_handle, "unknown TCP handle"});
            }
            return std::vector<protocol::VariableView>{};
        });
}

}  // namespace nrpc
