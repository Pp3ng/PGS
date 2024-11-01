#ifndef PGS_HTTP_HPP
#define PGS_HTTP_HPP

#include "common.hpp"
#include "cache.hpp"
#include "logger.hpp"
#include "middleware.hpp"

class Http
{
public:
    static std::string getRequestPath(const std::string &request);
    static void sendResponse(int client_socket, const std::string &content,
                             const std::string &mimeType, int statusCode,
                             const std::string &clientIp, bool isIndex = false,
                             Middleware *middleware = nullptr,
                             Cache *cache = nullptr);
    static bool isAssetRequest(const std::string &path);

private:
    // Constants for optimized I/O
    static constexpr size_t BUFFER_SIZE = 65536;
    static constexpr size_t ALIGNMENT = 512;
    static constexpr size_t SENDFILE_CHUNK = 1048576;
    static constexpr int MAX_IOV = IOV_MAX;

    // Socket option settings
    struct SocketSettings
    {
        static constexpr int keepAlive = 1;
        static constexpr int keepIdle = 60;
        static constexpr int keepInterval = 10;
        static constexpr int keepCount = 3;
    };

    // RAII wrappers
    class SocketOptionGuard;
    class FileGuard;
    class MMapGuard;

    // Helper functions
    static bool setupSocketOptions(int client_socket, int cork,
                                   const std::string &clientIp);
    static bool handleFileContent(FileGuard &fileGuard,
                                  const std::string &filePath,
                                  size_t &fileSize, time_t &lastModified,
                                  const std::string &clientIp);
    static bool compressContent(Middleware *middleware,
                                size_t fileSize,
                                std::pmr::vector<char> &fileContent,
                                std::pmr::string &compressedContent,
                                bool cacheHit, const FileGuard &fileGuard,
                                std::pmr::monotonic_buffer_resource &pool);
    static std::string generateHeaders(int statusCode,
                                       const std::string &mimeType,
                                       size_t fileSize, time_t lastModified,
                                       bool isCompressed);
    static size_t sendWithWritev(int client_socket, const std::string &headerStr,
                                 const std::pmr::string &compressedContent,
                                 const std::pmr::vector<char> &fileContent,
                                 bool isCompressed, bool cacheHit,
                                 const std::string &clientIp);
    static size_t sendLargeFile(int client_socket, const FileGuard &fileGuard,
                                size_t fileSize, const std::string &clientIp);
};

#endif // PGS_HTTP_HPP