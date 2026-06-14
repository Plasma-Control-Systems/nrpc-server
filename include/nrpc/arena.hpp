#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <vector>

namespace nrpc {

enum class ArenaError { out_of_memory, invalid_alignment };

class Arena {
public:
    explicit Arena(std::size_t capacity);

    [[nodiscard]] std::expected<std::span<std::byte>, ArenaError> allocate(
        std::size_t size, std::size_t alignment = alignof(std::max_align_t));
    void reset() noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t used() const noexcept;

private:
    std::vector<std::byte> storage_;
    std::size_t offset_{0};
};

}  // namespace nrpc

