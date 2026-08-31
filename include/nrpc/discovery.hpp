#pragma once

#include <string_view>

#include "nrpc/dispatcher.hpp"

namespace nrpc {

inline constexpr std::uint8_t discovery_function_index = 2;

bool register_discovery_handler(
    Dispatcher& dispatcher, std::string_view platform,
    std::string_view architecture, std::string_view server_version);

}  // namespace nrpc
