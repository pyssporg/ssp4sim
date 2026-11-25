#pragma once

#include "daemon.hpp"
#include "protocol.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ssp4simd
{
    /// AF_UNIX stream server. A single poll()-driven thread owns the listening
    /// socket, every client socket, the daemon, and the monitor tick. Clients
    /// are non-blocking; each owns an input framer and an output line queue.
    class Server
    {
    public:
        Server(int listen_fd, std::string socket_path, Daemon daemon);
        ~Server();

        Server(const Server &) = delete;
        Server &operator=(const Server &) = delete;

        /// Run until shutdown is requested or external_stop() becomes true.
        void run(const std::function<bool()> &external_stop);

        /// Bind a fresh listener at path. A live server at the path is an
        /// error; a stale socket/file is removed.
        static int create_listening_socket(const std::string &path);

    private:
        struct Client;

        void accept_clients();
        void read_client(Client &client);
        void flush_client(Client &client);
        void reap_clients();

        void broadcast(const json &event);
        void broadcast_pending_events();
        void request_shutdown();

        /// Close clients, drain their output, then unlink the socket.
        void shutdown();

        int listen_fd_ = -1;
        std::string socket_path_;
        Daemon daemon_;

        bool shutdown_requested_ = false;
        bool shutdown_complete_ = false;
        std::vector<std::unique_ptr<Client>> clients_;
    };
}
