#ifndef TERMINAL_UTILS_H
#define TERMINAL_UTILS_H

#pragma once
#include <string>

namespace TerminalUtils
{
    // ANSI escape codes for colors
    const std::string RESET = "\033[0m";
    const std::string BLACK = "\033[30m";
    const std::string RED = "\033[31m";
    const std::string GREEN = "\033[32m";
    const std::string YELLOW = "\033[33m";
    const std::string BLUE = "\033[34m";
    const std::string MAGENTA = "\033[35m";
    const std::string CYAN = "\033[36m";
    const std::string WHITE = "\033[37m";
    const std::string BOLD = "\033[1m";

    // Special symbols
    const std::string CHECK_MARK = "✓";
    const std::string CROSS_MARK = "✗";
    const std::string INFO_MARK = "ℹ";
    const std::string WARN_MARK = "⚠";

    inline std::string success(const std::string &msg)
    {
        return GREEN + CHECK_MARK + " " + msg + RESET;
    }

    inline std::string error(const std::string &msg)
    {
        return RED + CROSS_MARK + " " + msg + RESET;
    }

    inline std::string info(const std::string &msg)
    {
        return BLUE + INFO_MARK + " " + msg + RESET;
    }

    inline std::string warning(const std::string &msg)
    {
        return YELLOW + WARN_MARK + " " + msg + RESET;
    }

    inline std::string highlight(const std::string &msg)
    {
        return BOLD + msg + RESET;
    }

    inline std::string step(int num, const std::string &msg)
    {
        return CYAN + "[Step " + std::to_string(num) + "] " + msg + RESET;
    }
}

#endif // TERMINAL_UTILS_H