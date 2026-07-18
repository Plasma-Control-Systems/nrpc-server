#include <gtest/gtest.h>

#include <bit>
#include <memory>
#include <string>
#include <vector>

#include "nrpc/lcard_backend.hpp"

namespace {

std::vector<std::byte> integer(std::uint64_t value, std::size_t width) {
    std::vector<std::byte> output(width);
    for (std::size_t i = 0; i < width; ++i)
        output[i] = static_cast<std::byte>(
            (value >> ((width - i - 1U) * 8U)) & 0xffU);
    return output;
}
std::vector<std::byte> floating(double value) {
    return integer(std::bit_cast<std::uint64_t>(value), 8);
}
nrpc::protocol::Variable variable(nrpc::protocol::DataType type,
                                  std::uint32_t count,
                                  std::vector<std::byte> data) {
    return {type, count, std::move(data)};
}
std::vector<std::byte> string_data(std::string value) {
    std::vector<std::byte> output;
    for (char byte : value) output.push_back(static_cast<std::byte>(byte));
    output.push_back(std::byte{0});
    return output;
}
std::uint64_t decode(std::span<const std::byte> data) {
    std::uint64_t output{};
    for (auto byte : data)
        output = (output << 8U) | std::to_integer<std::uint64_t>(byte);
    return output;
}

class FakeDevice final : public nrpc::LcardDevice {
public:
    const nrpc::LcardDeviceInfo& info() const noexcept override { return info_; }
    std::expected<std::int16_t, nrpc::BackendError> read_adc(
        const nrpc::LcardAdcChannel& channel, double timeout) override {
        last_channels = {channel}; last_timeout = timeout; return -123;
    }
    std::expected<nrpc::LcardAdcFrame, nrpc::BackendError> read_adc_frame(
        std::span<const nrpc::LcardAdcChannel> channels,
        std::uint32_t sample_count, double rate, double timeout) override {
        last_channels.assign(channels.begin(), channels.end());
        last_sample_count = sample_count;
        last_rate = rate; last_timeout = timeout;
        std::vector<std::int16_t> codes(sample_count);
        for (std::size_t i = 0; i < codes.size(); ++i)
            codes[i] = static_cast<std::int16_t>(i);
        return nrpc::LcardAdcFrame{std::move(codes), 99.5};
    }
    std::expected<void, nrpc::BackendError> write_dac(
        std::uint8_t channel, std::uint16_t code, double timeout) override {
        dac_channel = channel; dac_code = code; last_timeout = timeout; return {};
    }
    std::expected<nrpc::LcardDacWriteResult, nrpc::BackendError> write_dac_frames(
        std::span<const std::uint16_t> first,
        std::span<const std::uint16_t> second, double rate,
        double timeout) override {
        dac0.assign(first.begin(), first.end()); dac1.assign(second.begin(), second.end());
        last_rate = rate; last_timeout = timeout;
        return nrpc::LcardDacWriteResult{static_cast<std::uint32_t>(first.size()), 12.25};
    }
    void close() noexcept override { closed = true; }

    nrpc::LcardDeviceInfo info_{19, "L791", 'C', true, 15};
    std::vector<nrpc::LcardAdcChannel> last_channels;
    std::vector<std::uint16_t> dac0, dac1;
    double last_rate{}, last_timeout{};
    std::uint32_t last_sample_count{};
    std::uint8_t dac_channel{};
    std::uint16_t dac_code{};
    bool closed{};
};

class FakeDriver final : public nrpc::LcardDriver {
public:
    std::expected<std::unique_ptr<nrpc::LcardDevice>, nrpc::BackendError> open(
        std::uint32_t received_slot, std::string_view received_firmware) override {
        slot = received_slot; firmware = received_firmware;
        auto result = std::make_unique<FakeDevice>();
        last = result.get();
        return result;
    }
    std::uint32_t slot{};
    std::string firmware;
    FakeDevice* last{};
};

std::vector<nrpc::protocol::Variable> channel_arrays(
    std::initializer_list<std::uint32_t> channels) {
    std::vector<std::byte> raw(channels.size(), std::byte{0});
    std::vector<std::byte> channel_data;
    std::vector<std::byte> ranges;
    std::vector<std::byte> modes(channels.size(), std::byte{0});
    for (const auto channel : channels) {
        auto encoded = integer(channel, 4);
        channel_data.insert(channel_data.end(), encoded.begin(), encoded.end());
        auto range = floating(10.0);
        ranges.insert(ranges.end(), range.begin(), range.end());
    }
    const auto count = static_cast<std::uint32_t>(channels.size());
    return {
        variable(nrpc::protocol::DataType::boolean, count, std::move(raw)),
        variable(nrpc::protocol::DataType::uint32, count, std::move(channel_data)),
        variable(nrpc::protocol::DataType::float64, count, std::move(ranges)),
        variable(nrpc::protocol::DataType::uint8, count, std::move(modes)),
    };
}

}  // namespace

TEST(LcardBackend, OpensReportsMetadataReadsFrameAndCloses) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher; nrpc::register_lcard_backend(dispatcher, driver);
    nrpc::ResourceManager resources; nrpc::Arena arena(4096);
    const nrpc::RequestContext context{7, 2500, resources};
    const std::vector open_args{
        variable(nrpc::protocol::DataType::uint32, 1, integer(3, 4)),
        variable(nrpc::protocol::DataType::string, 10, string_data("L791.bio")),
    };
    auto opened = dispatcher.dispatch(nrpc::lcard_function::open, context,
                                      open_args, arena);
    ASSERT_TRUE(opened); ASSERT_EQ(opened->size(), 6U);
    EXPECT_EQ(driver->slot, 3U); EXPECT_EQ(driver->firmware, "L791.bio");
    const auto handle = decode(opened->front().data);

    arena.reset();
    std::vector frame_args{
        variable(nrpc::protocol::DataType::uint64, 1, integer(handle, 8))};
    auto channels = channel_arrays({0, 1});
    frame_args.insert(frame_args.end(), channels.begin(), channels.end());
    frame_args.push_back(variable(nrpc::protocol::DataType::float64, 1,
                                  floating(100.0)));
    frame_args.push_back(variable(nrpc::protocol::DataType::uint32, 1,
                                  integer(8, 4)));
    auto frame = dispatcher.dispatch(nrpc::lcard_function::read_adc_frame,
                                     context, frame_args, arena);
    ASSERT_TRUE(frame); ASSERT_EQ(frame->size(), 2U);
    EXPECT_EQ(frame->front().element_count, 8U);
    EXPECT_EQ(driver->last->last_sample_count, 8U);
    EXPECT_DOUBLE_EQ(driver->last->last_timeout, 2.5);
    EXPECT_DOUBLE_EQ(driver->last->last_rate, 100.0);

    auto incomplete_args = frame_args;
    incomplete_args.back() = variable(nrpc::protocol::DataType::uint32, 1,
                                      integer(7, 4));
    auto incomplete = dispatcher.dispatch(nrpc::lcard_function::read_adc_frame,
                                          context, incomplete_args, arena);
    ASSERT_FALSE(incomplete);
    EXPECT_EQ(incomplete.error().code, nrpc::lcard_error::invalid_arguments);

    const std::vector close_args{
        variable(nrpc::protocol::DataType::uint64, 1, integer(handle, 8))};
    EXPECT_TRUE(dispatcher.dispatch(nrpc::lcard_function::close, context,
                                    close_args, arena));
    EXPECT_EQ(resources.size(), 0U);
}

TEST(LcardBackend, WritesTwoChannelDacFrames) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher; nrpc::register_lcard_backend(dispatcher, driver);
    nrpc::ResourceManager resources; nrpc::Arena arena(4096);
    const nrpc::RequestContext context{4, 1000, resources};
    const std::vector open_args{
        variable(nrpc::protocol::DataType::uint32, 1, integer(0, 4)),
        variable(nrpc::protocol::DataType::string, 1, string_data("")),
    };
    auto opened = dispatcher.dispatch(nrpc::lcard_function::open, context,
                                      open_args, arena);
    ASSERT_TRUE(opened); const auto handle = decode(opened->front().data);
    std::vector<std::byte> first = integer(1, 2), second = integer(2, 2);
    first.insert(first.end(), second.begin(), second.end());
    std::vector<std::byte> third = integer(3, 2), fourth = integer(4, 2);
    third.insert(third.end(), fourth.begin(), fourth.end());
    const std::vector args{
        variable(nrpc::protocol::DataType::uint64, 1, integer(handle, 8)),
        variable(nrpc::protocol::DataType::uint16, 2, std::move(first)),
        variable(nrpc::protocol::DataType::uint16, 2, std::move(third)),
        variable(nrpc::protocol::DataType::float64, 1, floating(12.5)),
    };
    arena.reset();
    auto written = dispatcher.dispatch(nrpc::lcard_function::write_dac_frames,
                                       context, args, arena);
    ASSERT_TRUE(written); EXPECT_EQ(decode(written->front().data), 2U);
    EXPECT_EQ(driver->last->dac0, (std::vector<std::uint16_t>{1, 2}));
    EXPECT_EQ(driver->last->dac1, (std::vector<std::uint16_t>{3, 4}));
}
