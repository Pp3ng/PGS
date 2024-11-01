#ifndef PGS_LOGGER_HPP
#define PGS_LOGGER_HPP

#include "common.hpp"

class Logger
{
private:
    // Define log levels using enum class for type safety and better semantics
    enum class LogLevel
    {
        INFO,
        WARNING,
        ERROR,
        SUCCESS
    };

    // ANSI escape codes for colors
    static constexpr std::array<const char *, 9> COLORS = {
        "\033[0m",  // RESET
        "\033[30m", // BLACK
        "\033[31m", // RED
        "\033[32m", // GREEN
        "\033[33m", // YELLOW
        "\033[34m", // BLUE
        "\033[35m", // MAGENTA
        "\033[36m", // CYAN
        "\033[37m", // WHITE
    };

    static constexpr const char *BOLD = "\033[1m";

    // Special symbols
    static constexpr const char *CHECK_MARK = "✅";
    static constexpr const char *CROSS_MARK = "❌";
    static constexpr const char *INFO_MARK = "🔵";
    static constexpr const char *WARN_MARK = "⚠️";

    // Constant string views for log levels
    static constexpr std::string_view LOG_LEVELS[] = {
        "INFO",
        "WARNING",
        "ERROR",
        "SUCCESS"};

    // Enhanced log message structure
    struct LogMessage
    {
        std::string message;
        LogLevel level;
        std::string ip;
        std::chrono::system_clock::time_point timestamp;

        LogMessage(std::string msg, LogLevel lvl, std::string clientIp)
            : message(std::move(msg)), level(lvl), ip(std::move(clientIp)),
              timestamp(std::chrono::system_clock::now()) {}
    };

    // Memory management and synchronization members
    std::pmr::monotonic_buffer_resource pool{64 * 1024};
    std::pmr::deque<LogMessage> messageQueue{&pool};

    mutable std::shared_mutex mutex;
    std::ofstream logFile;
    std::condition_variable_any queueCV;
    std::jthread loggerThread;
    std::atomic<bool> running{true};

    std::optional<std::chrono::steady_clock::time_point> lastEventWaitLog;
    bool isWaitingForEvents{false};

    static constexpr auto EVENT_WAIT_LOG_INTERVAL = std::chrono::seconds(5);

    static inline std::unique_ptr<Logger> instance;
    static inline std::once_flag initFlag;

    // Private helper functions
    [[nodiscard]] static std::string formatSuccess(const std::string &msg);
    [[nodiscard]] static std::string formatError(const std::string &msg);
    [[nodiscard]] static std::string formatInfo(const std::string &msg);
    [[nodiscard]] static std::string formatWarning(const std::string &msg);
    [[nodiscard]] static std::string formatStep(int num, const std::string &msg);
    [[nodiscard]] std::string formatLogMessage(const LogMessage &msg);

    Logger();
    void processLogs(std::stop_token st);
    void writeLogMessage(const LogMessage &msg);

public:
    static Logger *getInstance();
    static void destroyInstance();

    void log(std::string_view message, LogLevel level = LogLevel::INFO,
             std::string_view ip = "-");
    void error(std::string_view message, std::string_view ip = "-");
    void warning(std::string_view message, std::string_view ip = "-");
    void success(std::string_view message, std::string_view ip = "-");
    void info(std::string_view message, std::string_view ip = "-");
    void step(int num, std::string_view message, std::string_view ip = "-");

    ~Logger();

    // Delete copy and move operations
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger(Logger &&) = delete;
    Logger &operator=(Logger &&) = delete;
};

#endif // PGS_LOGGER_HPP