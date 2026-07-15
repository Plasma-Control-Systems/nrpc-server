#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "nrpc/lcard_backend.hpp"

namespace {

std::vector<std::byte> u32(std::uint32_t value) {
    return {
        static_cast<std::byte>((value >> 24U) & 0xFFU),
        static_cast<std::byte>((value >> 16U) & 0xFFU),
        static_cast<std::byte>((value >> 8U) & 0xFFU),
        static_cast<std::byte>(value & 0xFFU),
    };
}

std::vector<std::byte> u64(std::uint64_t value) {
    std::vector<std::byte> output(8);
    for (std::size_t index = 0; index < output.size(); ++index) {
        output[index] = static_cast<std::byte>(
            (value >> ((output.size() - index - 1U) * 8U)) & 0xFFU);
    }
    return output;
}

std::vector<std::byte> f64(double value) {
    return u64(std::bit_cast<std::uint64_t>(value));
}

std::vector<std::byte> text(std::string value) {
    std::vector<std::byte> output;
    for (const auto character : value) {
        output.push_back(static_cast<std::byte>(character));
    }
    output.push_back(std::byte{0});
    return output;
}

class FakeSession final : public nrpc::LCardSession {
public:
    explicit FakeSession(nrpc::LCardBoardType board) : board(board) {}

    std::expected<nrpc::LCardSlotInfo, nrpc::BackendError> slot_info() override {
        return nrpc::LCardSlotInfo{
            static_cast<std::uint32_t>(board), 23, 5};
    }

    std::expected<void, nrpc::BackendError> load_bios(
        std::string_view path) override {
        bios = path;
        return {};
    }

    std::expected<nrpc::LCardAsyncResult, nrpc::BackendError> io_async(
        const nrpc::LCardAsyncParameters& parameters) override {
        operation = parameters.operation;
        mode = parameters.mode;
        channel_count = parameters.channel_count;
        channels.assign(parameters.channels.begin(), parameters.channels.end());
        nrpc::LCardAsyncResult result{
            parameters.rate / 2.0, parameters.channel_count, {}};
        result.data[0] = 101;
        result.data[1] = 202;
        return result;
    }

    std::expected<nrpc::LCardAdcFrameResult, nrpc::BackendError> adc_frame(
        const nrpc::LCardAdcFrameParameters& parameters) override {
        frame_channel_count = parameters.channel_count;
        frame_rate = parameters.rate;
        frame_samples_per_channel = parameters.samples_per_channel;
        frame_start_channel = parameters.start_channel;
        frame_end_channel = parameters.end_channel;
        frame_mode = parameters.mode;
        frame_auto_init = parameters.auto_init;
        nrpc::LCardAdcFrameResult result{
            parameters.rate / 2.0,
            parameters.channel_count,
            parameters.samples_per_channel,
            {},
        };
        result.data.resize(
            parameters.channel_count * parameters.samples_per_channel);
        for (std::size_t index = 0; index < result.data.size(); ++index) {
            result.data[index] = static_cast<std::uint32_t>(100 + index);
        }
        return result;
    }

    void close() noexcept override {}

    std::string bios;
    nrpc::LCardBoardType board;
    nrpc::LCardAsyncOperation operation{};
    std::uint32_t mode{};
    std::uint32_t channel_count{};
    std::vector<std::uint32_t> channels;
    double frame_rate{};
    std::uint32_t frame_channel_count{};
    std::uint32_t frame_samples_per_channel{};
    std::uint32_t frame_start_channel{};
    std::uint32_t frame_end_channel{};
    std::uint32_t frame_mode{};
    bool frame_auto_init{};
};

class FakeDriver final : public nrpc::LCardDriver {
public:
    std::expected<std::unique_ptr<nrpc::LCardSession>, nrpc::BackendError> open(
        std::uint32_t received_slot,
        nrpc::LCardBoardType received_board,
        std::string_view bios_path) override {
        slot = received_slot;
        board = received_board;
        bios = bios_path;
        auto session = std::make_unique<FakeSession>(received_board);
        last_session = session.get();
        return session;
    }

    std::uint32_t slot{};
    nrpc::LCardBoardType board{};
    std::string bios;
    FakeSession* last_session{};
};

std::uint64_t decode_u64(std::span<const std::byte> data) {
    std::uint64_t value = 0;
    for (const auto byte : data) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
    }
    return value;
}

}  // namespace

TEST(LCardBackend, OpensRunsAsyncIoReportsSlotAndCloses) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher;
    nrpc::register_lcard_backend(dispatcher, driver);
    nrpc::ResourceManager resources;
    nrpc::Arena arena(4096);
    const nrpc::RequestContext context{11, 500, resources};

    const auto slot = u32(2);
    const auto board =
        u32(static_cast<std::uint32_t>(nrpc::LCardBoardType::e140));
    const auto bios = text("l761");
    const std::array open_arguments{
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, slot},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, board},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::string,
            static_cast<std::uint32_t>(bios.size()),
            bios},
    };
    const auto opened = dispatcher.dispatch(
        nrpc::lcard_function::open, context, open_arguments, arena);
    ASSERT_TRUE(opened.has_value());
    ASSERT_EQ(opened->size(), 1U);
    const auto handle = decode_u64(opened->front().data);
    EXPECT_EQ(driver->slot, 2U);
    EXPECT_EQ(driver->board, nrpc::LCardBoardType::e140);
    EXPECT_EQ(driver->bios, "l761");

    arena.reset();
    const auto handle_data = u64(handle);
    const auto operation =
        u32(static_cast<std::uint32_t>(
            nrpc::LCardAsyncOperation::adc_input));
    const auto rate = f64(2000.0);
    const auto mode = u32(9);
    const auto count = u32(1);
    const auto channels = u32(3);
    const std::vector<std::byte> no_data;
    const std::array io_arguments{
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint64, 1, handle_data},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, operation},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::float64, 1, rate},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, mode},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, count},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, channels},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 0, no_data},
    };
    const auto io_result = dispatcher.dispatch(
        nrpc::lcard_function::io_async, context, io_arguments, arena);
    ASSERT_TRUE(io_result.has_value());
    ASSERT_EQ(io_result->size(), 2U);
    EXPECT_EQ(driver->last_session->operation,
              nrpc::LCardAsyncOperation::adc_input);
    EXPECT_EQ(driver->last_session->mode, 9U);
    EXPECT_EQ(driver->last_session->channel_count, 1U);
    EXPECT_EQ(
        driver->last_session->channels,
        (std::vector<std::uint32_t>{3}));

    arena.reset();
    const std::array handle_argument{
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint64, 1, handle_data},
    };
    const auto info = dispatcher.dispatch(
        nrpc::lcard_function::slot_info, context, handle_argument, arena);
    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->size(), 1U);
    EXPECT_EQ(info->front().element_count, 3U);

    arena.reset();
    const auto closed = dispatcher.dispatch(
        nrpc::lcard_function::close, context, handle_argument, arena);
    ASSERT_TRUE(closed.has_value());
    const auto closed_again = dispatcher.dispatch(
        nrpc::lcard_function::close, context, handle_argument, arena);
    ASSERT_FALSE(closed_again.has_value());
    EXPECT_EQ(closed_again.error().code, nrpc::lcard_error::invalid_handle);
}

TEST(LCardBackend, RunsAdcFrameWithExplicitShape) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher;
    nrpc::register_lcard_backend(dispatcher, driver);
    nrpc::ResourceManager resources;
    nrpc::Arena arena(4096);
    const nrpc::RequestContext context{11, 500, resources};

    const auto slot = u32(0);
    const auto board =
        u32(static_cast<std::uint32_t>(nrpc::LCardBoardType::e440));
    const auto bios = text("");
    const std::array open_arguments{
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, slot},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, board},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::string,
            static_cast<std::uint32_t>(bios.size()),
            bios},
    };
    const auto opened = dispatcher.dispatch(
        nrpc::lcard_function::open, context, open_arguments, arena);
    ASSERT_TRUE(opened.has_value());
    const auto handle = decode_u64(opened->front().data);

    arena.reset();
    const auto handle_data = u64(handle);
    const auto channel_count = u32(2);
    const auto rate = f64(400.0);
    const auto samples = u32(3);
    const auto start_channel = u32(1);
    const auto end_channel = u32(2);
    const auto mode = u32(32);
    const std::vector<std::byte> auto_init{std::byte{1}};
    const std::array frame_arguments{
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint64, 1, handle_data},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, channel_count},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::float64, 1, rate},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, samples},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, start_channel},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, end_channel},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, mode},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::boolean, 1, auto_init},
    };
    const auto frame_result = dispatcher.dispatch(
        nrpc::lcard_function::adc_frame, context, frame_arguments, arena);
    ASSERT_TRUE(frame_result.has_value());
    ASSERT_EQ(frame_result->size(), 4U);
    EXPECT_EQ(driver->last_session->frame_rate, 400.0);
    EXPECT_EQ(driver->last_session->frame_channel_count, 2U);
    EXPECT_EQ(driver->last_session->frame_samples_per_channel, 3U);
    EXPECT_EQ(driver->last_session->frame_start_channel, 1U);
    EXPECT_EQ(driver->last_session->frame_end_channel, 2U);
    EXPECT_EQ(driver->last_session->frame_mode, 32U);
    EXPECT_TRUE(driver->last_session->frame_auto_init);
    EXPECT_EQ((*frame_result)[3].element_count, 6U);
}

TEST(LCardBackend, RejectsInvalidAsyncOperationBeforeCallingDevice) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher;
    nrpc::register_lcard_backend(dispatcher, driver);
    nrpc::ResourceManager resources;
    nrpc::Arena arena(1024);
    const nrpc::RequestContext context{1, 0, resources};

    const auto handle = u64(99);
    const auto operation = u32(99);
    const auto rate = f64(0);
    const auto mode = u32(0);
    const auto count = u32(0);
    const std::vector<std::byte> empty;
    const std::array arguments{
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint64, 1, handle},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, operation},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::float64, 1, rate},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, mode},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 1, count},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 0, empty},
        nrpc::protocol::Variable{
            nrpc::protocol::DataType::uint32, 0, empty},
    };
    const auto result = dispatcher.dispatch(
        nrpc::lcard_function::io_async, context, arguments, arena);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, nrpc::lcard_error::invalid_arguments);
}
