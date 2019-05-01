/* listener.cc
   Mathieu Stefani, 12 August 2015

*/

#include <pistache/listener.h>
#include <pistache/peer.h>
#include <pistache/common.h>
#include <pistache/os.h>
#include <pistache/transport.h>
#include <pistache/errors.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <chrono>
#include <memory>
#include <vector>

#include <cerrno>
#include <signal.h>


namespace Pistache {
namespace Tcp {

namespace {
    volatile sig_atomic_t g_listen_fd = -1;

    void closeListener() {
        if (g_listen_fd != -1) {
            ::close(g_listen_fd);
            g_listen_fd = -1;
        }
    }

    void handle_sigint(int) {
        closeListener();
    }
}

void setSocketOptions(Fd fd, Flags<Options> options) {
    if (options.hasFlag(Options::ReuseAddr)) {
        int one = 1;
        TRY(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one)));
    }

    if (options.hasFlag(Options::Linger)) {
        struct linger opt;
        opt.l_onoff = 1;
        opt.l_linger = 1;
        TRY(::setsockopt(fd, SOL_SOCKET, SO_LINGER, &opt, sizeof (opt)));
    }

    if (options.hasFlag(Options::FastOpen)) {
        int hint = 5;
        TRY(::setsockopt(fd, SOL_TCP, TCP_FASTOPEN, &hint, sizeof (hint)));
    }
    if (options.hasFlag(Options::NoDelay)) {
        int one = 1;
        TRY(::setsockopt(fd, SOL_TCP, TCP_NODELAY, &one, sizeof (one)));
    }

}

Listener::Listener()
    : addr_()
    , listen_fd(-1)
    , backlog_(Const::MaxBacklog)
    , shutdownFd()
    , poller()
    , options_()
    , workers_(Const::DefaultWorkers)
    , reactor_()
    , transportKey()
{ }

Listener::Listener(const Address& address)
    : addr_(address)
    , listen_fd(-1)
    , backlog_(Const::MaxBacklog)
    , shutdownFd()
    , poller()
    , options_()
    , workers_(Const::DefaultWorkers)
    , reactor_()
    , transportKey()
{
}

Listener::~Listener() {
    if (isBound())
        shutdown();
    if (acceptThread.joinable())
        acceptThread.join();
}

void
Listener::init(
    size_t workers,
    Flags<Options> options, int backlog)
{
    if (workers > hardware_concurrency()) {
        // Log::warning() << "More workers than available cores"
    }

    options_ = options;
    backlog_ = backlog;

    if (options_.hasFlag(Options::InstallSignalHandler)) {
        if (signal(SIGINT, handle_sigint) == SIG_ERR) {
            throw std::runtime_error("Could not install signal handler");
        }
    }

    workers_ = workers;

}

void
Listener::setHandler(const std::shared_ptr<Handler>& handler) {
    handler_ = handler;
}

void
Listener::pinWorker(size_t worker, const CpuSet& set)
{
    UNUSED(worker)
    UNUSED(set)
#if 0
    if (ioGroup.empty()) {
        throw std::domain_error("Invalid operation, did you call init() before ?");
    }
    if (worker > ioGroup.size()) {
        throw std::invalid_argument("Trying to pin invalid worker");
    }

    auto &wrk = ioGroup[worker];
    wrk->pin(set);
#endif
}

void
Listener::bind() {
    bind(addr_);
}

void
Listener::bind(const Address& address) {
    if (!handler_)
        throw std::runtime_error("Call setHandler before calling bind()");
    addr_ = address;

    struct addrinfo hints;
    hints.ai_family = address.family();
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_protocol = 0;

    const auto& host = addr_.host();
    const auto& port = addr_.port().toString();
    AddrInfo addr_info;

    TRY(addr_info.invoke(host.c_str(), port.c_str(), &hints));

    int fd = -1;

    const addrinfo * addr = nullptr;
    for (addr = addr_info.get_info_ptr(); addr; addr = addr->ai_next) {
        fd = ::socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd < 0) continue;

        setSocketOptions(fd, options_);

        if (::bind(fd, addr->ai_addr, addr->ai_addrlen) < 0) {
            close(fd);
            continue;
        }

        TRY(::listen(fd, backlog_));
        break;
    }

    // At this point, it is still possible that we couldn't bind any socket. If it is the case, the previous
    // loop would have exited naturally and addr will be null.
    if (addr == nullptr) {
        throw std::runtime_error(strerror(errno));
    }

    make_non_blocking(fd);
    poller.addFd(fd, Polling::NotifyOn::Read, Polling::Tag(fd));
    listen_fd = fd;
    g_listen_fd = fd;

    transport_ = std::make_shared<Transport>(handler_);

    reactor_.init(Aio::AsyncContext(workers_));
    transportKey = reactor_.addHandler(transport_);
}

bool
Listener::isBound() const {
    return listen_fd != -1;
}

// Return actual TCP port Listener is on, or 0 on error / no port.
// Notes:
// 1) Default constructor for 'Port()' sets value to 0.
// 2) Socket is created inside 'Listener::run()', which is called from
//    'Endpoint::serve()' and 'Endpoint::serveThreaded()'.  So getting the
//    port is only useful if you attempt to do so from a _different_ thread
//    than the one running 'Listener::run()'.  So for a traditional single-
//    threaded program this method is of little value.
Port
Listener::getPort() const {
    if (listen_fd == -1) {
        return Port();
    }

    struct sockaddr_in sock_addr = {0};
    socklen_t addrlen = sizeof(sock_addr);
    auto sock_addr_alias = reinterpret_cast<struct sockaddr*>(&sock_addr);

    if (-1 == getsockname(listen_fd, sock_addr_alias, &addrlen)) {
        return Port();
    }

    return Port(ntohs(sock_addr.sin_port));
}

void
Listener::run(std::promise<void>& promise) {
    shutdownFd.bind(poller);
    reactor_.run();

    promise.set_value();

    for (;;) {
        std::vector<Polling::Event> events;

        int ready_fds = poller.poll(events, 128, std::chrono::milliseconds(-1));
        if (ready_fds == -1) {
            if (errno == EINTR && g_listen_fd == -1) return;
            throw Error::system("Polling");
        }
        for (const auto& event: events) {
            if (event.tag == shutdownFd.tag())
                return;

            if (event.flags.hasFlag(Polling::NotifyOn::Read)) {
                auto fd = event.tag.value();
                if (static_cast<ssize_t>(fd) == listen_fd) {
                    try {
                        handleNewConnection();
                    }
                    catch (SocketError& ex) {
                        std::cerr << "Server: " << ex.what() << std::endl;
                    }
                    catch (ServerError& ex) {
                        std::cerr << "Server: " << ex.what() << std::endl;
                        throw;
                    }
                }
            }
        }
    }
}

void
Listener::runThreaded(std::promise<void>& promise) {
    acceptThread = std::thread([=, &promise]() { this->run(promise); });
}

void
Listener::shutdown() {
    if (shutdownFd.isBound()) shutdownFd.notify();
    reactor_.shutdown();
}

Async::Promise<Listener::Load>
Listener::requestLoad(const Listener::Load& old) {
    auto handlers = reactor_.handlers(transportKey);

    std::vector<Async::Promise<rusage>> loads;
    for (const auto& handler: handlers) {
        auto transport = std::static_pointer_cast<Transport>(handler);
        loads.push_back(transport->load());
    }

    return Async::whenAll(std::begin(loads), std::end(loads)).then([=](const std::vector<rusage>& usages) {

        Load res;
        res.raw = usages;

        if (old.raw.empty()) {
            res.global = 0.0;
            for (size_t i = 0; i < handlers.size(); ++i) res.workers.push_back(0.0);
        } else {

            auto totalElapsed = [](rusage usage) {
                return (usage.ru_stime.tv_sec * 1e6 + usage.ru_stime.tv_usec)
                     + (usage.ru_utime.tv_sec * 1e6 + usage.ru_utime.tv_usec);
            };

            auto now = std::chrono::system_clock::now();
            auto diff = now - old.tick;
            auto tick = std::chrono::duration_cast<std::chrono::microseconds>(diff);
            res.tick = now;

            for (size_t i = 0; i < usages.size(); ++i) {
                auto last = old.raw[i];
                const auto& usage = usages[i];

                auto nowElapsed = totalElapsed(usage);
                auto timeElapsed = nowElapsed - totalElapsed(last);

                auto loadPct = (timeElapsed * 100.0) / tick.count();
                res.workers.push_back(loadPct);
                res.global += loadPct;

            }

            res.global /= usages.size();
        }

        return res;

     }, Async::Throw);
}

Address
Listener::address() const {
    return addr_;
}

Options
Listener::options() const {
    return options_;
}

void
Listener::handleNewConnection() {
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    int client_fd = ::accept(listen_fd, (struct sockaddr *)&peer_addr, &peer_addr_len);
    if (client_fd < 0) {
        if (errno == EBADF || errno == ENOTSOCK)
            throw ServerError(strerror(errno));
        else
            throw SocketError(strerror(errno));
    }

    make_non_blocking(client_fd);

    auto peer = std::make_shared<Peer>(Address::fromUnix((struct sockaddr *)&peer_addr));
    peer->associateFd(client_fd);

    dispatchPeer(peer);
}

void
Listener::dispatchPeer(const std::shared_ptr<Peer>& peer) {
    auto handlers = reactor_.handlers(transportKey);
    auto idx = peer->fd() % handlers.size();
    auto transport = std::static_pointer_cast<Transport>(handlers[idx]);

    transport->handleNewPeer(peer);

}

} // namespace Tcp
} // namespace Pistache
