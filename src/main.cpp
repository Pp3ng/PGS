#include "common.hpp"
#include "server.hpp"
#include "parser.hpp"
#include "logger.hpp"
#include "config.hpp"

std::atomic<bool> running(true); // flag to control main loop

void signalHandler([[maybe_unused]] int signal)
{
    running = false; // set running flag to false
}

auto main(void) -> int
{
    // set up signal handlers with SA_RESTART flag
    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    try
    {
        Config config = Parser::getInstance()->parseConfig("pgs_conf.json");

        // set higher priority for server process
        if (nice(-10) == -1 && errno != 0)
        {
            Logger::getInstance()->warning("Failed to set process priority: " +
                                           std::string(strerror(errno)));
        }

        // create server instance
        std::unique_ptr<Server> server = std::make_unique<Server>(
            config.port,
            config.staticFolder,
            config.threadCount,
            config.rateLimit.maxRequests,
            config.rateLimit.timeWindow,
            config.cache.sizeMB,
            config.cache.maxAgeSeconds);

        // pin server thread to first CPU core
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);

        std::thread serverThread([&]()
                                 { server->start(); });
        pthread_setaffinity_np(serverThread.native_handle(), sizeof(cpu_set_t), &cpuset);

        // main loop with shorter sleep time for more responsive shutdown
        while (running)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // high priority shutdown thread
        std::thread shutdownThread([&]()
                                   {
            pthread_setschedprio(pthread_self(), -10);
            server->stop(); });

        // join threads
        if (shutdownThread.joinable())
            shutdownThread.join();
        if (serverThread.joinable())
            serverThread.join();

        // explicitly reset server to ensure proper cleanup
        server.reset();
    }
    catch (const std::exception &e)
    {
        Logger::getInstance()->error("Fatal error: " + std::string(e.what()));
        return EXIT_FAILURE;
    }

    Logger::getInstance()->info("Server shut down successfully");
    Logger::destroyInstance();
    return EXIT_SUCCESS;
}