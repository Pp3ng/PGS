#include "http.hpp"
#include "compression.hpp"
// RAII wrappers for resource management
class Http::SocketOptionGuard
{
    int &cork;
    int client_socket;

public:
    SocketOptionGuard(int &c, int cs) : cork(c), client_socket(cs) {}
    ~SocketOptionGuard()
    {
        cork = 0;
        setsockopt(client_socket, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
    }
};

class Http::FileGuard
{
    int fd;

public:
    FileGuard() : fd(-1) {}
    explicit FileGuard(int f) : fd(f) {}
    ~FileGuard()
    {
        if (fd != -1)
            close(fd);
    }
    int get() const { return fd; }
    void reset(int f = -1)
    {
        if (fd != -1)
            close(fd);
        fd = f;
    }
};

class Http::MMapGuard
{
    void *addr;
    size_t length;

public:
    MMapGuard(void *a, size_t l) : addr(a), length(l) {}
    ~MMapGuard()
    {
        if (addr != MAP_FAILED)
            munmap(addr, length);
    }
};

[[nodiscard]]
bool Http::isAssetRequest(const std::string &path)
{
    // cache string length to avoid multiple calls
    const size_t pathLen = path.length();
    if (pathLen < 4)
        return false; // minimum length for an asset (.css)

    // convert last 10 chars (max extension length) to lowercase once
    // this avoids converting the entire path
    alignas(8) char lastChars[10]; // aligned for better memory access
    const size_t checkLen = std::min(pathLen, size_t(10));
    std::transform(path.end() - checkLen, path.end(), lastChars, ::tolower);

    std::string_view lastView(lastChars, checkLen);

    // most common image types and web assets first (ordered by frequency)
    static constexpr std::array<std::string_view, 16> commonExts = {
        ".jpg", ".png", ".gif", // most frequent image types
        ".jpeg", ".webp",       // less frequent image types
        ".css", ".js",          // essential web assets
        ".ico", ".svg",         // common icons
        ".woff2", ".woff",      // modern fonts first
        ".ttf",                 // legacy font
        ".mp4", ".webm",        // video files
        ".json", ".xml"         // data files
    };

    // fast path: check most common extensions first using simd-friendly loop
    const auto commonExtsSize = commonExts.size();
    for (size_t i = 0; i < commonExtsSize; ++i)
    {
        const auto &ext = commonExts[i];
        if (checkLen >= ext.length() &&
            lastView.compare(checkLen - ext.length(), ext.length(), ext) == 0)
        {
            return true;
        }
        // early exit after checking most common types
        if (i == 7 && pathLen > 6)
            break; // skip less common types for longer paths
    }

    // less common extensions - now includes previously separate ones
    static constexpr std::array<std::string_view, 5> rareExts = {
        ".eot", ".map", ".pdf", ".mp3", ".wav"};

    // check rare extensions only for specific path patterns
    if (pathLen > 8)
    { // only check for longer paths
        size_t dotPos = path.find_last_of('.');
        if (dotPos != std::string::npos)
        {
            std::string_view ext(lastChars + (dotPos - (pathLen - checkLen)),
                                 checkLen - (dotPos - (pathLen - checkLen)));
            if (std::any_of(rareExts.begin(), rareExts.end(),
                            [&](const auto &rareExt)
                            { return ext == rareExt; }))
            {
                return true;
            }
        }
    }

    // directory check optimization using string_view and branch prediction
    std::string_view pathView(path);

    // hot path: check most common asset directories first
    static constexpr std::array<std::string_view, 6> hotDirs = {
        "/img/", "/images/",   // image directories
        "/css/", "/js/",       // essential asset directories
        "/assets/", "/static/" // common resource directories
    };

    // use likely/unlikely for better branch prediction
    if (std::any_of(hotDirs.begin(), hotDirs.end(),
                    [&](const auto &dir)
                    { return pathView.starts_with(dir); }))
        [[likely]]
    {
        return true;
    }

    // cold path: less common directories
    static constexpr std::array<std::string_view, 3> coldDirs = {
        "/fonts/", "/media/", "/photos/"};

    // check cold directories only if hot directories didn't match
    if (pathLen > 7) [[unlikely]]
    {
        if (std::any_of(coldDirs.begin(), coldDirs.end(), [&](const auto &dir)
                        { return pathView.find(dir) != std::string_view::npos; }))
        {
            return true;
        }
    }

    return false;
}

[[nodiscard]]
std::string Http::getRequestPath(const std::string &request)
{
    size_t pos1 = request.find("GET ");
    size_t pos2 = request.find(" HTTP/");
    if (pos1 == std::string::npos ||
        pos2 == std::string::npos) // check if request is valid
    {
        return "/";
    }
    return request.substr(pos1 + 4, pos2 - (pos1 + 4));
}

[[nodiscard]]
bool Http::handleClientCache(int client_socket, const std::string &request,
                             time_t lastModified, const std::string &mimeType,
                             const std::string &filePath, const std::string &clientIp,
                             std::chrono::steady_clock::time_point startTime)
{
    if (checkClientCache(request, lastModified))
    {
        std::string headerStr = generateHeaders(304, mimeType, 0, lastModified, false);
        send(client_socket, headerStr.c_str(), headerStr.size(), MSG_NOSIGNAL);

        auto endTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            endTime - startTime);

        Logger::getInstance()->info(
            "[" + clientIp + "] Response sent: " +
            "status=304, path=" + filePath +
            ", cache=CLIENT" +
            ", time=" + std::to_string(duration.count()) + "µs" +
            ", bytes=" + std::to_string(headerStr.size()));
        return true;
    }
    return false;
}

void Http::sendResponse(int client_socket, const std::string &filePath,
                        const std::string &mimeType, int statusCode,
                        const std::string &clientIp, bool isIndex,
                        Middleware *middleware, Cache *cache,
                        const std::string &request)
{
    std::pmr::monotonic_buffer_resource pool(64 * 1024);
    auto startTime = std::chrono::steady_clock::now();
    size_t totalBytesSent = 0;

    // if is image, set cork to 0, otherwise to 1
    bool isImage = mimeType.find("image/") == 0;
    int cork = isImage ? 0 : 1;
    SocketOptionGuard sockGuard(cork, client_socket);

    if (!setupSocketOptions(client_socket, cork, clientIp))
    {
        return;
    }

    std::pmr::vector<char> fileContent{&pool};
    size_t fileSize;
    time_t lastModified = 0;
    bool cacheHit = false;

    // try to get content from cache
    if (cache && statusCode == 200) [[likely]]
    {
        std::pmr::vector<char> cachedContent{&pool};
        std::string cachedMimeType;
        time_t cachedLastModified;

        if (cache->get(filePath, cachedContent, cachedMimeType, cachedLastModified))
        {
            cacheHit = true;
            fileSize = cachedContent.size();
            lastModified = cachedLastModified;

            if (handleClientCache(client_socket, request, lastModified,
                                  cachedMimeType, filePath, clientIp, startTime))
            {
                return;
            }

            std::string headerStr = generateHeaders(statusCode, cachedMimeType,
                                                    fileSize, cachedLastModified, false);

            totalBytesSent = sendUnified(client_socket, headerStr, cachedContent, clientIp);

            auto endTime = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                endTime - startTime);

            Logger::getInstance()->info(
                "Cache hit sent: " + filePath +
                    ", size=" + std::to_string(cachedContent.size()) +
                    ", time=" + std::to_string(duration.count()) + "µs",
                clientIp);
            return;
        }
    }

    // cache miss - get file stats and handle client cache
    struct stat file_stat;
    if (stat(filePath.c_str(), &file_stat) == 0)
    {
        lastModified = file_stat.st_mtime;

        if (handleClientCache(client_socket, request, lastModified,
                              mimeType, filePath, clientIp, startTime))
        {
            return;
        }
    }

    // handle uncached content
    FileGuard fileGuard;
    if (!cacheHit)
    {
        if (!handleFileContent(fileGuard, filePath, fileSize,
                               lastModified, clientIp))
        {
            return;
        }
    }

    // compression handling
    bool isCompressed = false;
    std::pmr::string compressedContent{&pool};

    if (middleware && Compression::shouldCompress(mimeType, fileSize) &&
        mimeType.find("image/") == std::string::npos)
    {
        isCompressed = compressContent(middleware, fileSize, fileContent,
                                       compressedContent, cacheHit, fileGuard, pool);
        if (isCompressed)
        {
            fileSize = compressedContent.size();
        }
    }

    // generate response headers
    std::string headerStr = generateHeaders(statusCode, mimeType, fileSize,
                                            lastModified, isCompressed);

    // unified sending logic
    if (isCompressed)
    {
        // send compressed content using writev
        std::pmr::vector<char> compData{&pool};
        compData.assign(compressedContent.begin(), compressedContent.end());
        totalBytesSent = sendUnified(client_socket, headerStr, compData, clientIp);
    }
    else
    {
        // send file content directly
        totalBytesSent = sendUnified(client_socket, headerStr, fileGuard, fileSize, clientIp, isImage);

        if (cache && statusCode == 200 && !cacheHit &&
            ((!isImage && fileSize <= 2 * 1024 * 1024) || // 2MB for non-images
             (isImage && fileSize <= 512 * 1024)))        // 512KB for images
        {
            // read file for caching
            std::pmr::vector<char> content{&pool};
            if (readFileToMemory(fileGuard, fileSize, content, pool))
            {
                std::string keyTemp(filePath);
                std::string mimeTemp(mimeType);
                cache->set(std::move(keyTemp), std::move(content),
                           std::move(mimeTemp), lastModified);

                Logger::getInstance()->info(
                    "set cache: " + filePath +
                    " cache size: " + std::to_string(cache->size()) +
                    ", cache count: " + std::to_string(cache->count()));
            }
        }
    }

    // record performance metrics
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        endTime - startTime);

    if (isIndex)
    {
        Logger::getInstance()->info(
            "Response sent: status=" + std::to_string(statusCode) +
                ", path=" + filePath + ", size=" + std::to_string(fileSize) +
                ", type=" + mimeType + ", cache=" + (cacheHit ? "HIT" : "MISS") +
                ", time=" + std::to_string(duration.count()) + "µs" +
                ", bytes=" + std::to_string(totalBytesSent),
            clientIp);
    }
}

[[nodiscard]]
bool Http::setupSocketOptions(int client_socket, int cork,
                              const std::string &clientIp)
{
    auto setSocketOption = [&](int level, int optname, const void *optval,
                               socklen_t optlen)
    {
        if (setsockopt(client_socket, level, optname, optval, optlen) < 0)
        {
            Logger::getInstance()->error("Failed to set socket option: " +
                                             std::string(strerror(errno)),
                                         clientIp);
            close(client_socket);
            return false;
        }
        return true;
    };

    return setSocketOption(SOL_SOCKET, SO_KEEPALIVE, &SocketSettings::keepAlive,
                           sizeof(SocketSettings::keepAlive)) &&
           setSocketOption(IPPROTO_TCP, TCP_KEEPIDLE, &SocketSettings::keepIdle,
                           sizeof(SocketSettings::keepIdle)) &&
           setSocketOption(IPPROTO_TCP, TCP_KEEPINTVL,
                           &SocketSettings::keepInterval,
                           sizeof(SocketSettings::keepInterval)) &&
           setSocketOption(IPPROTO_TCP, TCP_KEEPCNT, &SocketSettings::keepCount,
                           sizeof(SocketSettings::keepCount)) &&
           setSocketOption(IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
}

[[nodiscard]]
bool Http::checkClientCache(const std::string &request, time_t lastModified)
{
    // find if-modified-since header
    size_t pos = request.find("If-Modified-Since: ");
    if (pos == std::string::npos)
    {
        return false;
    }

    // parse the date
    pos += 19; // length of "If-Modified-Since: "
    size_t endPos = request.find("\r\n", pos);
    if (endPos == std::string::npos)
    {
        return false;
    }

    std::string dateStr = request.substr(pos, endPos - pos);
    struct tm tm = {};
    if (strptime(dateStr.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm) == nullptr)
    {
        return false;
    }

    time_t clientCacheTime = timegm(&tm);
    return clientCacheTime >= lastModified;
}

[[nodiscard]]
bool Http::handleFileContent(FileGuard &fileGuard, const std::string &filePath,
                             size_t &fileSize, time_t &lastModified,
                             const std::string &clientIp)
{
    // open file with O_DIRECT for large files to bypass system cache
    int flags = O_RDONLY;
    struct stat statBuf;
    if (stat(filePath.c_str(), &statBuf) == 0 &&
        statBuf.st_size > 10 * 1024 * 1024) // 10mb threshold
    {
        flags |= O_DIRECT;
    }

    int fd = open(filePath.c_str(), flags);
    if (fd == -1 &&
        errno == EINVAL) // o_direct not supported, fallback to normal open
    {
        fd = open(filePath.c_str(), O_RDONLY);
    }

    if (fd == -1)
    {
        Logger::getInstance()->error(
            "Failed to open file: errno=" + std::to_string(errno), clientIp);
        return false;
    }
    fileGuard.reset(fd);

    // advise kernel about access pattern for optimal I/O performance
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    struct stat file_stat;
    if (fstat(fd, &file_stat) == -1)
    {
        Logger::getInstance()->error("Fstat failed: errno=" + std::to_string(errno),
                                     clientIp);
        return false;
    }

    fileSize = file_stat.st_size;
    lastModified = file_stat.st_mtime;

    return true;
}

[[nodiscard]]
bool Http::compressContent(Middleware *middleware, size_t fileSize,
                           std::pmr::vector<char> &fileContent,
                           std::pmr::string &compressedContent, bool cacheHit,
                           const FileGuard &fileGuard,
                           std::pmr::monotonic_buffer_resource &pool)
{
    if (cacheHit)
    {
        compressedContent = middleware->process(
            std::string(fileContent.begin(), fileContent.end()));
    }
    else
    {
        // use aligned buffer with RAII
        struct AlignedBuffer
        {
            void *ptr;
            AlignedBuffer(size_t size, size_t alignment) : ptr(nullptr)
            {
                if (posix_memalign(&ptr, alignment, size) != 0)
                {
                    throw std::bad_alloc();
                }
            }
            ~AlignedBuffer() { free(ptr); }
            void *get() { return ptr; }
        } alignedBuffer(BUFFER_SIZE, ALIGNMENT);

        std::pmr::vector<char> buffer{&pool};
        buffer.reserve(fileSize);

        size_t totalRead = 0;
        while (totalRead < fileSize)
        {
            ssize_t bytesRead = read(fileGuard.get(), alignedBuffer.get(),
                                     std::min(BUFFER_SIZE, fileSize - totalRead));
            if (bytesRead <= 0)
                break;
            buffer.insert(buffer.end(), static_cast<char *>(alignedBuffer.get()),
                          static_cast<char *>(alignedBuffer.get()) + bytesRead);
            totalRead += bytesRead;
        }

        compressedContent =
            middleware->process(std::string(buffer.begin(), buffer.end()));
    }
    return true;
}

[[nodiscard]]
std::string Http::generateHeaders(int statusCode, const std::string &mimeType,
                                  size_t fileSize, time_t lastModified,
                                  bool isCompressed)
{
    // pre-allocate header string capacity
    std::string headerStr;
    headerStr.reserve(1024); // reserve reasonable space for headers

    // generate time strings
    char timeBuffer[128], lastModifiedBuffer[128];
    time_t now = time(nullptr);
    struct tm tmBuf;
    const struct tm *tm_info = gmtime_r(&now, &tmBuf); // thread-safe version
    strftime(timeBuffer, sizeof(timeBuffer), "%a, %d %b %Y %H:%M:%S GMT",
             tm_info);

    tm_info = gmtime_r(&lastModified, &tmBuf);
    strftime(lastModifiedBuffer, sizeof(lastModifiedBuffer),
             "%a, %d %b %Y %H:%M:%S GMT", tm_info);

    // status message lookup using array for better performance
    static const char *const STATUS_MESSAGES[] = {
        "OK",                    // 200
        nullptr,                 // 201-299
        nullptr,                 // 300-303
        "Not Modified",          // 304
        nullptr,                 // 305-399
        "Bad Request",           // 400
        "Unauthorized",          // 401
        nullptr,                 // 402
        "Forbidden",             // 403
        "Not Found",             // 404
        nullptr,                 // 405-499
        "Internal Server Error", // 500
        nullptr,                 // 501-502
        "Service Unavailable",   // 503
    };

    const char *statusMessage;
    if (statusCode >= 200 &&
        statusCode <
            static_cast<int>(sizeof(STATUS_MESSAGES) / sizeof(char *)) + 200)
    {
        statusMessage = STATUS_MESSAGES[statusCode - 200];
        if (!statusMessage)
            statusMessage = "Unknown Status";
    }
    else
    {
        statusMessage = "Unknown Status";
    }

    // assemble response headers using string
    headerStr = "HTTP/1.1 " + std::to_string(statusCode) + " " + statusMessage +
                "\r\n"
                "Server: RobustHTTP/1.0\r\n"
                "Date: " +
                std::string(timeBuffer) +
                "\r\n"
                "Content-Type: " +
                mimeType +
                "\r\n"
                "Content-Length: " +
                std::to_string(fileSize) +
                "\r\n"
                "Last-Modified: " +
                std::string(lastModifiedBuffer) +
                "\r\n"
                "Connection: keep-alive\r\n"
                "Keep-Alive: timeout=60, max=1000\r\n"
                "Accept-Ranges: bytes\r\n"
                "Cache-Control: public, max-age=31536000\r\n"
                "X-Content-Type-Options: nosniff\r\n"
                "X-Frame-Options: SAMEORIGIN\r\n"
                "X-XSS-Protection: 1; mode=block\r\n";

    if (isCompressed)
    {
        headerStr += "Content-Encoding: gzip\r\n"
                     "Vary: Accept-Encoding\r\n";
    }
    headerStr += "\r\n";

    return headerStr;
}

[[nodiscard]]
size_t Http::sendUnified(int client_socket, const std::string &headerStr,
                         const std::pmr::vector<char> &content,
                         const std::string &clientIp)
{
    // use writev for headers + content in memory
    std::array<struct iovec, 2> iov;
    iov[0].iov_base = const_cast<char *>(headerStr.c_str());
    iov[0].iov_len = headerStr.size();
    iov[1].iov_base = const_cast<char *>(content.data());
    iov[1].iov_len = content.size();

    size_t totalSize = headerStr.size() + content.size();
    size_t totalSent = 0;
    int iovcnt = 2;

    while (totalSent < totalSize && iovcnt > 0)
    {
        ssize_t sent = writev(client_socket, iov.data(), iovcnt);
        if (sent <= 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(1000));
                continue;
            }
            Logger::getInstance()->error(
                "Failed to send response: errno=" + std::to_string(errno), clientIp);
            break;
        }
        totalSent += sent;

        // update iovec structures
        while (sent > 0 && iovcnt > 0)
        {
            if (static_cast<size_t>(sent) >= iov[0].iov_len)
            {
                sent -= iov[0].iov_len;
                iovcnt--;
                if (iovcnt > 0)
                {
                    iov[0] = iov[1];
                }
            }
            else
            {
                iov[0].iov_base = static_cast<char *>(iov[0].iov_base) + sent;
                iov[0].iov_len -= sent;
                break;
            }
        }
    }
    return totalSent;
}

[[nodiscard]]
size_t Http::sendUnified(int client_socket, const std::string &headerStr,
                         const FileGuard &fileGuard, size_t fileSize,
                         const std::string &clientIp, bool isImage)
{
    // Use larger threshold for images to prefer writev
    static constexpr size_t SMALL_FILE_THRESHOLD = 1024 * 1024; // 1MB
    static constexpr size_t IMAGE_THRESHOLD = 4 * 1024 * 1024;  // 4MB for images
    static constexpr size_t SENDFILE_CHUNK = 256 * 1024;        // 256KB chunks

    size_t threshold = isImage ? IMAGE_THRESHOLD : SMALL_FILE_THRESHOLD;
    size_t totalSent = 0;

    if (fileSize <= threshold)
    {
        // for small files, read to memory and use writev
        std::vector<char> content(fileSize);
        if (pread(fileGuard.get(), content.data(), fileSize, 0) == static_cast<ssize_t>(fileSize))
        {
            std::pmr::vector<char> pmrContent(content.begin(), content.end());
            totalSent = sendUnified(client_socket, headerStr, pmrContent, clientIp);
        }
        else
        {
            Logger::getInstance()->error("Failed to read small file", clientIp);
        }
    }
    else
    {
        // for large files, send headers first, then use chunked sendfile
        ssize_t headerSent = send(client_socket, headerStr.c_str(), headerStr.size(), MSG_NOSIGNAL);
        if (headerSent <= 0)
        {
            Logger::getInstance()->error("Failed to send headers", clientIp);
            return 0;
        }
        totalSent += headerSent;

        // use chunked sendfile for better responsiveness
        off_t offset = 0;
        while (offset < static_cast<off_t>(fileSize))
        {
            size_t chunkSize = std::min(SENDFILE_CHUNK, fileSize - offset);
            ssize_t sent = sendfile(client_socket, fileGuard.get(), &offset, chunkSize);
            if (sent <= 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    continue;
                }
                Logger::getInstance()->error(
                    "Sendfile failed: errno=" + std::to_string(errno), clientIp);
                break;
            }
            totalSent += sent;

            if (isImage && offset % (1024 * 1024) == 0) // Every 1MB for images
            {
                std::this_thread::yield();
            }
        }
    }
    return totalSent;
}

[[nodiscard]]
bool Http::readFileToMemory(const FileGuard &fileGuard, size_t fileSize,
                            std::pmr::vector<char> &content,
                            [[maybe_unused]] std::pmr::monotonic_buffer_resource &pool)
{
    content.reserve(fileSize);

    // align buffer
    std::unique_ptr<char[]> alignedBuffer(
        new (std::align_val_t{ALIGNMENT}) char[BUFFER_SIZE]);

    size_t totalRead = 0;
    if (lseek(fileGuard.get(), 0, SEEK_SET) == -1)
    {
        return false;
    }

    while (totalRead < fileSize)
    {
        ssize_t bytesRead = read(fileGuard.get(), alignedBuffer.get(),
                                 std::min(BUFFER_SIZE, fileSize - totalRead));
        if (bytesRead <= 0)
            break;

        content.insert(content.end(), alignedBuffer.get(),
                       alignedBuffer.get() + bytesRead);
        totalRead += bytesRead;
    }

    return totalRead == fileSize;
}
