#pragma once

#include "lanlink/common/result.hpp"
#include "lanlink/protocol/wire.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lanlink::transport {

struct Endpoint {
    std::string host;
    std::uint16_t port{};
    friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

struct ConnectionOptions {
    std::chrono::milliseconds io_timeout{1000};
};

enum class ConnectionState {
    kConnected,
    kClosed,
    Connected = kConnected,
    Closed = kClosed,
};

class Connection {
  public:
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    static Result<Connection> connect(const Endpoint& endpoint, ConnectionOptions options = {});

    Result<void> send(const protocol::Message& message);
    Result<std::vector<protocol::Message>>
    receive(std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});
    Result<void> close();

    ConnectionState state() const noexcept;
    bool connected() const noexcept { return state() == ConnectionState::kConnected; }
    std::size_t buffered_bytes() const noexcept;

  private:
    struct Impl;
    explicit Connection(std::unique_ptr<Impl> impl);
    friend class Listener;
    std::unique_ptr<Impl> impl_;
};

class Listener {
  public:
    Listener(Listener&&) noexcept;
    Listener& operator=(Listener&&) noexcept;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    ~Listener();

    static Result<Listener> bind(const Endpoint& endpoint, ConnectionOptions options = {});

    Result<Connection> accept(std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});
    Result<void> close();

    Endpoint local_endpoint() const;
    bool open() const noexcept;

  private:
    struct Impl;
    explicit Listener(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace lanlink::transport
