#ifndef PGS_PARSER_HPP
#define PGS_PARSER_HPP

#include "common.hpp"
#include "config.hpp"
#include "logger.hpp"

class Parser
{
public:
    // delete copy constructor and assignment operator
    Parser(const Parser &) = delete;
    Parser &operator=(const Parser &) = delete;

    // get singleton instance
    static Parser *getInstance()
    {
        static Parser instance;
        return &instance;
    }

    // parse configuration file
    [[nodiscard]] Config parseConfig(const std::string &configFilePath);

private:
    // private constructor for singleton
    Parser() = default;
};

#endif // PGS_PARSER_HPP