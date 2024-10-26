#include <iostream>
#include <fstream>
#include <thread>
#include <vector>
#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <sstream>
#include <chrono>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <iomanip>
#include <csignal>

/**
 * Thread-safe logging system implementing the Singleton pattern.
 * Provides centralized logging capabilities with configurable log levels
 * and buffer sizes.
 */
class Log
{
public:
    // Log levels
    enum Level
    {
        INFO = 0,
        WARNING = 1,
        ERROR = 2
    };

    static Log *Instance()
    {
        static Log instance;
        return &instance;
    }

    void init(int level, const std::string &filename, int buffer_size)
    {
        log_level = level;
        log_file.open(filename, std::ios::out | std::ios::app);
        buffer.reserve(buffer_size);

        // Write header to log file
        WriteLog(INFO, "=== New Test Session Started ===");
    }

    void SetLevel(int level)
    {
        log_level = level;
    }

    void WriteLog(int level, const std::string &message)
    {
        if (level >= log_level)
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            auto now = std::chrono::system_clock::now();
            auto now_time = std::chrono::system_clock::to_time_t(now);
            log_file << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S")
                     << " [" << getLevelString(level) << "] "
                     << message << std::endl;
        }
    }

private:
    Log() {} // Private constructor for singleton
    ~Log()
    {
        if (log_file.is_open())
        {
            log_file.close();
        }
    }

    std::string getLevelString(int level)
    {
        switch (level)
        {
        case INFO:
            return "INFO";
        case WARNING:
            return "WARNING";
        case ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
        }
    }

    std::ofstream log_file;
    std::atomic<int> log_level{0};
    std::vector<std::string> buffer;
    std::mutex log_mutex;
};

// Macro for easier logging
#define LOG_INFO(format, ...)                           \
    do                                                  \
    {                                                   \
        std::stringstream ss;                           \
        ss << format << " " << __VA_ARGS__;             \
        Log::Instance()->WriteLog(Log::INFO, ss.str()); \
    } while (0)

/**
 * Thread Pool implementation for managing worker threads.
 * Handles task distribution and thread lifecycle management.
 */
class ThreadPool
{
public:
    ThreadPool(size_t threads) : stop(false)
    {
        for (size_t i = 0; i < threads; ++i)
        {
            workers.emplace_back([this]
                                 {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        condition.wait(lock, [this] { 
                            return stop || !tasks.empty(); 
                        });
                        
                        if (stop && tasks.empty()) {
                            return;
                        }
                        
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();
                } });
        }
    }

    ~ThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread &worker : workers)
        {
            worker.join();
        }
    }

    template <class F>
    void AddTask(F &&task)
    {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(task));
        }
        condition.notify_one();
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

/**
 * Main WebServer testing class that manages the load testing process.
 */
class WebServerTest
{
public:
    struct TestStats
    {
        std::atomic<int> success_count{0};
        std::atomic<int> fail_count{0};
        std::atomic<int> completed_count{0};
        std::atomic<long long> total_bytes_received{0};
        std::atomic<long long> total_response_time{0};
    };

    WebServerTest(const std::string &ip, int port, int num_threads, int num_requests, bool read_data = false)
        : server_ip(ip), server_port(port), thread_count(num_threads),
          request_count(num_requests), read_full_response(read_data) {}

    void RunTest()
    {
        ThreadPool pool(thread_count);
        auto start_time = std::chrono::high_resolution_clock::now();

        // Start test tasks
        for (int i = 0; i < thread_count; ++i)
        {
            pool.AddTask([this]
                         {
                for (int j = 0; j < request_count / thread_count; ++j) {
                    auto request_start = std::chrono::high_resolution_clock::now();
                    if (SendRequest()) {
                        stats.success_count++;
                        auto request_end = std::chrono::high_resolution_clock::now();
                        stats.total_response_time += std::chrono::duration_cast<std::chrono::milliseconds>(
                            request_end - request_start).count();
                    } else {
                        stats.fail_count++;
                    }
                    stats.completed_count++;
                } });
        }

        // Monitor progress
        MonitorProgress();

        // Calculate and display results
        auto end_time = std::chrono::high_resolution_clock::now();
        DisplayResults(start_time, end_time);
    }

private:
    bool SendRequest()
    {
        int sock = CreateSocket();
        if (sock < 0)
        {
            return false;
        }

        if (!ConnectToServer(sock))
        {
            close(sock);
            return false;
        }

        if (!SendHTTPRequest(sock))
        {
            close(sock);
            return false;
        }

        bool success = false;
        if (read_full_response)
        {
            success = ReceiveFullResponse(sock);
        }
        else
        {
            success = ReceiveHeaderOnly(sock);
        }

        close(sock);
        return success;
    }

    int CreateSocket()
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            LOG_INFO("Socket creation failed:", strerror(errno));
            return -1;
        }

        // Set non-blocking mode
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        return sock;
    }

    bool ConnectToServer(int sock)
    {
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

        int connect_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
        if (connect_result < 0)
        {
            if (errno != EINPROGRESS)
            {
                return false;
            }

            // Wait for connection with timeout
            fd_set write_fds;
            struct timeval timeout
            {
                1, 0
            }; // 1 second timeout
            FD_ZERO(&write_fds);
            FD_SET(sock, &write_fds);

            if (select(sock + 1, nullptr, &write_fds, nullptr, &timeout) <= 0)
            {
                return false;
            }
        }

        // Restore blocking mode
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);

        // Set socket timeouts
        struct timeval tv
        {
            1, 0
        }; // 1 second timeout
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        return true;
    }

    bool SendHTTPRequest(int sock)
    {
        std::string request = "GET / HTTP/1.1\r\n"
                              "Host: " +
                              server_ip + "\r\n"
                                          "Connection: close\r\n\r\n";

        return send(sock, request.c_str(), request.length(), 0) >= 0;
    }

    bool ReceiveHeaderOnly(int sock)
    {
        char buffer[1024];
        int bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received > 0)
        {
            stats.total_bytes_received += bytes_received;
            return true;
        }
        return false;
    }

    bool ReceiveFullResponse(int sock)
    {
        char buffer[4096];
        int total_bytes = 0;
        while (true)
        {
            int bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (bytes_received <= 0)
            {
                break;
            }
            total_bytes += bytes_received;
        }
        if (total_bytes > 0)
        {
            stats.total_bytes_received += total_bytes;
            return true;
        }
        return false;
    }

    void MonitorProgress()
    {
        while (stats.completed_count < request_count)
        {
            std::cout << "\rProgress: " << stats.completed_count * 100 / request_count << "% ("
                      << stats.completed_count << "/" << request_count << " requests)"
                      << " Success: " << stats.success_count
                      << " Failed: " << stats.fail_count
                      << " Bytes Received: " << stats.total_bytes_received
                      << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << std::endl;
    }

    void DisplayResults(
        const std::chrono::high_resolution_clock::time_point &start_time,
        const std::chrono::high_resolution_clock::time_point &end_time)
    {

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        double requests_per_second = (stats.success_count * 1000.0) / duration.count();
        double avg_response_time = stats.success_count > 0 ? static_cast<double>(stats.total_response_time) / stats.success_count : 0;

        std::cout << "\nTest Results:" << std::endl;
        std::cout << "Total Requests: " << request_count << std::endl;
        std::cout << "Successful Requests: " << stats.success_count << std::endl;
        std::cout << "Failed Requests: " << stats.fail_count << std::endl;
        std::cout << "Total Time: " << duration.count() << "ms" << std::endl;
        std::cout << "Requests per second: " << std::fixed << std::setprecision(2)
                  << requests_per_second << std::endl;
        std::cout << "Average Response Time: " << std::fixed << std::setprecision(2)
                  << avg_response_time << "ms" << std::endl;
        std::cout << "Total Data Received: " << (stats.total_bytes_received / 1024.0 / 1024.0)
                  << " MB" << std::endl;
    }

    std::string server_ip;
    int server_port;
    int thread_count;
    int request_count;
    bool read_full_response;
    TestStats stats;
};

int main(int argc, char *argv[])
{
    if (argc < 5 || argc > 6)
    {
        std::cout << "Usage: " << argv[0]
                  << " <ip> <port> <threads> <requests> [read_data=0/1]" << std::endl;
        std::cout << "Example: " << argv[0]
                  << " 127.0.0.1 8080 10 1000 1" << std::endl;
        return 1;
    }

    Log::Instance()->init(0, "webserver_test.log", 5000);

    std::string ip = argv[1];
    int port = std::stoi(argv[2]);
    int threads = std::stoi(argv[3]);
    int requests = std::stoi(argv[4]);
    bool read_data = (argc == 6) ? std::stoi(argv[5]) != 0 : false;

    LOG_INFO("Starting WebServer test with:",
             "IP=" << ip << " Port=" << port
                   << " Threads=" << threads
                   << " Requests=" << requests
                   << " ReadData=" << (read_data ? "true" : "false"));

    WebServerTest tester(ip, port, threads, requests, read_data);
    tester.RunTest();

    return 0;
}