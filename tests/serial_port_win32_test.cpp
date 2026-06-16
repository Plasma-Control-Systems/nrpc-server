#include "serial_port_win32_internal.hpp"

#include <gtest/gtest.h>

namespace {

nrpc::SerialConfig config(nrpc::SerialFlowControl flow) {
    return nrpc::SerialConfig{
        .baud_rate = 115200,
        .data_bits = 8,
        .parity = nrpc::SerialParity::even,
        .stop_bits = 2,
        .flow_control = flow,
    };
}

TEST(Win32SerialPort, NormalizesComDeviceNames) {
    const auto short_name = nrpc::win32_serial::device_path("COM3");
    ASSERT_TRUE(short_name);
    EXPECT_EQ(*short_name, L"\\\\.\\COM3");

    const auto long_name = nrpc::win32_serial::device_path("com17");
    ASSERT_TRUE(long_name);
    EXPECT_EQ(*long_name, L"\\\\.\\com17");

    const auto explicit_path =
        nrpc::win32_serial::device_path("\\\\.\\COM21");
    ASSERT_TRUE(explicit_path);
    EXPECT_EQ(*explicit_path, L"\\\\.\\COM21");
}

TEST(Win32SerialPort, MapsLineAndFlowControlSettings) {
    DCB state{};
    state.DCBlength = sizeof(state);
    nrpc::win32_serial::configure_state(
        state, config(nrpc::SerialFlowControl::rts_cts));

    EXPECT_EQ(state.BaudRate, 115200U);
    EXPECT_EQ(state.ByteSize, 8);
    EXPECT_EQ(state.Parity, EVENPARITY);
    EXPECT_EQ(state.StopBits, TWOSTOPBITS);
    EXPECT_TRUE(state.fBinary);
    EXPECT_TRUE(state.fParity);
    EXPECT_TRUE(state.fOutxCtsFlow);
    EXPECT_EQ(state.fRtsControl, RTS_CONTROL_HANDSHAKE);
    EXPECT_FALSE(state.fOutX);
    EXPECT_FALSE(state.fInX);

    nrpc::win32_serial::configure_state(
        state, config(nrpc::SerialFlowControl::software));
    EXPECT_FALSE(state.fOutxCtsFlow);
    EXPECT_TRUE(state.fOutX);
    EXPECT_TRUE(state.fInX);
    EXPECT_EQ(state.fRtsControl, RTS_CONTROL_ENABLE);

    nrpc::win32_serial::configure_state(
        state, config(nrpc::SerialFlowControl::dtr_dsr));
    EXPECT_TRUE(state.fOutxDsrFlow);
    EXPECT_EQ(state.fDtrControl, DTR_CONTROL_HANDSHAKE);
    EXPECT_FALSE(state.fOutX);
    EXPECT_FALSE(state.fInX);
}

}  // namespace
