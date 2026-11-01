#include "lanlink/protocol/wire.hpp"
#include "lanlink/transport/connection.hpp"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {
using namespace lanlink;
using namespace lanlink::protocol;
using namespace lanlink::transport;
using namespace std::chrono_literals;

TEST(TcpConnection, AcceptsLoopbackAndRoundTripsMessages) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok()) << listener_result.error();
    auto listener = std::move(listener_result.value());
    const auto port = listener.local_endpoint().port;

    std::atomic<bool> server_ok{false};
    std::thread server([&] {
        auto accepted = listener.accept(2s);
        if (!accepted.ok())
            return;
        auto connection = std::move(accepted.value());
        auto messages = connection.receive(2s);
        if (!messages.ok() || messages.value().size() != 1)
            return;
        if (!std::holds_alternative<Heartbeat>(messages.value()[0]) ||
            std::get<Heartbeat>(messages.value()[0]).counter != 7)
            return;
        if (!connection.send(Heartbeat{8}).ok())
            return;
        server_ok = true;
    });

    auto client_result = Connection::connect(Endpoint{"127.0.0.1", port});
    ASSERT_TRUE(client_result.ok()) << client_result.error();
    auto client = std::move(client_result.value());
    ASSERT_TRUE(client.send(Heartbeat{7}).ok());
    auto reply = client.receive(2s);
    ASSERT_TRUE(reply.ok()) << reply.error();
    ASSERT_EQ(reply.value().size(), 1u);
    ASSERT_TRUE(std::holds_alternative<Heartbeat>(reply.value()[0]));
    EXPECT_EQ(std::get<Heartbeat>(reply.value()[0]).counter, 8u);

    server.join();
    EXPECT_TRUE(server_ok);
}

TEST(TcpConnection, DeliversConsecutiveFramesAndHandlesPeerClose) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok()) << listener_result.error();
    auto listener = std::move(listener_result.value());
    const auto port = listener.local_endpoint().port;

    std::thread server([&] {
        auto accepted = listener.accept(2s);
        ASSERT_TRUE(accepted.ok()) << accepted.error();
        auto connection = std::move(accepted.value());
        ASSERT_TRUE(connection.send(Heartbeat{1}).ok());
        ASSERT_TRUE(connection.send(Heartbeat{2}).ok());
        ASSERT_TRUE(connection.close().ok());
    });

    auto client_result = Connection::connect(Endpoint{"127.0.0.1", port});
    ASSERT_TRUE(client_result.ok()) << client_result.error();
    auto client = std::move(client_result.value());
    std::vector<Message> messages;
    for (int i = 0; i < 3 && messages.size() < 2; ++i) {
        auto received = client.receive(2s);
        ASSERT_TRUE(received.ok()) << received.error();
        messages.insert(messages.end(), received.value().begin(), received.value().end());
    }
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(std::get<Heartbeat>(messages[0]).counter, 1u);
    EXPECT_EQ(std::get<Heartbeat>(messages[1]).counter, 2u);

    auto eof = client.receive(2s);
    EXPECT_FALSE(eof.ok());
    EXPECT_FALSE(client.connected());
    EXPECT_TRUE(client.close().ok());
    EXPECT_TRUE(client.close().ok());
    server.join();
}

TEST(TcpConnection, RefusesConnectionAfterListenerCloses) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok()) << listener_result.error();
    const auto port = listener_result.value().local_endpoint().port;
    ASSERT_TRUE(listener_result.value().close().ok());

    auto client = Connection::connect(Endpoint{"127.0.0.1", port});
    EXPECT_FALSE(client.ok());
}

} // namespace
