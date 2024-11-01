#include "parser.hpp"

Config Parser::parseConfig(const std::string &configFilePath)
{
    // log start of configuration reading process
    Logger::getInstance()->info("Reading configuration from: " + configFilePath);

    // open configuration file
    std::ifstream file(configFilePath);
    if (!file.is_open())
    {
        Logger::getInstance()->error("Could not open config file: " + configFilePath);
        throw std::runtime_error("Could not open config file!");
    }

    // parse JSON configuration
    json configJson;
    try
    {
        file >> configJson;
    }
    catch (const json::parse_error &e)
    {
        Logger::getInstance()->error("JSON parsing error: " + std::string(e.what()));
        throw std::runtime_error("Failed to parse configuration file");
    }

    // check if all required fields exist and are not null
    const std::array<std::pair<std::string, std::string>, 7>
        requiredFields = {{{"port", ""},
                           {"static_folder", ""},
                           {"thread_count", ""},
                           {"rate_limit.max_requests", "rate_limit"},
                           {"rate_limit.time_window", "rate_limit"},
                           {"cache.size_mb", "cache"},
                           {"cache.max_age_seconds", "cache"}}};

    for (const auto &[field, parent] : requiredFields)
    {
        if (parent.empty())
        {
            if (!configJson.contains(field) || configJson[field].is_null())
            {
                Logger::getInstance()->error("Missing or null field: " + field);
                throw std::runtime_error("Incomplete configuration file");
            }
        }
        else
        {
            const auto &fieldParts = field.substr(parent.length() + 1);
            if (!configJson.contains(parent) || configJson[parent].is_null() ||
                !configJson[parent].contains(fieldParts) ||
                configJson[parent][fieldParts].is_null())
            {
                Logger::getInstance()->error("Missing or null field: " + field);
                throw std::runtime_error("Incomplete configuration file");
            }
        }
    }

    // create a Config object and populate it with values from JSON
    Config config;
    config.port = configJson["port"];
    config.staticFolder = configJson["static_folder"];
    config.threadCount = configJson["thread_count"];
    config.rateLimit.maxRequests = configJson["rate_limit"]["max_requests"];
    config.rateLimit.timeWindow = configJson["rate_limit"]["time_window"];
    config.cache.sizeMB = configJson["cache"]["size_mb"].get<size_t>();
    config.cache.maxAgeSeconds = configJson["cache"]["max_age_seconds"].get<int>();

    // validate configuration values
    const auto validateConfig = [](const Config &cfg)
    {
        // validate port number
        if (cfg.port <= 0 || cfg.port > 65535)
        {
            throw std::runtime_error("Invalid port number: " + std::to_string(cfg.port));
        }

        // validate static folder path
        if (!fs::exists(cfg.staticFolder))
        {
            throw std::runtime_error("Static folder does not exist: " + cfg.staticFolder);
        }

        // validate thread count
        if (cfg.threadCount <= 0 || cfg.threadCount > 1000)
        {
            throw std::runtime_error("Invalid thread count: " + std::to_string(cfg.threadCount));
        }

        // validate rate limit settings
        if (cfg.rateLimit.maxRequests <= 0)
        {
            throw std::runtime_error("Invalid max requests: " +
                                     std::to_string(cfg.rateLimit.maxRequests));
        }
        if (cfg.rateLimit.timeWindow <= 0)
        {
            throw std::runtime_error("Invalid time window: " +
                                     std::to_string(cfg.rateLimit.timeWindow));
        }

        // validate cache settings
        if (cfg.cache.sizeMB == 0 ||
            cfg.cache.sizeMB > std::numeric_limits<size_t>::max() / (1024 * 1024))
        {
            throw std::runtime_error("Invalid cache size: " +
                                     std::to_string(cfg.cache.sizeMB) + " MB");
        }
        if (cfg.cache.maxAgeSeconds <= 0)
        {
            throw std::runtime_error("Invalid cache max age: " +
                                     std::to_string(cfg.cache.maxAgeSeconds) + " seconds");
        }
    };

    try
    {
        validateConfig(config);
    }
    catch (const std::runtime_error &e)
    {
        Logger::getInstance()->error(e.what());
        throw;
    }

    // log successful configuration loading
    Logger::getInstance()->success("Configuration loaded successfully");

    return config;
}