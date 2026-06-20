#include "nrpc/serial_backend.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <poll.h>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <utility>

namespace nrpc {
namespace {

BackendError posix_error(std::uint32_t code, std::string_view operation,
                         int error = errno) {
    return {code, std::string(operation) + " failed: " + std::strerror(error)};
}

std::expected<speed_t, BackendError> native_baud(std::uint32_t baud) {
#define NRPC_BAUD(value) case value: return B##value
    switch (baud) {
        NRPC_BAUD(50); NRPC_BAUD(75); NRPC_BAUD(110); NRPC_BAUD(134);
        NRPC_BAUD(150); NRPC_BAUD(200); NRPC_BAUD(300); NRPC_BAUD(600);
        NRPC_BAUD(1200); NRPC_BAUD(1800); NRPC_BAUD(2400); NRPC_BAUD(4800);
        NRPC_BAUD(9600); NRPC_BAUD(19200); NRPC_BAUD(38400);
#ifdef B57600
        NRPC_BAUD(57600);
#endif
#ifdef B115200
        NRPC_BAUD(115200);
#endif
#ifdef B230400
        NRPC_BAUD(230400);
#endif
#ifdef B460800
        NRPC_BAUD(460800);
#endif
#ifdef B500000
        NRPC_BAUD(500000);
#endif
#ifdef B576000
        NRPC_BAUD(576000);
#endif
#ifdef B921600
        NRPC_BAUD(921600);
#endif
#ifdef B1000000
        NRPC_BAUD(1000000);
#endif
#ifdef B1152000
        NRPC_BAUD(1152000);
#endif
#ifdef B1500000
        NRPC_BAUD(1500000);
#endif
#ifdef B2000000
        NRPC_BAUD(2000000);
#endif
#ifdef B2500000
        NRPC_BAUD(2500000);
#endif
#ifdef B3000000
        NRPC_BAUD(3000000);
#endif
#ifdef B3500000
        NRPC_BAUD(3500000);
#endif
#ifdef B4000000
        NRPC_BAUD(4000000);
#endif
        default:
            return std::unexpected(BackendError{
                serial_error::unsupported,
                "baud rate is not supported by this operating system"});
    }
#undef NRPC_BAUD
}

std::expected<void, BackendError> configure(int fd,
                                             const SerialConfig& config) {
    auto speed = native_baud(config.baud_rate);
    if (!speed) return std::unexpected(speed.error());

    termios state{};
    if (tcgetattr(fd, &state) != 0) {
        return std::unexpected(posix_error(
            serial_error::configuration_failed, "tcgetattr"));
    }
    cfmakeraw(&state);
    state.c_cflag |= CLOCAL | CREAD;
    state.c_cflag &= static_cast<tcflag_t>(~static_cast<tcflag_t>(CSIZE));
    switch (config.data_bits) {
        case 5: state.c_cflag |= CS5; break;
        case 6: state.c_cflag |= CS6; break;
        case 7: state.c_cflag |= CS7; break;
        case 8: state.c_cflag |= CS8; break;
        default:
            return std::unexpected(BackendError{
                serial_error::invalid_arguments, "invalid serial data bits"});
    }

    state.c_cflag &= static_cast<tcflag_t>(
        ~static_cast<tcflag_t>(PARENB | PARODD | CSTOPB));
#ifdef CMSPAR
    state.c_cflag &= static_cast<tcflag_t>(~static_cast<tcflag_t>(CMSPAR));
#endif
    switch (config.parity) {
        case SerialParity::none: break;
        case SerialParity::odd: state.c_cflag |= PARENB | PARODD; break;
        case SerialParity::even: state.c_cflag |= PARENB; break;
        case SerialParity::mark:
        case SerialParity::space:
#ifdef CMSPAR
            state.c_cflag |= PARENB | CMSPAR;
            if (config.parity == SerialParity::mark) state.c_cflag |= PARODD;
            break;
#else
            return std::unexpected(BackendError{
                serial_error::unsupported,
                "mark and space parity are not supported by this operating system"});
#endif
    }
    if (config.stop_bits == 2) {
        state.c_cflag |= CSTOPB;
    } else if (config.stop_bits != 1) {
        return std::unexpected(BackendError{
            serial_error::invalid_arguments, "invalid serial stop bits"});
    }

    state.c_iflag &= static_cast<tcflag_t>(
        ~static_cast<tcflag_t>(IXON | IXOFF | IXANY));
#ifdef CRTSCTS
    state.c_cflag &= static_cast<tcflag_t>(~static_cast<tcflag_t>(CRTSCTS));
#endif
    switch (config.flow_control) {
        case SerialFlowControl::none: break;
        case SerialFlowControl::software: state.c_iflag |= IXON | IXOFF; break;
        case SerialFlowControl::rts_cts:
#ifdef CRTSCTS
            state.c_cflag |= CRTSCTS;
            break;
#else
            return std::unexpected(BackendError{
                serial_error::unsupported,
                "RTS/CTS flow control is not supported by this operating system"});
#endif
        case SerialFlowControl::dtr_dsr:
            return std::unexpected(BackendError{
                serial_error::unsupported,
                "DTR/DSR flow control is not supported by POSIX termios"});
    }
    state.c_cc[VMIN] = 0;
    state.c_cc[VTIME] = 0;
    if (cfsetispeed(&state, *speed) != 0 ||
        cfsetospeed(&state, *speed) != 0 ||
        tcsetattr(fd, TCSANOW, &state) != 0) {
        return std::unexpected(posix_error(
            serial_error::configuration_failed, "tcsetattr"));
    }
    if (tcflush(fd, TCIOFLUSH) != 0) {
        return std::unexpected(posix_error(
            serial_error::configuration_failed, "tcflush"));
    }
    return {};
}

std::expected<void, BackendError> wait_for(
    int fd, short events, std::uint32_t timeout_ms, std::uint32_t error_code,
    std::string_view operation) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int wait_ms = -1;
        if (timeout_ms != 0) {
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline - clock::now()).count();
            wait_ms = remaining <= 0 ? 0 : static_cast<int>(
                std::min<std::int64_t>(remaining,
                                       std::numeric_limits<int>::max()));
        }
        pollfd descriptor{fd, events, 0};
        const int result = poll(&descriptor, 1, wait_ms);
        if (result > 0) {
            if ((descriptor.revents & events) != 0) return {};
            return std::unexpected(BackendError{
                error_code,
                std::string(operation) + " failed: device disconnected"});
        }
        if (result == 0) {
            return std::unexpected(BackendError{
                serial_error::operation_timeout,
                "serial " + std::string(operation) + " timed out"});
        }
        if (errno != EINTR) {
            return std::unexpected(posix_error(error_code, operation));
        }
    }
}

}  // namespace

struct SerialPort::Impl {
    int fd{-1};
    std::mutex mutex;
};

SerialPort::SerialPort(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
SerialPort::~SerialPort() { close(); }

std::expected<std::unique_ptr<SerialPort>, BackendError> SerialPort::open(
    std::string_view name, const SerialConfig& config) {
    if (name.empty() || name.find('\0') != std::string_view::npos) {
        return std::unexpected(BackendError{
            serial_error::invalid_arguments, "invalid serial port name"});
    }
    const std::string path(name);
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return std::unexpected(posix_error(serial_error::open_failed, "open"));
    }
    auto impl = std::make_unique<Impl>();
    impl->fd = fd;
    auto serial = std::unique_ptr<SerialPort>(new SerialPort(std::move(impl)));
    if (auto result = configure(fd, config); !result) {
        return std::unexpected(result.error());
    }
    return serial;
}

std::expected<std::size_t, BackendError> SerialPort::write(
    std::span<const std::byte> data, std::uint32_t timeout_ms) {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->fd < 0) return std::unexpected(BackendError{
        serial_error::write_failed, "serial port is closed"});
    if (data.empty()) return std::size_t{0};
    auto ready = wait_for(impl_->fd, POLLOUT, timeout_ms,
                          serial_error::write_failed, "write");
    if (!ready) return std::unexpected(ready.error());
    const ssize_t result = ::write(impl_->fd, data.data(), data.size());
    if (result < 0) return std::unexpected(posix_error(
        serial_error::write_failed, "write"));
    return static_cast<std::size_t>(result);
}

std::expected<std::size_t, BackendError> SerialPort::read(
    std::span<std::byte> output, std::uint32_t timeout_ms) {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->fd < 0) return std::unexpected(BackendError{
        serial_error::read_failed, "serial port is closed"});
    if (output.empty()) return std::size_t{0};
    auto ready = wait_for(impl_->fd, POLLIN, timeout_ms,
                          serial_error::read_failed, "read");
    if (!ready) return std::unexpected(ready.error());
    const ssize_t result = ::read(impl_->fd, output.data(), output.size());
    if (result < 0) return std::unexpected(posix_error(
        serial_error::read_failed, "read"));
    if (result == 0) return std::unexpected(BackendError{
        serial_error::read_failed, "serial read failed: device disconnected"});
    return static_cast<std::size_t>(result);
}

void SerialPort::close() noexcept {
    if (impl_ == nullptr) return;
    std::scoped_lock lock(impl_->mutex);
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
}

}  // namespace nrpc
