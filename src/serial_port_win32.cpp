#include "nrpc/serial_backend.hpp"
#include "serial_port_win32_internal.hpp"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace nrpc {
namespace {

BackendError windows_error(std::uint32_t code, std::string_view operation,
                           DWORD error = GetLastError()) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string detail = "Windows error " + std::to_string(error);
    if (length != 0 && buffer != nullptr) {
        const int utf8_length = WideCharToMultiByte(
            CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr,
            nullptr);
        if (utf8_length > 0) {
            detail.resize(static_cast<std::size_t>(utf8_length));
            WideCharToMultiByte(
                CP_UTF8, 0, buffer, static_cast<int>(length), detail.data(),
                utf8_length, nullptr, nullptr);
            while (!detail.empty() &&
                   (detail.back() == '\r' || detail.back() == '\n' ||
                    detail.back() == ' ')) {
                detail.pop_back();
            }
        }
        LocalFree(buffer);
    }
    return BackendError{
        code, std::string(operation) + " failed: " + detail};
}

std::expected<std::wstring, BackendError> utf8_to_wide(
    std::string_view value) {
    if (value.empty()) {
        return std::unexpected(BackendError{
            serial_error::invalid_arguments, "serial port name is empty"});
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return std::unexpected(windows_error(
            serial_error::invalid_arguments, "serial port name conversion"));
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), length) <= 0) {
        return std::unexpected(windows_error(
            serial_error::invalid_arguments, "serial port name conversion"));
    }
    return result;
}

bool is_com_name(std::wstring_view name) {
    if (name.size() < 4 || std::towupper(name[0]) != L'C' ||
        std::towupper(name[1]) != L'O' ||
        std::towupper(name[2]) != L'M') {
        return false;
    }
    return std::all_of(
        name.begin() + 3, name.end(),
        [](wchar_t character) { return std::iswdigit(character) != 0; });
}

BYTE native_parity(SerialParity parity) {
    switch (parity) {
        case SerialParity::none: return NOPARITY;
        case SerialParity::odd: return ODDPARITY;
        case SerialParity::even: return EVENPARITY;
        case SerialParity::mark: return MARKPARITY;
        case SerialParity::space: return SPACEPARITY;
    }
    return NOPARITY;
}

void configure_flow_control(DCB& state, SerialFlowControl flow) {
    state.fOutxCtsFlow = FALSE;
    state.fOutxDsrFlow = FALSE;
    state.fDtrControl = DTR_CONTROL_ENABLE;
    state.fDsrSensitivity = FALSE;
    state.fOutX = FALSE;
    state.fInX = FALSE;
    state.fRtsControl = RTS_CONTROL_ENABLE;
    switch (flow) {
        case SerialFlowControl::none:
            break;
        case SerialFlowControl::software:
            state.fOutX = TRUE;
            state.fInX = TRUE;
            break;
        case SerialFlowControl::rts_cts:
            state.fOutxCtsFlow = TRUE;
            state.fRtsControl = RTS_CONTROL_HANDSHAKE;
            break;
        case SerialFlowControl::dtr_dsr:
            state.fOutxDsrFlow = TRUE;
            state.fDtrControl = DTR_CONTROL_HANDSHAKE;
            break;
    }
}

std::expected<void, BackendError> set_read_timeout(
    HANDLE handle, std::uint32_t timeout_ms) {
    COMMTIMEOUTS timeouts{};
    if (timeout_ms == 0) {
        timeouts.ReadIntervalTimeout = 0;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 0;
    } else {
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
        timeouts.ReadTotalTimeoutConstant = timeout_ms;
    }
    if (!SetCommTimeouts(handle, &timeouts)) {
        return std::unexpected(windows_error(
            serial_error::read_failed, "SetCommTimeouts"));
    }
    return {};
}

std::expected<void, BackendError> set_write_timeout(
    HANDLE handle, std::uint32_t timeout_ms) {
    COMMTIMEOUTS timeouts{};
    timeouts.WriteTotalTimeoutConstant = timeout_ms;
    if (!SetCommTimeouts(handle, &timeouts)) {
        return std::unexpected(windows_error(
            serial_error::write_failed, "SetCommTimeouts"));
    }
    return {};
}

}  // namespace

namespace win32_serial {

std::expected<std::wstring, BackendError> device_path(
    std::string_view name) {
    auto wide = utf8_to_wide(name);
    if (!wide) {
        return std::unexpected(wide.error());
    }
    if (is_com_name(*wide)) {
        return std::wstring(L"\\\\.\\") + *wide;
    }
    return wide;
}

void configure_state(DCB& state, const SerialConfig& config) {
    state.BaudRate = config.baud_rate;
    state.ByteSize = config.data_bits;
    state.Parity = native_parity(config.parity);
    state.StopBits =
        config.stop_bits == 2 ? TWOSTOPBITS : ONESTOPBIT;
    state.fBinary = TRUE;
    state.fParity =
        config.parity == SerialParity::none ? FALSE : TRUE;
    state.fAbortOnError = FALSE;
    configure_flow_control(state, config.flow_control);
}

}  // namespace win32_serial

struct SerialPort::Impl {
    HANDLE handle{INVALID_HANDLE_VALUE};
    std::mutex mutex;
};

SerialPort::SerialPort(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

SerialPort::~SerialPort() { close(); }

std::expected<std::unique_ptr<SerialPort>, BackendError> SerialPort::open(
    std::string_view name, const SerialConfig& config) {
    auto path = win32_serial::device_path(name);
    if (!path) {
        return std::unexpected(path.error());
    }
    const HANDLE handle = CreateFileW(
        path->c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(windows_error(
            serial_error::open_failed, "CreateFileW"));
    }
    auto impl = std::make_unique<Impl>();
    impl->handle = handle;
    auto serial =
        std::unique_ptr<SerialPort>(new SerialPort(std::move(impl)));

    DCB state{};
    state.DCBlength = sizeof(state);
    if (!GetCommState(handle, &state)) {
        return std::unexpected(windows_error(
            serial_error::configuration_failed, "GetCommState"));
    }
    win32_serial::configure_state(state, config);
    if (!SetCommState(handle, &state)) {
        return std::unexpected(windows_error(
            serial_error::configuration_failed, "SetCommState"));
    }
    if (!SetupComm(handle, 4096, 4096)) {
        return std::unexpected(windows_error(
            serial_error::configuration_failed, "SetupComm"));
    }
    if (!PurgeComm(
            handle, PURGE_RXABORT | PURGE_RXCLEAR | PURGE_TXABORT |
                        PURGE_TXCLEAR)) {
        return std::unexpected(windows_error(
            serial_error::configuration_failed, "PurgeComm"));
    }
    return serial;
}

std::expected<std::size_t, BackendError> SerialPort::write(
    std::span<const std::byte> data, std::uint32_t timeout_ms) {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(BackendError{
            serial_error::write_failed, "serial port is closed"});
    }
    if (auto timeout = set_write_timeout(impl_->handle, timeout_ms);
        !timeout) {
        return std::unexpected(timeout.error());
    }
    const auto count = static_cast<DWORD>(std::min<std::size_t>(
        data.size(), std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(
            impl_->handle, data.data(), count, &written, nullptr)) {
        return std::unexpected(windows_error(
            serial_error::write_failed, "WriteFile"));
    }
    if (written == 0 && !data.empty() && timeout_ms != 0) {
        return std::unexpected(BackendError{
            serial_error::operation_timeout, "serial write timed out"});
    }
    return static_cast<std::size_t>(written);
}

std::expected<std::size_t, BackendError> SerialPort::read(
    std::span<std::byte> output, std::uint32_t timeout_ms) {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(BackendError{
            serial_error::read_failed, "serial port is closed"});
    }
    if (auto timeout = set_read_timeout(impl_->handle, timeout_ms);
        !timeout) {
        return std::unexpected(timeout.error());
    }
    const auto count = static_cast<DWORD>(std::min<std::size_t>(
        output.size(), std::numeric_limits<DWORD>::max()));
    DWORD received = 0;
    if (!ReadFile(
            impl_->handle, output.data(), count, &received, nullptr)) {
        return std::unexpected(windows_error(
            serial_error::read_failed, "ReadFile"));
    }
    if (received == 0 && timeout_ms != 0) {
        return std::unexpected(BackendError{
            serial_error::operation_timeout, "serial read timed out"});
    }
    return static_cast<std::size_t>(received);
}

void SerialPort::close() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    if (impl_->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->handle);
        impl_->handle = INVALID_HANDLE_VALUE;
    }
}

}  // namespace nrpc
