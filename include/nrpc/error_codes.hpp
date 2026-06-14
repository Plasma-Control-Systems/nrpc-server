#pragma once

#include <cstdint>

namespace nrpc::error_code {

inline constexpr std::uint32_t protocol_error = 1001;
inline constexpr std::uint32_t timeout = 1002;
inline constexpr std::uint32_t arena_exhausted = 1003;
inline constexpr std::uint32_t internal_error = 1004;

}  // namespace nrpc::error_code

