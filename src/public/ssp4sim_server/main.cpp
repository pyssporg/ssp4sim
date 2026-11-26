#include "daemon.hpp"
#include "server.hpp"

#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef SSP4SIMD_VERSION
#define SSP4SIMD_VERSION "unknown"
#endif

namespace
{
    volatile sig_atomic_t g_signal_received = 0;

    void handle_signal(int)
    {
        g_signal_received = 1;
    }

    void install_signal_handlers()
    {
        struct sigaction action
        {
        };
        action.sa_handler = &handle_signal;
        sigemptyset(&action.sa_mask);
        // No SA_RESTART: poll() must return EINTR so the loop can exit.
        action.sa_flags = 0;
        ::sigaction(SIGINT, &action, nullptr);
        ::sigaction(SIGTERM, &action, nullptr);
    }

    void print_help(const char *program)
    {
        std::cout
            << "Usage: " << program << " --config <path> --socket <path>\n"
            << "       " << program << " -h | --help\n"
            << "       " << program << " -v | --version\n"
            << "\n"
            << "Run an SSP4SIM simulation as a newline-delimited JSON daemon.\n"
            << "\n"
            << "Options:\n"
            << "  --config <path>   Simulation JSON configuration file (required).\n"
            << "  --socket <path>   AF_UNIX stream socket to listen on (required).\n"
            << "  -h, --help        Show this help message and exit.\n"
            << "  -v, --version     Show the daemon version and exit.\n";
    }

    struct Options
    {
        std::string config_path;
        std::string socket_path;
    };
}

int main(int argc, char *argv[])
{
    Options options;

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];

        if (argument == "-h" || argument == "--help")
        {
            print_help(argv[0]);
            return 0;
        }
        if (argument == "-v" || argument == "--version")
        {
            std::cout << SSP4SIMD_VERSION << '\n';
            return 0;
        }
        if (argument == "--config")
        {
            if (index + 1 >= argc)
            {
                std::cerr << "ssp4simd: --config requires a path\n";
                return 2;
            }
            options.config_path = argv[++index];
            continue;
        }
        if (argument == "--socket")
        {
            if (index + 1 >= argc)
            {
                std::cerr << "ssp4simd: --socket requires a path\n";
                return 2;
            }
            options.socket_path = argv[++index];
            continue;
        }

        std::cerr << "ssp4simd: unknown argument '" << argument << "'\n";
        print_help(argv[0]);
        return 2;
    }

    if (options.config_path.empty() || options.socket_path.empty())
    {
        std::cerr << "ssp4simd: --config and --socket are required\n";
        print_help(argv[0]);
        return 2;
    }

    try
    {
        ssp4simd::Daemon daemon(options.config_path);

        // Installed after the daemon (and its runner) so this handler wins for
        // SIGINT/SIGTERM, and before the socket exists so a signal can always
        // take the clean unlink path.
        install_signal_handlers();

        const int listen_fd =
            ssp4simd::Server::create_listening_socket(options.socket_path);

        ssp4simd::Server server(listen_fd, options.socket_path,
                                std::move(daemon));

        server.run([] { return g_signal_received != 0; });
    }
    catch (const std::exception &error)
    {
        std::cerr << "ssp4simd: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
