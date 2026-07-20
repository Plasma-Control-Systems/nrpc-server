#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "nrpc/logger.hpp"

TEST(Logger, WritesAndFlushesEverySink) {
    std::ostringstream first;
    std::ostringstream second;
    nrpc::Logger logger(
        {std::ref(first), std::ref(second)}, nrpc::LogLevel::debug);

    logger.log(nrpc::LogLevel::info, "server started");

    EXPECT_EQ(first.str(), second.str());
    EXPECT_NE(first.str().find("[INFO]"), std::string::npos);
    EXPECT_NE(first.str().find("server started"), std::string::npos);
}

TEST(Logger, ConcurrentMessagesRemainWholeLines) {
    std::ostringstream output;
    nrpc::Logger logger(output, nrpc::LogLevel::debug);
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&, thread] {
            for (int message = 0; message < 25; ++message) {
                logger.log(
                    nrpc::LogLevel::debug,
                    "message-" + std::to_string(thread) + '-' +
                        std::to_string(message));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    const auto text = output.str();
    EXPECT_EQ(
        static_cast<std::size_t>(
            std::count(text.begin(), text.end(), '\n')),
        100U);
}

TEST(Logger, PacketDumpRequiresRuntimeEnable) {
    const std::array<std::byte, 2> packet{std::byte{0x12}, std::byte{0xab}};

    std::ostringstream disabled;
    nrpc::Logger disabled_logger(disabled, nrpc::LogLevel::debug);
    disabled_logger.packet(nrpc::LogLevel::debug, "RX Request", packet, "function=0");
    EXPECT_EQ(disabled.str(), "");

    std::ostringstream enabled;
    nrpc::Logger enabled_logger(enabled, nrpc::LogLevel::debug, true);
    enabled_logger.packet(nrpc::LogLevel::debug, "RX Request", packet, "function=0");
    EXPECT_NE(enabled.str().find("RX Request function=0 bytes=12ab"),
              std::string::npos);
}

TEST(Logger, RpcTimingRequiresRuntimeEnable) {
    std::ostringstream disabled;
    nrpc::Logger disabled_logger(disabled, nrpc::LogLevel::debug);
    disabled_logger.rpc_timing("client=7 function=16 status=success",
                               std::chrono::microseconds(1234));
    EXPECT_EQ(disabled.str(), "");

    std::ostringstream enabled;
    nrpc::Logger enabled_logger(enabled, nrpc::LogLevel::debug, false, true);
    enabled_logger.rpc_timing("client=7 function=16 status=success",
                              std::chrono::microseconds(1234));
    EXPECT_NE(enabled.str().find(
                  "RPC Timing client=7 function=16 status=success duration_us=1234"),
              std::string::npos);
}
