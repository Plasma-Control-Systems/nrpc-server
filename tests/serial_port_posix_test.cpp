#include "nrpc/serial_backend.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

class PseudoTerminal {
public:
    PseudoTerminal() : master_(posix_openpt(O_RDWR | O_NOCTTY)) {
        if (master_ >= 0 &&
            (grantpt(master_) != 0 || unlockpt(master_) != 0)) {
            close(master_);
            master_ = -1;
        }
    }

    ~PseudoTerminal() {
        if (master_ >= 0) close(master_);
    }

    int master() const { return master_; }

    std::string slave_name() const {
        const char* name = ptsname(master_);
        return name == nullptr ? std::string{} : std::string(name);
    }

private:
    int master_;
};

nrpc::SerialConfig default_config() {
    return {
        .baud_rate = 115200,
        .data_bits = 8,
        .parity = nrpc::SerialParity::none,
        .stop_bits = 1,
        .flow_control = nrpc::SerialFlowControl::none,
    };
}

TEST(PosixSerialPort, ExchangesBytesThroughPseudoTerminal) {
    PseudoTerminal terminal;
    ASSERT_GE(terminal.master(), 0);

    auto serial = nrpc::SerialPort::open(
        terminal.slave_name(), default_config());
    ASSERT_TRUE(serial) << serial.error().message;

    const std::array outbound{std::byte{'n'}, std::byte{'R'}, std::byte{'P'},
                              std::byte{'C'}};
    auto written = (*serial)->write(outbound, 1000);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(*written, outbound.size());
    std::array<std::byte, outbound.size()> from_serial{};
    ASSERT_EQ(read(terminal.master(), from_serial.data(), from_serial.size()),
              static_cast<ssize_t>(from_serial.size()));
    EXPECT_EQ(from_serial, outbound);

    const std::array inbound{std::byte{'O'}, std::byte{'K'}};
    ASSERT_EQ(write(terminal.master(), inbound.data(), inbound.size()),
              static_cast<ssize_t>(inbound.size()));
    std::array<std::byte, inbound.size()> from_device{};
    auto received = (*serial)->read(from_device, 1000);
    ASSERT_TRUE(received) << received.error().message;
    EXPECT_EQ(*received, inbound.size());
    EXPECT_EQ(from_device, inbound);
}

TEST(PosixSerialPort, ReportsReadTimeout) {
    PseudoTerminal terminal;
    ASSERT_GE(terminal.master(), 0);
    auto serial = nrpc::SerialPort::open(
        terminal.slave_name(), default_config());
    ASSERT_TRUE(serial) << serial.error().message;

    std::array<std::byte, 1> buffer{};
    auto received = (*serial)->read(buffer, 10);
    ASSERT_FALSE(received);
    EXPECT_EQ(received.error().code, nrpc::serial_error::operation_timeout);
}

}  // namespace
