#ifndef PGS_ROUTER_HPP
#define PGS_ROUTER_HPP

#include "common.hpp"
#include "http.hpp"
#include "middleware.hpp"
#include "cache.hpp"
#include "logger.hpp"

namespace fs = std::filesystem;

class Router
{
public:
    explicit Router(const std::string &staticFolder);

    void route(const std::string &path, int client_socket,
               const std::string &clientIp, Middleware *middleware,
               Cache *cache, const std::string &request);

private:
    std::string staticFolder;                                       // path to static files
    [[nodiscard]] std::string getMimeType(const std::string &path); // get MIME type based on file extension
};

#endif // PGS_ROUTER_HPP