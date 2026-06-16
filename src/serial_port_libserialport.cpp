#include "nrpc/serial_backend.hpp"

#include <libserialport.h>

#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace nrpc {
namespace {

BackendError serial_failure(std::uint32_t code, std::string_view operation,
                            sp_return result) {
    if (result == SP_ERR_FAIL) {
        char* message = sp_last_error_message();
        std::string detail =
            message == nullptr ? "operating system error" : message;
        if (message != nullptr) {
            sp_free_error_message(message);
        }
        return BackendError{
            code, std::string(operation) + " failed: " + detail};
    }
    if (result == SP_ERR_SUPP) {
        return BackendError{
            serial_error::unsupported,
            std::string(operation) + " is not supported"};
    }
    if (result == SP_ERR_MEM) {
        return BackendError{
            code, std::string(operation) + " failed: out of memory"};
    }
    return BackendError{
        code, std::string(operation) + " failed: invalid arguments"};
}

sp_parity to_native(SerialParity parity) {
    return static_cast<sp_parity>(parity);
}

sp_flowcontrol to_native(SerialFlowControl flow) {
    switch (flow) {
        case SerialFlowControl::none: return SP_FLOWCONTROL_NONE;
        case SerialFlowControl::software: return SP_FLOWCONTROL_XONXOFF;
        case SerialFlowControl::rts_cts: return SP_FLOWCONTROL_RTSCTS;
        case SerialFlowControl::dtr_dsr: return SP_FLOWCONTROL_DTRDSR;
    }
    return SP_FLOWCONTROL_NONE;
}

}  // namespace

struct SerialPort::Impl {
    sp_port* port{nullptr};
    bool opened{false};
    std::mutex mutex;
};

SerialPort::SerialPort(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

SerialPort::~SerialPort() { close(); }

std::expected<std::unique_ptr<SerialPort>, BackendError> SerialPort::open(
    std::string_view name, const SerialConfig& config) {
    sp_port* port = nullptr;
    const std::string port_name(name);
    auto result = sp_get_port_by_name(port_name.c_str(), &port);
    if (result < SP_OK) {
        return std::unexpected(serial_failure(
            serial_error::open_failed, "sp_get_port_by_name", result));
    }
    auto impl = std::make_unique<Impl>();
    impl->port = port;
    auto serial =
        std::unique_ptr<SerialPort>(new SerialPort(std::move(impl)));
    result = sp_open(port, SP_MODE_READ_WRITE);
    if (result < SP_OK) {
        return std::unexpected(serial_failure(
            serial_error::open_failed, "sp_open", result));
    }
    if (config.baud_rate >
        static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(BackendError{
            serial_error::invalid_arguments, "baud rate is too large"});
    }
    serial->impl_->opened = true;
    const auto configure = [&](sp_return setting,
                               std::string_view operation)
        -> std::expected<void, BackendError> {
        if (setting < SP_OK) {
            return std::unexpected(serial_failure(
                serial_error::configuration_failed, operation, setting));
        }
        return {};
    };
    if (auto setting = configure(
            sp_set_baudrate(port, static_cast<int>(config.baud_rate)),
            "sp_set_baudrate");
        !setting) {
        return std::unexpected(setting.error());
    }
    if (auto setting = configure(
            sp_set_bits(port, static_cast<int>(config.data_bits)),
            "sp_set_bits");
        !setting) {
        return std::unexpected(setting.error());
    }
    if (auto setting = configure(
            sp_set_parity(port, to_native(config.parity)),
            "sp_set_parity");
        !setting) {
        return std::unexpected(setting.error());
    }
    if (auto setting = configure(
            sp_set_stopbits(port, static_cast<int>(config.stop_bits)),
            "sp_set_stopbits");
        !setting) {
        return std::unexpected(setting.error());
    }
    if (auto setting = configure(
            sp_set_flowcontrol(port, to_native(config.flow_control)),
            "sp_set_flowcontrol");
        !setting) {
        return std::unexpected(setting.error());
    }
    return serial;
}

std::expected<std::size_t, BackendError> SerialPort::write(
    std::span<const std::byte> data, std::uint32_t timeout_ms) {
    std::scoped_lock lock(impl_->mutex);
    const auto result = sp_blocking_write(
        impl_->port, data.data(), data.size(), timeout_ms);
    if (result < SP_OK) {
        return std::unexpected(serial_failure(
            serial_error::write_failed, "sp_blocking_write", result));
    }
    if (result == 0 && !data.empty() && timeout_ms != 0) {
        return std::unexpected(BackendError{
            serial_error::operation_timeout, "serial write timed out"});
    }
    return static_cast<std::size_t>(result);
}

std::expected<std::size_t, BackendError> SerialPort::read(
    std::span<std::byte> output, std::uint32_t timeout_ms) {
    std::scoped_lock lock(impl_->mutex);
    const auto result = sp_blocking_read_next(
        impl_->port, output.data(), output.size(), timeout_ms);
    if (result < SP_OK) {
        return std::unexpected(serial_failure(
            serial_error::read_failed, "sp_blocking_read_next", result));
    }
    if (result == 0 && timeout_ms != 0) {
        return std::unexpected(BackendError{
            serial_error::operation_timeout, "serial read timed out"});
    }
    return static_cast<std::size_t>(result);
}

void SerialPort::close() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    if (impl_->port != nullptr) {
        if (impl_->opened) {
            sp_close(impl_->port);
        }
        sp_free_port(impl_->port);
        impl_->port = nullptr;
        impl_->opened = false;
    }
}

}  // namespace nrpc
