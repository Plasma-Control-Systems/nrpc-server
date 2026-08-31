#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "nrpc/error_codes.hpp"
#include "nrpc/spi_backend.hpp"

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
    for (const char character : value)
        output.push_back(static_cast<std::byte>(character));
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
    for (const auto byte : data)
        value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
    return value;
}

class FakeDevice final : public nrpc::SpiDevice {
public:
    std::expected<std::vector<std::byte>, nrpc::BackendError> transfer(
        std::span<const std::byte> transmit,
        const nrpc::SpiTransferOptions& received_options) override {
        sent.assign(transmit.begin(), transmit.end());
        options = received_options;
        auto received = sent;
        for (auto& byte : received) byte ^= std::byte{0xff};
        return received;
    }

    void close() noexcept override { closed = true; }

    std::vector<std::byte> sent;
    nrpc::SpiTransferOptions options;
    bool closed{};
};

class FakeDriver final : public nrpc::SpiDriver {
public:
    std::expected<std::unique_ptr<nrpc::SpiDevice>, nrpc::BackendError> open(
        std::string_view received_device, std::uint8_t received_mode,
        std::uint32_t received_speed, std::uint8_t received_bits,
        bool received_lsb_first) override {
        device_path = received_device;
        mode = received_mode;
        speed = received_speed;
        bits = received_bits;
        lsb_first = received_lsb_first;
        auto result = std::make_unique<FakeDevice>();
        device = result.get();
        return result;
    }

    std::string device_path;
    std::uint8_t mode{};
    std::uint32_t speed{};
    std::uint8_t bits{};
    bool lsb_first{};
    FakeDevice* device{};
};

std::vector<nrpc::protocol::Variable> open_arguments(std::uint8_t mode = 3) {
    return {
        variable(nrpc::protocol::DataType::string, 14, text("/dev/spidev0.0")),
        variable(nrpc::protocol::DataType::uint8, 1, integer(mode, 1)),
        variable(nrpc::protocol::DataType::uint32, 1, integer(1'000'000, 4)),
        variable(nrpc::protocol::DataType::uint8, 1, integer(8, 1)),
        variable(nrpc::protocol::DataType::boolean, 1, {std::byte{0}}),
    };
}

}  // namespace

TEST(SpiBackend, OpensTransfersAndClosesDevice) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher;
    nrpc::register_spi_backend(dispatcher, driver);
    nrpc::ResourceManager resources;
    nrpc::Arena arena(128);
    const nrpc::RequestContext context{7, 1000, resources};

    auto opened = dispatcher.dispatch(nrpc::spi_function::open, context,
                                      open_arguments(), arena);
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->size(), 1U);
    const auto handle = decode(opened->front().data);
    EXPECT_EQ(driver->device_path, "/dev/spidev0.0");
    EXPECT_EQ(driver->mode, 3U);
    EXPECT_EQ(driver->speed, 1'000'000U);
    EXPECT_EQ(driver->bits, 8U);

    arena.reset();
    const std::vector transfer_arguments{
        variable(nrpc::protocol::DataType::uint64, 1, integer(handle, 8)),
        variable(nrpc::protocol::DataType::bytes, 3,
                 {std::byte{0x9f}, std::byte{0}, std::byte{0}}),
        variable(nrpc::protocol::DataType::uint32, 1, integer(500'000, 4)),
        variable(nrpc::protocol::DataType::uint16, 1, integer(10, 2)),
        variable(nrpc::protocol::DataType::boolean, 1, {std::byte{1}}),
    };
    auto transferred = dispatcher.dispatch(nrpc::spi_function::transfer,
                                           context, transfer_arguments, arena);
    ASSERT_TRUE(transferred);
    ASSERT_EQ(transferred->size(), 1U);
    EXPECT_EQ(transferred->front().type, nrpc::protocol::DataType::bytes);
    EXPECT_EQ(transferred->front().data[0], std::byte{0x60});
    EXPECT_EQ(driver->device->options.speed_hz, 500'000U);
    EXPECT_EQ(driver->device->options.delay_us, 10U);
    EXPECT_TRUE(driver->device->options.cs_change);

    const std::vector handle_argument{
        variable(nrpc::protocol::DataType::uint64, 1, integer(handle, 8))};
    EXPECT_TRUE(dispatcher.dispatch(nrpc::spi_function::close, context,
                                    handle_argument, arena));
    EXPECT_EQ(resources.size(), 0U);
}

TEST(SpiBackend, RejectsInvalidModeBeforeOpeningDevice) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher;
    nrpc::register_spi_backend(dispatcher, driver);
    nrpc::ResourceManager resources;
    nrpc::Arena arena(64);
    const nrpc::RequestContext context{1, 0, resources};

    const auto result = dispatcher.dispatch(nrpc::spi_function::open, context,
                                            open_arguments(4), arena);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, nrpc::spi_error::invalid_arguments);
    EXPECT_EQ(driver->device, nullptr);
}

TEST(SpiBackend, DoesNotOpenWhenResponseArenaIsExhausted) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher;
    nrpc::register_spi_backend(dispatcher, driver);
    nrpc::ResourceManager resources;
    nrpc::Arena arena(0);
    const nrpc::RequestContext context{1, 0, resources};

    const auto result = dispatcher.dispatch(nrpc::spi_function::open, context,
                                            open_arguments(), arena);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, nrpc::error_code::arena_exhausted);
    EXPECT_EQ(driver->device, nullptr);
}

TEST(SpiBackend, RejectsForeignHandle) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher;
    nrpc::register_spi_backend(dispatcher, driver);
    nrpc::ResourceManager resources;
    nrpc::Arena arena(64);
    const nrpc::RequestContext context{1, 0, resources};
    const std::vector arguments{
        variable(nrpc::protocol::DataType::uint64, 1, integer(1234, 8))};

    const auto result = dispatcher.dispatch(nrpc::spi_function::close, context,
                                            arguments, arena);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, nrpc::spi_error::invalid_handle);
}
