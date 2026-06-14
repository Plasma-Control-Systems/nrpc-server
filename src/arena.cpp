#include "nrpc/arena.hpp"

#include <bit>
#include <cstdint>

namespace nrpc {

Arena::Arena(std::size_t capacity) : storage_(capacity) {}

std::expected<std::span<std::byte>, ArenaError> Arena::allocate(
    std::size_t size, std::size_t alignment) {
    if (!std::has_single_bit(alignment)) {
        return std::unexpected(ArenaError::invalid_alignment);
    }
    const auto base = reinterpret_cast<std::uintptr_t>(storage_.data());
    const auto current = base + offset_;
    const auto aligned = (current + alignment - 1U) & ~(alignment - 1U);
    const auto padding = static_cast<std::size_t>(aligned - current);
    if (padding > storage_.size() - offset_ || size > storage_.size() - offset_ - padding) {
        return std::unexpected(ArenaError::out_of_memory);
    }
    offset_ += padding;
    auto result = std::span<std::byte>(storage_).subspan(offset_, size);
    offset_ += size;
    return result;
}

void Arena::reset() noexcept { offset_ = 0; }

std::size_t Arena::capacity() const noexcept { return storage_.size(); }

std::size_t Arena::used() const noexcept { return offset_; }

}  // namespace nrpc
