#include <gtest/gtest.h>

#include <stdexcept>

#include "nrpc/dispatcher.hpp"

TEST(Dispatcher, RoutesHandlerAndConvertsExceptions) {
    nrpc::Dispatcher dispatcher;
    nrpc::ResourceManager resources;
    nrpc::Arena arena(128);
    const nrpc::RequestContext context{1, 100, resources};

    ASSERT_TRUE(dispatcher.register_handler(
        2, [](const nrpc::RequestContext& received, auto, nrpc::Arena&) {
            EXPECT_EQ(received.client_id, 1U);
            return nrpc::BackendResult(std::vector<nrpc::protocol::VariableView>{});
        }));
    ASSERT_TRUE(dispatcher.dispatch(2, context, {}, arena).has_value());

    const auto missing = dispatcher.dispatch(3, context, {}, arena);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, 1U);

    ASSERT_TRUE(dispatcher.register_handler(
        4, [](const nrpc::RequestContext&, auto, nrpc::Arena&) -> nrpc::BackendResult {
            throw std::runtime_error("driver failed");
        }));
    const auto failed = dispatcher.dispatch(4, context, {}, arena);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, 2U);
}
