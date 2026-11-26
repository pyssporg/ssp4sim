#include "server.hpp"

#include "ndjson.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace ssp4simd
{
    namespace
    {
        constexpr std::size_t max_clients = 8;
        constexpr std::size_t max_writer_queue = 256;
        constexpr std::size_t max_writer_bytes = 4u * 1024u * 1024u;
        constexpr auto monitor_period = std::chrono::milliseconds(50);

        bool is_droppable(const json &event)
        {
            return event.value("event", "") == "time";
        }

        void set_nonblocking(int fd)
        {
            const int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags >= 0)
            {
                ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            }
        }

        // Restores the process umask on every exit path.
        struct UmaskGuard
        {
            mode_t previous;
            explicit UmaskGuard(mode_t mask) : previous(::umask(mask)) {}
            ~UmaskGuard() { ::umask(previous); }
        };

        std::string busy_line()
        {
            std::string line =
                make_error(nullptr, "server_busy", "too many clients").dump();
            line.push_back('\n');
            return line;
        }
    }

    struct Server::Client
    {
        explicit Client(int socket_fd) : fd(socket_fd) {}

        ~Client()
        {
            if (fd >= 0)
            {
                ::close(fd);
            }
        }

        void enqueue(std::string line, bool droppable)
        {
            if (dead || closing)
            {
                return;
            }
            const std::size_t bytes = line.size();
            if (out.size() >= max_writer_queue ||
                out_bytes + bytes > max_writer_bytes)
            {
                // A slow client may miss droppable time events; anything else
                // means the connection cannot keep up and is dropped.
                if (!droppable)
                {
                    dead = true;
                }
                return;
            }
            out_bytes += bytes;
            out.push_back(std::move(line));
        }

        int fd;
        LineFramer framer;
        std::deque<std::string> out;
        std::size_t out_offset = 0;
        std::size_t out_bytes = 0;
        bool closing = false;
        bool dead = false;
    };

    Server::Server(int listen_fd, std::string socket_path, Daemon daemon)
        : listen_fd_(listen_fd),
          socket_path_(std::move(socket_path)),
          daemon_(std::move(daemon))
    {
    }

    Server::~Server()
    {
        shutdown();
    }

    int Server::create_listening_socket(const std::string &path)
    {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;

        if (path.size() >= sizeof(address.sun_path))
        {
            throw std::runtime_error("socket path is too long: " + path);
        }
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
        {
            throw std::runtime_error(std::string("socket(): ") +
                                     std::strerror(errno));
        }

        // The socket file must never be world-writable, not even briefly
        // between bind() and chmod().
        UmaskGuard umask_guard(0077);

        // Bind first: only a path that is genuinely occupied needs probing.
        if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) <
            0)
        {
            const int bind_errno = errno;
            if (bind_errno != EADDRINUSE)
            {
                const std::string message =
                    std::string("bind(") + path + "): " +
                    std::strerror(bind_errno);
                ::close(fd);
                throw std::runtime_error(message);
            }

            const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (probe < 0)
            {
                const std::string message =
                    std::string("socket(): ") + std::strerror(errno);
                ::close(fd);
                throw std::runtime_error(message);
            }

            const bool live =
                ::connect(probe, reinterpret_cast<sockaddr *>(&address),
                          sizeof(address)) == 0;
            const int probe_errno = errno;
            ::close(probe);

            if (live)
            {
                ::close(fd);
                throw std::runtime_error(
                    "server_busy: another ssp4simd is already listening on " +
                    path);
            }
            if (probe_errno != ECONNREFUSED)
            {
                const std::string message =
                    "cannot probe existing socket path " + path + ": " +
                    std::strerror(probe_errno);
                ::close(fd);
                throw std::runtime_error(message);
            }

            if (::unlink(path.c_str()) != 0 && errno != ENOENT)
            {
                const std::string message =
                    "cannot remove stale socket path " + path + ": " +
                    std::strerror(errno);
                ::close(fd);
                throw std::runtime_error(message);
            }

            if (::bind(fd, reinterpret_cast<sockaddr *>(&address),
                       sizeof(address)) < 0)
            {
                const std::string message =
                    std::string("bind(") + path + "): " + std::strerror(errno);
                ::close(fd);
                throw std::runtime_error(message);
            }
        }

        if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) < 0)
        {
            const std::string message =
                std::string("chmod(") + path + "): " + std::strerror(errno);
            ::close(fd);
            ::unlink(path.c_str());
            throw std::runtime_error(message);
        }

        if (::listen(fd, 16) < 0)
        {
            const std::string message =
                std::string("listen(): ") + std::strerror(errno);
            ::close(fd);
            ::unlink(path.c_str());
            throw std::runtime_error(message);
        }

        set_nonblocking(fd);
        return fd;
    }

    void Server::run(const std::function<bool()> &external_stop)
    {
        using clock = std::chrono::steady_clock;

        auto next_tick = clock::now();

        for (;;)
        {
            if (shutdown_requested_ || external_stop())
            {
                break;
            }

            reap_clients();

            if (clock::now() >= next_tick)
            {
                daemon_.monitor_tick();
                broadcast_pending_events();
                next_tick = clock::now() + monitor_period;
            }

            std::vector<pollfd> descriptors;
            descriptors.reserve(clients_.size() + 1);

            pollfd listener{};
            listener.fd = listen_fd_;
            listener.events = POLLIN;
            descriptors.push_back(listener);

            for (const auto &client : clients_)
            {
                pollfd descriptor{};
                descriptor.fd = client->fd;
                descriptor.events = 0;
                if (!client->closing)
                {
                    descriptor.events |= POLLIN;
                }
                if (!client->out.empty())
                {
                    descriptor.events |= POLLOUT;
                }
                descriptors.push_back(descriptor);
            }

            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 next_tick - clock::now())
                                 .count();
            if (remaining < 0)
            {
                remaining = 0;
            }
            if (remaining > monitor_period.count())
            {
                remaining = monitor_period.count();
            }

            const int ready =
                ::poll(descriptors.data(), descriptors.size(),
                       static_cast<int>(remaining));
            if (ready < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }
            if (ready == 0)
            {
                continue;
            }

            const std::size_t polled = clients_.size();
            for (std::size_t index = 0; index < polled; ++index)
            {
                Client &client = *clients_[index];
                const short revents = descriptors[index + 1].revents;
                if (revents == 0)
                {
                    continue;
                }

                if ((revents & (POLLERR | POLLNVAL)) != 0)
                {
                    client.dead = true;
                    continue;
                }
                if ((revents & POLLIN) != 0 && !client.closing)
                {
                    read_client(client);
                }
                if (!client.dead && (revents & POLLOUT) != 0)
                {
                    flush_client(client);
                }
                if (!client.dead && (revents & POLLHUP) != 0 &&
                    client.out.empty())
                {
                    client.dead = true;
                }
            }

            if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0)
            {
                break;
            }
            if ((descriptors[0].revents & POLLIN) != 0 && !shutdown_requested_)
            {
                accept_clients();
            }

            reap_clients();
        }

        shutdown();
    }

    void Server::accept_clients()
    {
        for (;;)
        {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    return;
                }
                std::cerr << "ssp4simd: accept(): " << std::strerror(errno)
                          << '\n';
                return;
            }

            set_nonblocking(fd);

            if (clients_.size() >= max_clients)
            {
                const std::string line = busy_line();
                (void)::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
                ::close(fd);
                continue;
            }

            clients_.push_back(std::make_unique<Client>(fd));
        }
    }

    void Server::read_client(Client &client)
    {
        char buffer[64 * 1024];

        for (;;)
        {
            const ssize_t received = ::recv(client.fd, buffer, sizeof(buffer), 0);
            if (received > 0)
            {
                client.framer.feed(
                    buffer, static_cast<std::size_t>(received),
                    [this, &client](std::string_view line)
                    {
                        if (client.dead || client.closing)
                        {
                            return;
                        }

                        const auto parsed = parse_request(line);

                        json response;
                        if (!parsed.ok)
                        {
                            response = make_error(parsed.id, parsed.code,
                                                  parsed.message);
                        }
                        else
                        {
                            response = daemon_.handle(parsed.request);
                            const auto &command =
                                parsed.request["cmd"]
                                    .get_ref<const std::string &>();
                            if (response.value("ok", false) &&
                                command == "shutdown")
                            {
                                shutdown_requested_ = true;
                            }
                        }

                        std::string encoded;
                        try
                        {
                            encoded = response.dump();
                        }
                        catch (const std::exception &error)
                        {
                            encoded =
                                make_error(nullptr, "internal_error", error.what())
                                    .dump();
                        }
                        encoded.push_back('\n');
                        client.enqueue(std::move(encoded), false);
                        broadcast_pending_events();
                    },
                    [&client]
                    {
                        if (client.dead || client.closing)
                        {
                            return;
                        }
                        const auto response = make_error(
                            nullptr, "line_too_long",
                            "request line exceeds 1 MiB");
                        client.enqueue(response.dump() + "\n", false);
                        client.closing = true;
                    });

                if (client.dead || client.closing)
                {
                    return;
                }
                continue;
            }

            if (received == 0)
            {
                client.closing = true;
                return;
            }
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            client.dead = true;
            return;
        }
    }

    void Server::flush_client(Client &client)
    {
        while (!client.out.empty())
        {
            std::string &line = client.out.front();
            const ssize_t written =
                ::send(client.fd, line.data() + client.out_offset,
                       line.size() - client.out_offset, MSG_NOSIGNAL);
            if (written > 0)
            {
                client.out_offset += static_cast<std::size_t>(written);
                if (client.out_offset == line.size())
                {
                    client.out_bytes -= line.size();
                    client.out.pop_front();
                    client.out_offset = 0;
                }
                continue;
            }
            if (written < 0 && errno == EINTR)
            {
                continue;
            }
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                return;
            }
            client.dead = true;
            return;
        }
    }

    void Server::reap_clients()
    {
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                           [](const std::unique_ptr<Client> &client)
                           {
                               return client->dead ||
                                      (client->closing && client->out.empty());
                           }),
            clients_.end());
    }

    void Server::broadcast(const json &event)
    {
        std::string encoded;
        try
        {
            encoded = event.dump();
        }
        catch (const std::exception &)
        {
            return;
        }
        encoded.push_back('\n');

        const bool droppable = is_droppable(event);
        for (auto &client : clients_)
        {
            client->enqueue(encoded, droppable);
        }
    }

    void Server::broadcast_pending_events()
    {
        for (const auto &event : daemon_.collect_events())
        {
            broadcast(event);
        }
    }

    void Server::shutdown()
    {
        if (shutdown_complete_)
        {
            return;
        }
        shutdown_complete_ = true;

        // Stop and wait, then flush the terminal event before closing clients.
        daemon_.shutdown();
        broadcast_pending_events();

        if (listen_fd_ >= 0)
        {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(300);
        for (;;)
        {
            reap_clients();

            std::vector<pollfd> descriptors;
            for (const auto &client : clients_)
            {
                if (!client->out.empty())
                {
                    pollfd descriptor{};
                    descriptor.fd = client->fd;
                    descriptor.events = POLLOUT;
                    descriptors.push_back(descriptor);
                }
            }
            if (descriptors.empty())
            {
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                break;
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                      now)
                    .count();
            const int ready =
                ::poll(descriptors.data(), descriptors.size(),
                       static_cast<int>(remaining));
            if (ready < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }
            if (ready == 0)
            {
                break;
            }

            for (auto &client : clients_)
            {
                if (!client->out.empty())
                {
                    flush_client(*client);
                }
            }
        }

        clients_.clear();

        if (!socket_path_.empty())
        {
            ::unlink(socket_path_.c_str());
            socket_path_.clear();
        }
    }
}
