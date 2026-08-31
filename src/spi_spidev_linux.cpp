#include "nrpc/spi_backend.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <limits>
#include <string>
#include <utility>

namespace nrpc {
namespace {

BackendError system_error(std::uint32_t code, std::string operation) {
    const auto saved_errno = errno;
    operation += ": ";
    operation += std::strerror(saved_errno);
    return {code, std::move(operation)};
}

class SpidevDevice final : public SpiDevice {
public:
    SpidevDevice(int descriptor, std::uint8_t bits_per_word)
        : descriptor_(descriptor), bits_per_word_(bits_per_word) {}

    ~SpidevDevice() override { close(); }

    std::expected<std::vector<std::byte>, BackendError> transfer(
        std::span<const std::byte> transmit,
        const SpiTransferOptions& options) override {
        if (descriptor_ < 0) return std::unexpected(BackendError{
            spi_error::invalid_handle, "SPI device is closed"});
        if (transmit.size() > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(BackendError{
                spi_error::invalid_arguments, "SPI transfer is too large"});
        }
        std::vector<std::byte> receive(transmit.size());
        spi_ioc_transfer message{};
        message.tx_buf = reinterpret_cast<std::uintptr_t>(transmit.data());
        message.rx_buf = reinterpret_cast<std::uintptr_t>(receive.data());
        message.len = static_cast<std::uint32_t>(transmit.size());
        message.speed_hz = options.speed_hz;
        message.delay_usecs = options.delay_us;
        message.bits_per_word = bits_per_word_;
        message.cs_change = options.cs_change ? 1U : 0U;
        const int transferred =
            ::ioctl(descriptor_, SPI_IOC_MESSAGE(1), &message);
        if (transferred < 0) {
            return std::unexpected(system_error(
                spi_error::transfer_failed, "SPI full-duplex transfer"));
        }
        if (static_cast<std::size_t>(transferred) != transmit.size()) {
            return std::unexpected(BackendError{
                spi_error::transfer_failed,
                "SPI driver completed a short transfer"});
        }
        return receive;
    }

    void close() noexcept override {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
    }

private:
    int descriptor_;
    std::uint8_t bits_per_word_;
};

class SpidevDriver final : public SpiDriver {
public:
    std::expected<std::unique_ptr<SpiDevice>, BackendError> open(
        std::string_view device, std::uint8_t mode,
        std::uint32_t max_speed_hz, std::uint8_t bits_per_word,
        bool lsb_first) override {
        const std::string path(device);
        const int descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (descriptor < 0) {
            return std::unexpected(system_error(
                spi_error::open_failed, "opening SPI device " + path));
        }

        const auto fail = [&](std::string operation)
            -> std::expected<std::unique_ptr<SpiDevice>, BackendError> {
            const auto error = system_error(spi_error::configure_failed,
                                            std::move(operation));
            ::close(descriptor);
            return std::unexpected(error);
        };
        auto configured_mode = mode;
        auto configured_bits = bits_per_word;
        auto configured_speed = max_speed_hz;
        std::uint8_t configured_lsb = lsb_first ? 1U : 0U;
        if (::ioctl(descriptor, SPI_IOC_WR_MODE, &configured_mode) < 0)
            return fail("setting SPI mode");
        if (::ioctl(descriptor, SPI_IOC_WR_BITS_PER_WORD, &configured_bits) < 0)
            return fail("setting SPI bits per word");
        if (::ioctl(descriptor, SPI_IOC_WR_MAX_SPEED_HZ, &configured_speed) < 0)
            return fail("setting SPI maximum speed");
        if (::ioctl(descriptor, SPI_IOC_WR_LSB_FIRST, &configured_lsb) < 0)
            return fail("setting SPI bit order");
        std::unique_ptr<SpiDevice> result =
            std::make_unique<SpidevDevice>(descriptor, bits_per_word);
        return result;
    }
};

}  // namespace

std::shared_ptr<SpiDriver> create_spidev_driver() {
    return std::make_shared<SpidevDriver>();
}

}  // namespace nrpc
