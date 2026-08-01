#pragma once

#include <cstdint>

namespace nrpc::detail {

[[nodiscard]] constexpr bool lcard_supports_internal_correction(
    std::uint32_t board_type) noexcept {
    // LComp implements EnableCorrection for the L-761/L-780/L-783 family,
    // E14-440, and E20-10M (E2010B). E20-10 (E2010) explicitly reports
    // L_NOTSUPPORTED, while the remaining boards use the base implementation.
    return board_type == 5U || board_type == 6U || board_type == 14U ||
           board_type == 30U || board_type == 39U;
}

}  // namespace nrpc::detail
