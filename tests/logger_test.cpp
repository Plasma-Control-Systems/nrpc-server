#include <gtest/gtest.h>

#include <algorithm>
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
