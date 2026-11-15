#include "drums/control_server.h"
#include "drums/protocol.h"

#include "test_util.h"

#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXWebSocket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using namespace drums;
using namespace std::chrono_literals;

namespace {

bool waitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 5000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

class TestClient {
public:
    explicit TestClient(int port) {
        ws_.setUrl("ws://127.0.0.1:" + std::to_string(port) + "/");
        ws_.disableAutomaticReconnection();
        ws_.disablePerMessageDeflate();
        ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            std::lock_guard lock(mutex_);
            switch (msg->type) {
                case ix::WebSocketMessageType::Open: open_ = true; break;
                case ix::WebSocketMessageType::Message: messages_.push_back(msg->str); break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error: closed_ = true; break;
                default: break;
            }
            changed_.notify_all();
        });
        ws_.start();
    }
    ~TestClient() { ws_.stop(); }

    bool waitForMessages(size_t n, std::chrono::milliseconds timeout = 5000ms) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return messages_.size() >= n; });
    }
    bool waitOpen(std::chrono::milliseconds timeout = 5000ms) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return open_; });
    }
    bool waitClosed(std::chrono::milliseconds timeout = 5000ms) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return closed_; });
    }
    std::vector<std::string> messages() {
        std::lock_guard lock(mutex_);
        return messages_;
    }
    void send(const std::string& text) { ws_.send(text); }
    void stop() { ws_.stop(); }

private:
    ix::WebSocket ws_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> messages_;
    bool open_ = false;
    bool closed_ = false;
};

struct Recorder {
    std::mutex mutex;
    std::vector<std::string> commands;
    std::atomic<int> connects{0};

    void install(ControlServer& server) {
        server.onConnect([this] {
            const int n = ++connects;
            return std::vector<std::string>{proto::helloJson(48000, 64, 2.5),
                                            R"({"type":"state","n":)" + std::to_string(n) + "}"};
        });
        server.onCommand([this](const std::string& text) {
            std::lock_guard lock(mutex);
            commands.push_back(text);
        });
    }
    size_t commandCount() {
        std::lock_guard lock(mutex);
        return commands.size();
    }
};

}  // namespace

TEST_CASE("control server: greeting, commands, broadcast, disconnect") {
    const int port = ix::getFreePort();
    ControlServer server{uint16_t(port), "127.0.0.1"};
    Recorder rec;
    rec.install(server);
    std::string error;
    REQUIRE_MESSAGE(server.start(&error), error);
    CHECK(server.clientCount() == 0);

    TestClient a(port);
    REQUIRE(a.waitForMessages(2));
    auto msgs = a.messages();
    CHECK(msgs[0] == proto::helloJson(48000, 64, 2.5));
    CHECK(msgs[1] == R"({"type":"state","n":1})");
    REQUIRE(waitUntil([&] { return server.clientCount() == 1; }));

    a.send(R"({"type":"play"})");
    a.send(R"({"type":"setBpm","bpm":72})");
    REQUIRE(waitUntil([&] { return rec.commandCount() == 2; }));
    CHECK(rec.commands[0] == R"({"type":"play"})");
    CHECK(rec.commands[1] == R"({"type":"setBpm","bpm":72})");

    const std::string position = proto::positionJson(true, 10, 0, 10, 13.9);
    server.broadcast(position);
    REQUIRE(a.waitForMessages(3));
    CHECK(a.messages()[2] == position);

    // A second client gets its own greeting; the first one does not see it.
    TestClient b(port);
    REQUIRE(b.waitForMessages(2));
    CHECK(b.messages()[1] == R"({"type":"state","n":2})");
    REQUIRE(waitUntil([&] { return server.clientCount() == 2; }));
    server.broadcast(R"({"type":"missed","instrument":"kick","loop":0,"expectedTick":0})");
    REQUIRE(a.waitForMessages(4));
    REQUIRE(b.waitForMessages(3));
    CHECK(a.messages().size() == 4);
    CHECK(a.messages()[3] == b.messages()[2]);

    // Messages arrive in broadcast order.
    for (int k = 0; k < 200; ++k) server.broadcast(std::to_string(k));
    REQUIRE(a.waitForMessages(204));
    msgs = a.messages();
    for (int k = 0; k < 200; ++k) CHECK(msgs[size_t(4 + k)] == std::to_string(k));

    b.stop();
    CHECK(waitUntil([&] { return server.clientCount() == 1; }));
    a.stop();
    CHECK(waitUntil([&] { return server.clientCount() == 0; }));
    server.broadcast("nobody listens");  // no clients: fine

    server.stop();
    server.stop();  // idempotent

    // It can be started again on the same port.
    REQUIRE_MESSAGE(server.start(&error), error);
    TestClient c(port);
    REQUIRE(c.waitForMessages(2));
    CHECK(c.messages()[1] == R"({"type":"state","n":3})");
    server.stop();
    CHECK(c.waitClosed());
    CHECK(server.clientCount() == 0);
}

TEST_CASE("control server: port already in use") {
    const int port = ix::getFreePort();
    ControlServer first{uint16_t(port)};
    first.onConnect([] { return std::vector<std::string>{}; });
    REQUIRE(first.start());
    ControlServer second{uint16_t(port)};
    second.onConnect([] { return std::vector<std::string>{}; });
    std::string error;
    CHECK_FALSE(second.start(&error));
    CHECK_FALSE(error.empty());
    MESSAGE("start() error: " << error);
}

TEST_CASE("control server: clients disconnecting during broadcasts, stop while busy") {
    const int port = ix::getFreePort();
    auto server = std::make_unique<ControlServer>(uint16_t(port));
    Recorder rec;
    rec.install(*server);
    REQUIRE(server->start());

    std::atomic<bool> running{true};
    std::atomic<int> broadcasts{0};
    std::thread broadcaster([&] {
        const std::string payload = proto::positionJson(true, 1234.5, 0, 1234.5, 1714.6);
        while (running) {
            server->broadcast(payload);
            ++broadcasts;
            std::this_thread::sleep_for(50us);
        }
    });

    for (int k = 0; k < 15; ++k) {
        TestClient client(port);
        REQUIRE(client.waitForMessages(5));  // greeting + a few broadcasts
        auto msgs = client.messages();
        CHECK(msgs[0].find(R"("type":"hello")") != std::string::npos);  // greeting first
        CHECK(msgs[1].find(R"("type":"state")") != std::string::npos);
        if (k % 3 == 0) client.send(R"({"type":"getState"})");
        // ~TestClient disconnects while the broadcaster keeps going
    }
    CHECK(waitUntil([&] { return server->clientCount() == 0; }));

    // Stop and destroy the server while clients are connected and broadcasts are running.
    TestClient last1(port), last2(port);
    REQUIRE(last1.waitForMessages(3));
    REQUIRE(last2.waitForMessages(3));
    server->stop();
    CHECK(last1.waitClosed());
    CHECK(last2.waitClosed());
    running = false;
    broadcaster.join();
    server.reset();
    CHECK(broadcasts > 0);
}

TEST_CASE("control server: a client that stops reading never blocks broadcast") {
    const int port = ix::getFreePort();
    ControlServer server{uint16_t(port)};
    Recorder rec;
    rec.install(server);
    REQUIRE(server.start());

    // A raw WebSocket client with a tiny receive buffer that never reads after the handshake.
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    int small = 4096;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    const std::string request =
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    REQUIRE(::send(fd, request.data(), request.size(), 0) == ssize_t(request.size()));
    std::string response;
    char c;
    while (response.find("\r\n\r\n") == std::string::npos && ::recv(fd, &c, 1, 0) == 1) response += c;
    CHECK(response.find(" 101 ") != std::string::npos);

    TestClient fast(port);
    REQUIRE(fast.waitForMessages(2));
    REQUIRE(waitUntil([&] { return server.clientCount() == 2; }));

    // 4000 x 4 KB = 16 MB: far more than the slow client's socket buffers and the 4 MB backlog
    // limit. Paced so that the fast (debug-build) client keeps up.
    const std::string big = R"({"type":"error","message":")" + std::string(4096, 'x') + R"("})";
    std::chrono::nanoseconds slowest{0}, inBroadcast{0};
    for (int k = 0; k < 4000; ++k) {
        const auto t0 = std::chrono::steady_clock::now();
        server.broadcast(big);
        const auto dt = std::chrono::steady_clock::now() - t0;
        slowest = std::max(slowest, dt);
        inBroadcast += dt;
        if (k % 100 == 99) REQUIRE(fast.waitForMessages(size_t(2 + k + 1), 10000ms));
    }
    MESSAGE("4000 broadcasts: " << std::chrono::duration<double, std::milli>(inBroadcast).count()
                                << " ms in broadcast(), slowest call "
                                << std::chrono::duration<double, std::micro>(slowest).count() << " us");
    // Generous bounds (sanitizer builds are slow); a broadcast blocked by the slow client would
    // never return at all.
    CHECK(inBroadcast < 10s);
    CHECK(slowest < 250ms);

    // The fast client got everything, the slow one is disconnected.
    CHECK(fast.waitForMessages(4002, 10000ms));
    CHECK(fast.messages().size() == 4002);
    CHECK(waitUntil([&] { return server.clientCount() == 1; }, 10000ms));
    server.broadcast("after");
    CHECK(fast.waitForMessages(4003));
    ::close(fd);
    server.stop();
}

TEST_CASE("control server: handlers that throw do not kill the connection") {
    const int port = ix::getFreePort();
    ControlServer server{uint16_t(port)};
    std::atomic<int> calls{0};
    server.onConnect([]() -> std::vector<std::string> { throw std::runtime_error("boom"); });
    server.onCommand([&](const std::string&) {
        ++calls;
        throw std::runtime_error("bad command");
    });
    REQUIRE(server.start());
    TestClient client(port);
    REQUIRE(client.waitOpen());  // the server counts the client before the client side is open
    REQUIRE(waitUntil([&] { return server.clientCount() == 1; }));
    client.send("1");
    client.send("2");
    REQUIRE(waitUntil([&] { return calls == 2; }));
    server.broadcast("still here");
    REQUIRE(client.waitForMessages(1));
    CHECK(client.messages()[0] == "still here");
    // Invalid UTF-8 is not sent (it would make clients drop the connection).
    server.broadcast(std::string("\xff\xfe"));
    server.broadcast("after");
    REQUIRE(client.waitForMessages(2));
    CHECK(client.messages()[1] == "after");
}
