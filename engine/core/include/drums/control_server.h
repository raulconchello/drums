// Local WebSocket server for the UI. CONTRACT FILE. Implemented in core/src. Not real-time.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace drums {

class ControlServer {
public:
    // Raw JSON text from a client; called on a server thread (handler must be thread-safe).
    using CommandHandler = std::function<void(const std::string& text)>;
    // A client connected; return the messages to send to that client only (e.g. hello + state).
    using ConnectHandler = std::function<std::vector<std::string>()>;

    explicit ControlServer(uint16_t port = 7878, std::string host = "127.0.0.1");
    ~ControlServer();  // stops the server
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    void onCommand(CommandHandler handler);  // set before start()
    void onConnect(ConnectHandler handler);  // set before start()

    bool start(std::string* error = nullptr);  // false if the port is taken etc.
    void stop();

    void broadcast(const std::string& text);  // thread-safe, never blocks on slow clients
    int clientCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace drums
