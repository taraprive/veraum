#pragma once

#include <functional>
#include <string>
#include <vector>

#include "src/core/types.h"

namespace hftarb {

// Every exchange is exposed through this interface. The arbitrage engine only
// knows about normalized OrderBook ticks — never about exchange-specific APIs.
class IMarketAdapter {
public:
    using TickCallback = std::function<void(const Tick&)>;

    virtual ~IMarketAdapter() = default;

    virtual const std::string& name() const = 0;

    // Symbols this adapter is responsible for.
    virtual const std::vector<std::string>& symbols() const = 0;

    virtual void setTickCallback(TickCallback cb) = 0;

    // Connect + subscribe (blocking; returns false on failure).
    virtual bool start() = 0;

    // Stop the feed. Must be safe to call from any thread.
    virtual void stop() = 0;

    virtual bool isConnected() const = 0;

    // Keep-alive / health probe. Adapters override with real heartbeat logic.
    virtual void heartbeat() {}
};

// Minimal WebSocket abstraction. Concrete implementations (Boost.Beast,
// IXWebSocket, uWebSockets, ...) plug in behind this interface. The scaffold
// ships with a MockWsClient so the pipeline runs offline end-to-end.
class IWsClient {
public:
    using MessageCallback = std::function<void(const std::string& payload)>;
    using StateCallback = std::function<void(bool connected)>;

    virtual ~IWsClient() = default;

    virtual void setMessageCallback(MessageCallback cb) = 0;
    virtual void setStateCallback(StateCallback cb) = 0;

    virtual bool connect(const std::string& url, const std::string& subMessage) = 0;
    virtual bool send(const std::string& msg) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
};

}  // namespace hftarb
