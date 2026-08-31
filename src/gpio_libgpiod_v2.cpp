#include "nrpc/gpio_backend.hpp"

#include <cerrno>
#include <cstring>
#include <gpiod.h>
#include <string>
#include <utility>

namespace nrpc {
namespace {

BackendError gpio_system_error(std::uint32_t code, std::string operation,
                               int error = errno) {
    return {code, std::move(operation) + " failed: " + std::strerror(error)};
}

class LibgpiodLine final : public GpioLine {
public:
    LibgpiodLine(gpiod_chip* chip, gpiod_line_request* request,
                 std::uint32_t offset, GpioDirection direction)
        : chip_(chip), request_(request), offset_(offset),
          direction_(direction) {}
    ~LibgpiodLine() override { close(); }

    std::expected<bool, BackendError> read() override {
        const auto value = gpiod_line_request_get_value(request_, offset_);
        if (value == GPIOD_LINE_VALUE_ERROR) {
            return std::unexpected(
                gpio_system_error(gpio_error::read_failed, "GPIO read"));
        }
        return value == GPIOD_LINE_VALUE_ACTIVE;
    }

    std::expected<void, BackendError> write(bool value) override {
        if (direction_ != GpioDirection::output) {
            return std::unexpected(BackendError{
                gpio_error::write_failed, "cannot write to a GPIO input"});
        }
        const auto line_value = value ? GPIOD_LINE_VALUE_ACTIVE
                                      : GPIOD_LINE_VALUE_INACTIVE;
        if (gpiod_line_request_set_value(request_, offset_, line_value) < 0) {
            return std::unexpected(
                gpio_system_error(gpio_error::write_failed, "GPIO write"));
        }
        return {};
    }

    void close() noexcept override {
        if (request_ != nullptr) {
            gpiod_line_request_release(request_);
            request_ = nullptr;
        }
        if (chip_ != nullptr) {
            gpiod_chip_close(chip_);
            chip_ = nullptr;
        }
    }

private:
    gpiod_chip* chip_{};
    gpiod_line_request* request_{};
    std::uint32_t offset_{};
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

        gpiod_line_settings* settings = gpiod_line_settings_new();
        gpiod_line_config* line_config = gpiod_line_config_new();
        gpiod_request_config* request_config = gpiod_request_config_new();
        if (settings == nullptr || line_config == nullptr ||
            request_config == nullptr) {
            auto error = gpio_system_error(
                gpio_error::line_request_failed,
                "allocating GPIO request configuration");
            if (settings != nullptr) gpiod_line_settings_free(settings);
            if (line_config != nullptr) gpiod_line_config_free(line_config);
            if (request_config != nullptr)
                gpiod_request_config_free(request_config);
            gpiod_chip_close(chip);
            return std::unexpected(std::move(error));
        }

        const auto native_direction = direction == GpioDirection::input
                                          ? GPIOD_LINE_DIRECTION_INPUT
                                          : GPIOD_LINE_DIRECTION_OUTPUT;
        int status = gpiod_line_settings_set_direction(settings,
                                                        native_direction);
        gpiod_line_settings_set_active_low(settings, active_low);
        if (direction == GpioDirection::output) {
            status |= gpiod_line_settings_set_output_value(
                settings, initial_value ? GPIOD_LINE_VALUE_ACTIVE
                                        : GPIOD_LINE_VALUE_INACTIVE);
        }
        const unsigned int native_offset = offset;
        status |= gpiod_line_config_add_line_settings(
            line_config, &native_offset, 1U, settings);
        const std::string consumer_name(consumer);
        gpiod_request_config_set_consumer(request_config,
                                          consumer_name.c_str());
        gpiod_line_request* request = status == 0
            ? gpiod_chip_request_lines(chip, request_config, line_config)
            : nullptr;
        const int request_error = errno;

        gpiod_request_config_free(request_config);
        gpiod_line_config_free(line_config);
        gpiod_line_settings_free(settings);
        if (request == nullptr) {
            auto error = gpio_system_error(
                gpio_error::line_request_failed, "requesting GPIO line",
                request_error);
            gpiod_chip_close(chip);
            return std::unexpected(std::move(error));
        }
        return std::unique_ptr<GpioLine>(
            new LibgpiodLine(chip, request, offset, direction));
    }
};

}  // namespace

std::shared_ptr<GpioDriver> create_libgpiod_driver() {
    return std::make_shared<LibgpiodDriver>();
}

}  // namespace nrpc
