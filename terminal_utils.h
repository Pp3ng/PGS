#ifndef TERMINAL_UTILS_H
#define TERMINAL_UTILS_H

#include <string>
#include <array>

namespace TerminalUtils
{
    // ANSI escape codes for colors
    constexpr std::array<const char *, 9> COLORS = {
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

    constexpr const char *BOLD = "\033[1m";

    // Special symbols (updated)
    constexpr const char *CHECK_MARK = "✅"; // U+2705
    constexpr const char *CROSS_MARK = "❌"; // U+274C
    constexpr const char *INFO_MARK = "🔵";  // U+1F4E2
    constexpr const char *WARN_MARK = "⚠️";   // U+26A0

    inline std::string success(const std::string &msg)
    {
        return std::string(COLORS[3]) + CHECK_MARK + " " + msg + COLORS[0]; // GREEN
    }

    inline std::string error(const std::string &msg)
    {
        return std::string(COLORS[2]) + CROSS_MARK + " " + msg + COLORS[0]; // RED
    }

    inline std::string info(const std::string &msg)
    {
        return std::string(COLORS[5]) + INFO_MARK + " " + msg + COLORS[0]; // BLUE
    }

    inline std::string warning(const std::string &msg)
    {
        return std::string(COLORS[4]) + WARN_MARK + " " + msg + COLORS[0]; // YELLOW
    }

    inline std::string highlight(const std::string &msg)
    {
        return std::string(BOLD) + msg + COLORS[0]; // RESET
    }

    inline std::string step(int num, const std::string &msg)
    {
        return std::string(COLORS[6]) + "[Step " + std::to_string(num) + "] " + msg + COLORS[0]; // CYAN
    }
}

#endif // TERMINAL_UTILS_H
