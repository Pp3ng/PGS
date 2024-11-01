#include "logger.hpp"

// Helper functions for formatting with colors
std::string Logger::formatSuccess(const std::string &msg)
{
    return std::string(COLORS[3]) + CHECK_MARK + " " + msg + COLORS[0];
}

std::string Logger::formatError(const std::string &msg)
{
    return std::string(COLORS[2]) + CROSS_MARK + " " + msg + COLORS[0];
}

std::string Logger::formatInfo(const std::string &msg)
{
    return std::string(COLORS[5]) + INFO_MARK + " " + msg + COLORS[0];
}

std::string Logger::formatWarning(const std::string &msg)
{
    return std::string(COLORS[4]) + WARN_MARK + " " + msg + COLORS[0];
}

std::string Logger::formatStep(int num, const std::string &msg)
{
    return std::string(COLORS[6]) + "[Step " + std::to_string(num) + "] " + msg + COLORS[0];
}

Logger::Logger()
{
    logFile.open("pgs.log", std::ios::app);
    loggerThread = std::jthread([this](std::stop_token st)
                                { processLogs(st); });
    writeLogMessage({{"Logger initialized"}, LogLevel::SUCCESS, "-"});
}

void Logger::processLogs(std::stop_token st)
{
    while (!st.stop_requested())
    {
        std::vector<LogMessage> messages;
        messages.reserve(100);

        {
            std::shared_lock lock(mutex);
            auto pred = [this]
            {
                return !messageQueue.empty() || !running;
            };

            if (queueCV.wait_for(lock, std::chrono::seconds(1), pred)) // wait for 1 second
            {
                while (!messageQueue.empty() && messages.size() < 100)
                {
                    messages.push_back(std::move(messageQueue.front())); // move message to vector
                    messageQueue.pop_front();                            // remove message from queue
                }
            }
        }

        if (!messages.empty())
        {
            std::unique_lock lock(mutex);
            for (const auto &msg : messages)
            {
                writeLogMessage(msg);
            }
            logFile.flush();
        }
    }
}

void Logger::writeLogMessage(const LogMessage &msg)
{
    std::string fileMessage = formatLogMessage(msg);
    logFile << fileMessage << std::endl;

    std::string consoleMessage;
    switch (msg.level)
    {
    case LogLevel::ERROR:
        consoleMessage = formatError(fileMessage);
        break;
    case LogLevel::WARNING:
        consoleMessage = formatWarning(fileMessage);
        break;
    case LogLevel::SUCCESS:
        consoleMessage = formatSuccess(fileMessage);
        break;
    default:
        consoleMessage = formatInfo(fileMessage);
        break;
    }
    std::cout << consoleMessage << std::endl;
}

std::string Logger::formatLogMessage(const LogMessage &msg)
{
    auto time = std::chrono::system_clock::to_time_t(msg.timestamp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  msg.timestamp.time_since_epoch()) %
              1000;

    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S]",
                  std::localtime(&time));

    std::ostringstream oss;
    oss << timestamp << "."
        << std::setfill('0') << std::setw(3) << ms.count()
        << " [" << LOG_LEVELS[static_cast<int>(msg.level)] << "] "
        << "[" << msg.ip << "] "
        << msg.message;

    return oss.str();
}

Logger *Logger::getInstance()
{
    std::call_once(initFlag, []()
                   { instance = std::unique_ptr<Logger>(new Logger()); });
    return instance.get();
}

void Logger::log(std::string_view message, LogLevel level, std::string_view ip)
{
    if (message == "Waiting for events...")
    {
        std::shared_lock lock(mutex);
        auto now = std::chrono::steady_clock::now();

        if (!isWaitingForEvents ||
            !lastEventWaitLog ||
            (now - *lastEventWaitLog) >= EVENT_WAIT_LOG_INTERVAL)
        {
            isWaitingForEvents = true;
            lastEventWaitLog = now;
        }
        else
        {
            return;
        }
    }
    else
    {
        isWaitingForEvents = false;
    }

    {
        std::unique_lock lock(mutex);
        messageQueue.emplace_back(std::string(message), level, std::string(ip));
    }
    queueCV.notify_one();
}

void Logger::error(std::string_view message, std::string_view ip)
{
    log(message, LogLevel::ERROR, ip);
}

void Logger::warning(std::string_view message, std::string_view ip)
{
    log(message, LogLevel::WARNING, ip);
}

void Logger::success(std::string_view message, std::string_view ip)
{
    log(message, LogLevel::SUCCESS, ip);
}

void Logger::info(std::string_view message, std::string_view ip)
{
    log(message, LogLevel::INFO, ip);
}

void Logger::step(int num, std::string_view message, std::string_view ip)
{
    log(formatStep(num, std::string(message)), LogLevel::INFO, ip);
}

void Logger::destroyInstance()
{
    instance.reset();
}

Logger::~Logger()
{
    running = false;
    queueCV.notify_all();
    if (logFile.is_open())
    {
        logFile.close();
    }
}