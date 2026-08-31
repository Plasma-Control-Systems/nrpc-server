#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "nrpc/gpio_backend.hpp"
#include "nrpc/error_codes.hpp"

namespace {

std::vector<std::byte> integer(std::uint64_t value, std::size_t size) {
    std::vector<std::byte> output(size);
    for (std::size_t index = 0; index < size; ++index) {
        output[index] = static_cast<std::byte>(
            (value >> ((size - index - 1U) * 8U)) & 0xffU);
    }
    return output;
}

std::vector<std::byte> text(std::string value) {
    std::vector<std::byte> output;
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(character));
    }
    output.push_back(std::byte{0});
    return output;
}

nrpc::protocol::Variable variable(nrpc::protocol::DataType type,
                                  std::uint32_t count,
                                  std::vector<std::byte> data) {
    return {type, count, std::move(data)};
}

std::uint64_t decode(std::span<const std::byte> data) {
    std::uint64_t value{};
    for (const auto byte : data) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
    }
    return value;
}

class FakeLine final : public nrpc::GpioLine {
public:
    std::expected<bool, nrpc::BackendError> read() override { return value; }
    std::expected<void, nrpc::BackendError> write(bool next) override {
        value = next;
        return {};
    }
    void close() noexcept override { closed = true; }

    bool value{};
    bool closed{};
};

class FakeDriver final : public nrpc::GpioDriver {
public:
    std::expected<std::unique_ptr<nrpc::GpioLine>, nrpc::BackendError> open(
        std::string_view received_chip, std::uint32_t received_offset,
        nrpc::GpioDirection received_direction, bool received_initial,
        bool received_active_low, std::string_view received_consumer) override {
        chip = received_chip;
        offset = received_offset;
        direction = received_direction;
        initial = received_initial;
        active_low = received_active_low;
        consumer = received_consumer;
        auto result = std::make_unique<FakeLine>();
        result->value = initial;
        line = result.get();
        return result;
    }

    std::string chip;
    std::uint32_t offset{};
    nrpc::GpioDirection direction{};
    bool initial{};
    bool active_low{};
    std::string consumer;
    FakeLine* line{};
};

class OtherHandle {
public:
    OtherHandle(const OtherHandle&) = delete;
    OtherHandle& operator=(const OtherHandle&) = delete;
    OtherHandle() = default;
    void close() noexcept {}
};

}  // namespace

TEST(GpioBackend, OpensWritesReadsAndClosesLine) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher;
    nrpc::register_gpio_backend(dispatcher, driver);
    nrpc::ResourceManager resources;
    nrpc::Arena arena(128);
    const nrpc::RequestContext context{7, 1000, resources};
    const std::vector open_arguments{
        variable(nrpc::protocol::DataType::string, 15,
                 text("/dev/gpiochip0")),
        variable(nrpc::protocol::DataType::uint32, 1, integer(17, 4)),
        variable(nrpc::protocol::DataType::uint8, 1, integer(1, 1)),
        variable(nrpc::protocol::DataType::boolean, 1, {std::byte{0}}),
        variable(nrpc::protocol::DataType::boolean, 1, {std::byte{1}}),
        variable(nrpc::protocol::DataType::string, 12, text("nrpc-tests")),
    };

    auto opened = dispatcher.dispatch(nrpc::gpio_function::open, context,
                                      open_arguments, arena);
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->size(), 1U);
    const auto handle = decode(opened->front().data);
    EXPECT_EQ(driver->chip, "/dev/gpiochip0");
    EXPECT_EQ(driver->offset, 17U);
    EXPECT_EQ(driver->direction, nrpc::GpioDirection::output);
    EXPECT_TRUE(driver->active_low);

    arena.reset();
    const std::vector write_arguments{
        variable(nrpc::protocol::DataType::uint64, 1, integer(handle, 8)),
        variable(nrpc::protocol::DataType::boolean, 1, {std::byte{1}}),
    };
    EXPECT_TRUE(dispatcher.dispatch(nrpc::gpio_function::write, context,
                                    write_arguments, arena));
    EXPECT_TRUE(driver->line->value);

    const std::vector handle_argument{
        variable(nrpc::protocol::DataType::uint64, 1, integer(handle, 8))};
    auto read = dispatcher.dispatch(nrpc::gpio_function::read, context,
                                    handle_argument, arena);
    ASSERT_TRUE(read);
    ASSERT_EQ(read->size(), 1U);
    EXPECT_EQ(read->front().type, nrpc::protocol::DataType::boolean);
    EXPECT_EQ(read->front().data[0], std::byte{1});

    EXPECT_TRUE(dispatcher.dispatch(nrpc::gpio_function::close, context,
                                    handle_argument, arena));
    EXPECT_EQ(resources.size(), 0U);
}

TEST(GpioBackend, RejectsInvalidDirectionWithoutOpeningLine) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher;
    nrpc::register_gpio_backend(dispatcher, driver);
    nrpc::ResourceManager resources;
    nrpc::Arena arena(128);
    const nrpc::RequestContext context{9, 0, resources};
    const std::vector arguments{
        variable(nrpc::protocol::DataType::string, 15,
                 text("/dev/gpiochip0")),
        variable(nrpc::protocol::DataType::uint32, 1, integer(1, 4)),
        variable(nrpc::protocol::DataType::uint8, 1, integer(2, 1)),
        variable(nrpc::protocol::DataType::boolean, 1, {std::byte{0}}),
        variable(nrpc::protocol::DataType::boolean, 1, {std::byte{0}}),
        variable(nrpc::protocol::DataType::string, 12, text("nrpc-tests")),
    };

    const auto result = dispatcher.dispatch(nrpc::gpio_function::open,
                                            context, arguments, arena);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, nrpc::gpio_error::invalid_arguments);
    EXPECT_EQ(driver->line, nullptr);
}

TEST(GpioBackend, DoesNotRequestLineWhenArenaIsExhausted) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher;
    nrpc::register_gpio_backend(dispatcher, driver);
    nrpc::ResourceManager resources;
    nrpc::Arena arena(0);
    const nrpc::RequestContext context{3, 0, resources};
    const std::vector arguments{
        variable(nrpc::protocol::DataType::string, 15,
                 text("/dev/gpiochip0")),
        variable(nrpc::protocol::DataType::uint32, 1, integer(1, 4)),
        variable(nrpc::protocol::DataType::uint8, 1, integer(0, 1)),
        variable(nrpc::protocol::DataType::boolean, 1, {std::byte{0}}),
        variable(nrpc::protocol::DataType::boolean, 1, {std::byte{0}}),
        variable(nrpc::protocol::DataType::string, 12, text("nrpc-tests")),
    };

    const auto result = dispatcher.dispatch(nrpc::gpio_function::open,
                                            context, arguments, arena);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, nrpc::error_code::arena_exhausted);
    EXPECT_EQ(driver->line, nullptr);
}

TEST(GpioBackend, CloseDoesNotRemoveAnotherBackendHandle) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher;
    nrpc::register_gpio_backend(dispatcher, driver);
    nrpc::ResourceManager resources;
    nrpc::Arena arena(64);
    const nrpc::RequestContext context{5, 0, resources};
    constexpr std::uint64_t other_handle = 1234;
    ASSERT_TRUE(resources.register_handle(
        context.client_id, other_handle, std::make_unique<OtherHandle>()));
    const std::vector arguments{
        variable(nrpc::protocol::DataType::uint64, 1,
                 integer(other_handle, 8))};

    const auto result = dispatcher.dispatch(nrpc::gpio_function::close,
                                            context, arguments, arena);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, nrpc::gpio_error::invalid_handle);
    EXPECT_EQ(resources.size(), 1U);
}
