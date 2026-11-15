#include "drums/control_server.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXUtf8Validator.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

// Notes on IXWebSocket (11.4.5) that shape this file:
//  - A server-side send() blocks until the client has taken the data (it flushes the socket
//    before returning). So nothing is sent on the caller's thread: broadcast() only queues, and
//    every client has its own sender thread. A client that falls more than kMaxQueuedBytes
//    behind is disconnected (the UI reconnects and gets hello + state again).
//  - The connection thread's read poll and the flush inside send() share one wake-up pipe per
//    socket. A close() issued while a sender is blocked in a flush may wake the sender instead of
//    the connection thread, which then sleeps on. Hence: after an overflow the sender repeats
//    close() once it is no longer polling, and stop() retires the sender threads before asking
//    IXWebSocket to close the connections.
//  - When a send fails because the peer went away, the sender thread itself reports the close
//    (WebSocketTransport::setReadyState(CLOSED), whose check-then-set is not atomic) and may call
//    the message callback right after the connection thread has reset it to nullptr when its
//    run loop ended: std::bad_function_call -> terminate. Hence onClose() on the connection
//    thread does not return before the sender thread has finished (bounded wait, because IX may
//    hold a transport mutex the sender needs while it reports the close; in that case the
//    sender's own close report finds the CLOSED state and does not call back).

namespace drums {
namespace {

constexpr size_t kMaxQueuedBytes = 4u << 20;  // minutes of normal traffic (~10 KB/s)
constexpr int kMaxConnections = 32;
constexpr int kHandshakeTimeoutSecs = 3;
constexpr uint16_t kCloseTooSlow = 1008;  // policy violation
constexpr auto kSenderExitWait = std::chrono::milliseconds(500);

using Message = std::shared_ptr<const std::string>;

struct Client {
    std::weak_ptr<ix::WebSocket> socket;  // owned by IXWebSocket's connection thread

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Message> queue;
    size_t queuedBytes = 0;
    bool ready = false;          // the onConnect messages are queued: sending may start
    bool closed = false;         // connection gone or server stopping: the sender exits
    bool overflow = false;       // too slow: being disconnected, further messages are dropped
    bool closeAgain = false;     // overflow: the sender repeats close() (see the note above)
    bool senderDone = true;      // no sender thread running (set false on Open)
    std::thread::id senderId;    // set by the sender thread itself
    std::condition_variable senderExited;

    std::thread sender;  // started on Open (under clientsMutex); joined by reap() or stop()
};

void senderLoop(const std::shared_ptr<Client>& client) {
    {
        std::lock_guard lock(client->mutex);
        client->senderId = std::this_thread::get_id();
    }
    struct Done {
        Client& c;
        ~Done() {
            {
                std::lock_guard lock(c.mutex);
                c.senderDone = true;
            }
            c.senderExited.notify_all();
        }
    } done{*client};

    for (;;) {
        Message message;
        bool closeAgain = false;
        {
            std::unique_lock lock(client->mutex);
            client->wake.wait(lock, [&] {
                return client->closed || client->closeAgain || (client->ready && !client->queue.empty());
            });
            if (client->closed) return;
            if (client->closeAgain) {
                client->closeAgain = false;
                closeAgain = true;
            } else {
                message = std::move(client->queue.front());
                client->queue.pop_front();
                client->queuedBytes -= message->size();
            }
        }
        auto socket = client->socket.lock();
        if (!socket) return;
        if (closeAgain)
            socket->close(kCloseTooSlow, "client too slow");
        else
            socket->sendUtf8Text(*message);  // fails quietly once the connection is closing
    }
}

}  // namespace

struct ControlServer::Impl {
    uint16_t port;
    std::string host;
    CommandHandler commandHandler;
    ConnectHandler connectHandler;

    std::mutex lifecycleMutex;  // start / stop
    std::unique_ptr<ix::WebSocketServer> server;

    mutable std::mutex clientsMutex;
    std::vector<std::shared_ptr<Client>> clients;   // open connections, receive broadcasts
    std::vector<std::shared_ptr<Client>> finished;  // closed, sender thread not joined yet
    bool stopping = false;                          // stop() owns every sender thread now

    Impl(uint16_t p, std::string h) : port(p), host(std::move(h)) {}

    // ---- called on IXWebSocket's connection threads ----

    void onOpen(const std::shared_ptr<Client>& client) {
        reap();
        {
            std::lock_guard lock(clientsMutex);
            if (stopping) return;  // IXWebSocket closes it in a moment
            clients.push_back(client);  // broadcasts from now on are queued (not sent before ready)
            {
                std::lock_guard clientLock(client->mutex);
                client->senderDone = false;
            }
            client->sender = std::thread(senderLoop, client);
        }

        std::vector<std::string> greeting;
        if (connectHandler) {
            try {
                greeting = connectHandler();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "drums::ControlServer: onConnect handler threw: %s\n", e.what());
            } catch (...) {
                std::fprintf(stderr, "drums::ControlServer: onConnect handler threw\n");
            }
        }
        {
            // The greeting goes before anything broadcast while the handler ran.
            std::lock_guard lock(client->mutex);
            for (auto it = greeting.rbegin(); it != greeting.rend(); ++it) {
                if (!ix::validateUtf8(*it)) continue;
                client->queuedBytes += it->size();
                client->queue.push_front(std::make_shared<const std::string>(std::move(*it)));
            }
            client->ready = true;
        }
        client->wake.notify_one();
    }

    // May also run on the client's sender thread (IXWebSocket reports a failed write as a close).
    void onClose(const std::shared_ptr<Client>& client) {
        {
            std::lock_guard lock(clientsMutex);
            auto it = std::find(clients.begin(), clients.end(), client);
            if (it != clients.end()) {
                clients.erase(it);
                if (!stopping) finished.push_back(client);  // its sender thread is joined later
            }
        }
        std::unique_lock lock(client->mutex);
        client->closed = true;
        client->queue.clear();
        client->queuedBytes = 0;
        client->wake.notify_one();
        // On the connection thread: let the sender leave IXWebSocket before IX resets the
        // callbacks (see the note at the top).
        if (client->senderId != std::this_thread::get_id())
            client->senderExited.wait_for(lock, kSenderExitWait, [&] { return client->senderDone; });
    }

    void onText(const std::string& text) {
        if (!commandHandler) return;
        try {
            commandHandler(text);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "drums::ControlServer: command handler threw: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "drums::ControlServer: command handler threw\n");
        }
    }

    // Join the sender threads of closed connections (they exit right after `closed` is set).
    void reap() {
        std::vector<std::shared_ptr<Client>> done;
        {
            std::lock_guard lock(clientsMutex);
            done.swap(finished);
        }
        for (auto& c : done)
            if (c->sender.joinable()) c->sender.join();
    }
};

ControlServer::ControlServer(uint16_t port, std::string host)
    : impl_(std::make_unique<Impl>(port, std::move(host))) {}

ControlServer::~ControlServer() { stop(); }

void ControlServer::onCommand(CommandHandler handler) { impl_->commandHandler = std::move(handler); }
void ControlServer::onConnect(ConnectHandler handler) { impl_->connectHandler = std::move(handler); }

bool ControlServer::start(std::string* error) {
    Impl& s = *impl_;
    std::lock_guard lifecycle(s.lifecycleMutex);
    if (s.server) return true;  // already running

    ix::initNetSystem();
    const std::string host = s.host == "localhost" ? "127.0.0.1" : s.host;
    const int family = host.find(':') != std::string::npos ? AF_INET6 : AF_INET;
    auto server = std::make_unique<ix::WebSocketServer>(s.port, host, ix::SocketServer::kDefaultTcpBacklog,
                                                        kMaxConnections, kHandshakeTimeoutSecs, family);
    server->disablePerMessageDeflate();

    Impl* impl = &s;  // the IX server (and its threads) never outlive *impl: stop() runs first
    server->setOnConnectionCallback([impl](std::weak_ptr<ix::WebSocket> weakSocket,
                                           std::shared_ptr<ix::ConnectionState>) {
        auto socket = weakSocket.lock();
        if (!socket) return;
        auto client = std::make_shared<Client>();
        client->socket = weakSocket;
        // Runs on this connection's IX thread: Open, then the messages, then Close.
        socket->setOnMessageCallback([impl, client](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open: impl->onOpen(client); break;
                case ix::WebSocketMessageType::Message:
                    if (!msg->binary) impl->onText(msg->str);
                    break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error: impl->onClose(client); break;
                default: break;
            }
        });
    });

    auto result = server->listen();
    if (!result.first) {
        if (error) *error = result.second;
        return false;
    }
    server->start();
    s.server = std::move(server);
    return true;
}

void ControlServer::stop() {
    Impl& s = *impl_;
    std::lock_guard lifecycle(s.lifecycleMutex);
    if (!s.server) return;

    // 1. Retire our sender threads. close() cancels a send blocked on a slow client.
    std::vector<std::shared_ptr<Client>> all;
    {
        std::lock_guard lock(s.clientsMutex);
        s.stopping = true;
        all = std::move(s.clients);
        all.insert(all.end(), s.finished.begin(), s.finished.end());
        s.clients.clear();
        s.finished.clear();
    }
    for (auto& c : all) {
        {
            std::lock_guard lock(c->mutex);
            c->closed = true;
            c->queue.clear();
            c->queuedBytes = 0;
        }
        c->wake.notify_one();
        if (auto socket = c->socket.lock()) socket->close();
    }
    for (auto& c : all)
        if (c->sender.joinable()) c->sender.join();

    // 2. IXWebSocket closes every connection and joins its threads (the Close callbacks run
    //    during this call). Nothing else polls the sockets now, so its wake-ups arrive.
    s.server->stop();
    s.server.reset();

    std::lock_guard lock(s.clientsMutex);
    s.stopping = false;
}

void ControlServer::broadcast(const std::string& text) {
    Impl& s = *impl_;
    if (!ix::validateUtf8(text)) {
        std::fprintf(stderr, "drums::ControlServer: not broadcasting a message that is not valid UTF-8\n");
        return;
    }
    auto message = std::make_shared<const std::string>(text);
    std::vector<std::shared_ptr<ix::WebSocket>> tooSlow;
    {
        std::lock_guard lock(s.clientsMutex);
        for (auto& c : s.clients) {
            std::lock_guard clientLock(c->mutex);
            if (c->closed || c->overflow) continue;
            if (c->queuedBytes + message->size() > kMaxQueuedBytes) {
                c->overflow = true;
                c->closeAgain = true;
                c->queue.clear();
                c->queuedBytes = 0;
                c->wake.notify_one();
                if (auto socket = c->socket.lock()) tooSlow.push_back(std::move(socket));
                continue;
            }
            c->queue.push_back(message);
            c->queuedBytes += message->size();
            c->wake.notify_one();
        }
    }
    // close() does not wait for the peer (it cancels a pending flush), so this stays quick.
    for (auto& socket : tooSlow) socket->close(kCloseTooSlow, "client too slow");
}

int ControlServer::clientCount() const {
    std::lock_guard lock(impl_->clientsMutex);
    return static_cast<int>(impl_->clients.size());
}

}  // namespace drums
