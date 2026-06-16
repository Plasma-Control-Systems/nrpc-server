#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <expected>
#include <string>
#include <string_view>

#include "nrpc/serial_backend.hpp"

namespace nrpc::win32_serial {

std::expected<std::wstring, BackendError> device_path(
    std::string_view name);
void configure_state(DCB& state, const SerialConfig& config);

}  // namespace nrpc::win32_serial
