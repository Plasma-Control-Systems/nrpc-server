#include <gtest/gtest.h>

#include <bit>
#include <memory>
#include <string>
#include <vector>

#include "nrpc/nidaqmx_backend.hpp"
#include "nrpc/error_codes.hpp"

namespace {

std::vector<std::byte> integer(std::uint64_t value, std::size_t size) {
    std::vector<std::byte> output(size);
    for (std::size_t i = 0; i < size; ++i)
        output[i] = static_cast<std::byte>((value >> ((size - i - 1U) * 8U)) & 0xffU);
    return output;
}
std::vector<std::byte> f64(double value) { return integer(std::bit_cast<std::uint64_t>(value), 8); }
std::vector<std::byte> text(std::string value) {
    std::vector<std::byte> output;
    for (char c : value) output.push_back(static_cast<std::byte>(c));
    output.push_back(std::byte{0});
    return output;
}
nrpc::protocol::Variable variable(nrpc::protocol::DataType type,
                                  std::uint32_t count,
                                  std::vector<std::byte> data) {
    return {type, count, std::move(data)};
}
std::uint64_t decode(std::span<const std::byte> data) {
    std::uint64_t result{};
    for (auto byte : data) result = (result << 8U) | std::to_integer<std::uint64_t>(byte);
    return result;
}

class FakeTask final : public nrpc::NidaqmxTask {
public:
    explicit FakeTask(nrpc::NidaqmxChannelType type) : type_(type) {}
    nrpc::NidaqmxChannelType channel_type() const noexcept override { return type_; }
    std::expected<void, nrpc::BackendError> start() override { started = true; return {}; }
    std::expected<std::size_t, nrpc::BackendError> read_analog(
        std::span<double> values, std::uint32_t, double timeout) override {
        last_timeout = timeout; values[0] = 21.25; values[1] = 22.5; return 2;
    }
    std::expected<std::size_t, nrpc::BackendError> write_analog(
        std::span<const double> values, double timeout) override {
        analog.assign(values.begin(), values.end()); last_timeout = timeout; return values.size();
    }
    std::expected<std::size_t, nrpc::BackendError> read_digital(
        std::span<std::uint32_t> values, std::uint32_t, double timeout) override {
        last_timeout = timeout; values[0] = 0x5aU; return 1;
    }
    std::expected<std::size_t, nrpc::BackendError> write_digital(
        std::span<const std::uint32_t> values, double timeout) override {
        digital.assign(values.begin(), values.end()); last_timeout = timeout; return values.size();
    }
    void close() noexcept override { closed = true; }
    bool started{};
    bool closed{};
    double last_timeout{};
    std::vector<double> analog;
    std::vector<std::uint32_t> digital;
private:
    nrpc::NidaqmxChannelType type_;
};

class FakeDriver final : public nrpc::NidaqmxDriver {
public:
    std::expected<std::unique_ptr<nrpc::NidaqmxTask>, nrpc::BackendError> create_ai_voltage(
        std::string_view physical, double minimum, double maximum,
        nrpc::NidaqmxTerminalConfig terminal) override {
        channel = physical; min = minimum; max = maximum; terminal_config = terminal;
        return make(nrpc::NidaqmxChannelType::analog_input);
    }
    std::expected<std::unique_ptr<nrpc::NidaqmxTask>, nrpc::BackendError> create_ai_thermocouple(
        std::string_view physical, double minimum, double maximum,
        nrpc::NidaqmxThermocoupleType received_type,
        nrpc::NidaqmxCjcSource source, double value,
        std::string_view received_cjc_channel) override {
        channel = physical; min = minimum; max = maximum; tc_type = received_type;
        cjc_source = source; cjc_value = value; cjc_channel = received_cjc_channel;
        return make(nrpc::NidaqmxChannelType::analog_input);
    }
    std::expected<std::unique_ptr<nrpc::NidaqmxTask>, nrpc::BackendError> create_ao_voltage(
        std::string_view physical, double minimum, double maximum) override {
        channel = physical; min = minimum; max = maximum;
        return make(nrpc::NidaqmxChannelType::analog_output);
    }
    std::expected<std::unique_ptr<nrpc::NidaqmxTask>, nrpc::BackendError> create_digital(
        std::string_view physical, bool received_output, bool received_all_lines) override {
        channel = physical; output = received_output; all_lines = received_all_lines;
        return make(output ? nrpc::NidaqmxChannelType::digital_output
                           : nrpc::NidaqmxChannelType::digital_input);
    }
    std::expected<std::unique_ptr<nrpc::NidaqmxTask>, nrpc::BackendError> make(
        nrpc::NidaqmxChannelType type) {
        auto task = std::make_unique<FakeTask>(type); last_task = task.get(); return task;
    }
    std::string channel, cjc_channel;
    double min{}, max{}, cjc_value{};
    nrpc::NidaqmxTerminalConfig terminal_config{};
    nrpc::NidaqmxThermocoupleType tc_type{};
    nrpc::NidaqmxCjcSource cjc_source{};
    bool output{}, all_lines{};
    FakeTask* last_task{};
};

}  // namespace

TEST(NidaqmxBackend, CreatesThermocoupleStartsReadsAndCloses) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher; nrpc::register_nidaqmx_backend(dispatcher, driver);
    nrpc::ResourceManager resources; nrpc::Arena arena(1024);
    const nrpc::RequestContext context{7, 2500, resources};
    const std::vector args{
        variable(nrpc::protocol::DataType::string, 9, text("Dev1/ai0")),
        variable(nrpc::protocol::DataType::float64, 1, f64(-100.0)),
        variable(nrpc::protocol::DataType::float64, 1, f64(500.0)),
        variable(nrpc::protocol::DataType::int32, 1, integer(10073, 4)),
        variable(nrpc::protocol::DataType::int32, 1, integer(10200, 4)),
        variable(nrpc::protocol::DataType::float64, 1, f64(25.0)),
        variable(nrpc::protocol::DataType::string, 1, text("")),
    };
    auto opened = dispatcher.dispatch(nrpc::nidaqmx_function::create_ai_thermocouple, context, args, arena);
    ASSERT_TRUE(opened); const auto handle = decode(opened->front().data);
    EXPECT_EQ(driver->channel, "Dev1/ai0"); EXPECT_EQ(driver->tc_type, nrpc::NidaqmxThermocoupleType::k);
    const std::vector handle_arg{variable(nrpc::protocol::DataType::uint64, 1, integer(handle, 8))};
    EXPECT_TRUE(dispatcher.dispatch(nrpc::nidaqmx_function::start, context, handle_arg, arena));
    arena.reset(); auto read_args = handle_arg; read_args.push_back(variable(nrpc::protocol::DataType::uint32, 1, integer(2, 4)));
    auto read = dispatcher.dispatch(nrpc::nidaqmx_function::read_analog, context, read_args, arena);
    ASSERT_TRUE(read); EXPECT_EQ(read->front().element_count, 2U); EXPECT_DOUBLE_EQ(driver->last_task->last_timeout, 2.5);
    EXPECT_TRUE(dispatcher.dispatch(nrpc::nidaqmx_function::close, context, handle_arg, arena));
}

TEST(NidaqmxBackend, CreatesDigitalOutputAndWritesValues) {
    auto driver = std::make_shared<FakeDriver>(); nrpc::Dispatcher dispatcher;
    nrpc::register_nidaqmx_backend(dispatcher, driver); nrpc::ResourceManager resources;
    nrpc::Arena arena(1024); const nrpc::RequestContext context{3, 1000, resources};
    const std::vector args{
        variable(nrpc::protocol::DataType::string, 18, text("Dev1/port0/line0")),
        variable(nrpc::protocol::DataType::boolean, 1, {std::byte{1}})};
    auto opened = dispatcher.dispatch(nrpc::nidaqmx_function::create_do, context, args, arena);
    ASSERT_TRUE(opened); auto handle = decode(opened->front().data); arena.reset();
    auto values = integer(1, 4); auto second = integer(0, 4); values.insert(values.end(), second.begin(), second.end());
    const std::vector write_args{
        variable(nrpc::protocol::DataType::uint64, 1, integer(handle, 8)),
        variable(nrpc::protocol::DataType::uint32, 2, values)};
    auto written = dispatcher.dispatch(nrpc::nidaqmx_function::write_digital, context, write_args, arena);
    ASSERT_TRUE(written); EXPECT_EQ(decode(written->front().data), 2U);
    EXPECT_EQ(driver->last_task->digital, (std::vector<std::uint32_t>{1, 0}));
}

TEST(NidaqmxBackend, DoesNotCreateTaskWhenResponseArenaIsExhausted) {
    auto driver = std::make_shared<FakeDriver>();
    nrpc::Dispatcher dispatcher;
    nrpc::register_nidaqmx_backend(dispatcher, driver);
    nrpc::ResourceManager resources;
    nrpc::Arena arena(0);
    const nrpc::RequestContext context{9, 1000, resources};
    const std::vector args{
        variable(nrpc::protocol::DataType::string, 18,
                 text("Dev1/port0/line0")),
        variable(nrpc::protocol::DataType::boolean, 1, {std::byte{1}}),
    };

    const auto opened = dispatcher.dispatch(
        nrpc::nidaqmx_function::create_do, context, args, arena);

    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, nrpc::error_code::arena_exhausted);
    EXPECT_EQ(driver->last_task, nullptr);
    EXPECT_EQ(resources.size(), 0U);
}
