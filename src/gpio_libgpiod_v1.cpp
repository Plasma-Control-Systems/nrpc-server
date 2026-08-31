#include "nrpc/gpio_backend.hpp"

#include <cerrno>
#include <cstring>
#include <gpiod.h>
#include <string>
#include <utility>

namespace nrpc {
namespace {

BackendError gpio_system_error(std::uint32_t code, std::string operation) {
    return {code, std::move(operation) + " failed: " + std::strerror(errno)};
}

class LibgpiodLine final : public GpioLine {
public:
    LibgpiodLine(gpiod_chip* chip, gpiod_line* line, GpioDirection direction)
        : chip_(chip), line_(line), direction_(direction) {}
    ~LibgpiodLine() override { close(); }

    std::expected<bool, BackendError> read() override {
        const int value = gpiod_line_get_value(line_);
        if (value < 0) {
            return std::unexpected(
                gpio_system_error(gpio_error::read_failed, "GPIO read"));
        }
        return value != 0;
    }

    std::expected<void, BackendError> write(bool value) override {
        if (direction_ != GpioDirection::output) {
            return std::unexpected(BackendError{
                gpio_error::write_failed, "cannot write to a GPIO input"});
        }
        if (gpiod_line_set_value(line_, value ? 1 : 0) < 0) {
            return std::unexpected(
                gpio_system_error(gpio_error::write_failed, "GPIO write"));
        }
        return {};
    }

    void close() noexcept override {
        if (line_ != nullptr) {
            gpiod_line_release(line_);
            line_ = nullptr;
        }
        if (chip_ != nullptr) {
            gpiod_chip_close(chip_);
            chip_ = nullptr;
        }
    }

private:
    gpiod_chip* chip_{};
    gpiod_line* line_{};
    GpioDirection direction_;
};

class LibgpiodDriver final : public GpioDriver {
public:
    std::expected<std::unique_ptr<GpioLine>, BackendError> open(
        std::string_view chip_path, std::uint32_t offset,
        GpioDirection direction, bool initial_value, bool active_low,
        std::string_view consumer) override {
        const std::string path(chip_path);
        gpiod_chip* chip = gpiod_chip_open(path.c_str());
        if (chip == nullptr) {
            return std::unexpected(gpio_system_error(
                gpio_error::chip_open_failed, "opening GPIO chip " + path));
        }
        gpiod_line* line = gpiod_chip_get_line(chip, offset);
        if (line == nullptr) {
            auto error = gpio_system_error(
                gpio_error::line_request_failed, "getting GPIO line");
            gpiod_chip_close(chip);
            return std::unexpected(std::move(error));
        }

        const std::string consumer_name(consumer);
        const gpiod_line_request_config config{
            .consumer = consumer_name.c_str(),
            .request_type = direction == GpioDirection::input
                                ? GPIOD_LINE_REQUEST_DIRECTION_INPUT
                                : GPIOD_LINE_REQUEST_DIRECTION_OUTPUT,
            .flags = active_low ? GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW : 0,
        };
        if (gpiod_line_request(line, &config, initial_value ? 1 : 0) < 0) {
            auto error = gpio_system_error(
                gpio_error::line_request_failed, "requesting GPIO line");
            gpiod_chip_close(chip);
            return std::unexpected(std::move(error));
        }
        return std::unique_ptr<GpioLine>(
            new LibgpiodLine(chip, line, direction));
    }
};

}  // namespace

std::shared_ptr<GpioDriver> create_libgpiod_driver() {
    return std::make_shared<LibgpiodDriver>();
}

}  // namespace nrpc
