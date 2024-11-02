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
        return false; // Minimum length for an asset (.css)

    // convert last 10 chars (max extension length) to lowercase once
    // this avoids converting the entire path
    alignas(8) char lastChars[10]; // Aligned for better memory access
    const size_t checkLen = std::min(pathLen, size_t(10));
    std::transform(path.end() - checkLen, path.end(), lastChars, ::tolower);

    std::string_view lastView(lastChars, checkLen);

    // Most common image types and web assets first (ordered by frequency)
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
        pos2 == std::string::npos) // Check if request is valid
    {
        return "/";
    }
    return request.substr(pos1 + 4, pos2 - (pos1 + 4));
}

void Http::sendResponse(int client_socket, const std::string &filePath,
                        const std::string &mimeType, int statusCode,
                        const std::string &clientIp, bool isIndex,
                        Middleware *middleware, Cache *cache)
{
    // Create a memory resource for this request
    std::pmr::monotonic_buffer_resource pool(64 * 1024); // 64KB initial size

    // Performance metrics
    auto startTime = std::chrono::steady_clock::now();
    size_t totalBytesSent = 0;

    // Socket options
    int cork = 1;
    SocketOptionGuard sockGuard(cork, client_socket);

    // Set socket options including TCP_CORK
    if (!setupSocketOptions(client_socket, cork, clientIp))
    {
        return;
    }

    // File content and cache handling
    std::pmr::vector<char> fileContent{&pool};
    size_t fileSize;
    time_t lastModified;
    bool cacheHit = false;

    // Try to get content from cache
    if (cache && statusCode == 200)
    {
        std::string cachedMimeType;
        cacheHit = cache->get(filePath, fileContent, cachedMimeType, lastModified);
        if (cacheHit)
        {
            fileSize = fileContent.size();
            Logger::getInstance()->info("Cache hit for: " + filePath, clientIp);
        }
    }

    // Handle file if not in cache
    FileGuard fileGuard;
    if (!cacheHit)
    {
        if (!handleFileContent(fileGuard, filePath, fileSize,
                               lastModified, clientIp))
        {
            return;
        }
    }

    // Compression handling
    bool isCompressed = false;
    std::pmr::string compressedContent{&pool};

    if (middleware && Compression::shouldCompress(mimeType, fileSize) &&
        mimeType.find("image/") == std::string::npos)
    {
        isCompressed =
            compressContent(middleware, fileSize, fileContent,
                            compressedContent, cacheHit, fileGuard, pool);
        if (isCompressed)
        {
            fileSize = compressedContent.size();
        }
    }

    // Generate response headers
    std::string headerStr = generateHeaders(statusCode, mimeType, fileSize,
                                            lastModified, isCompressed);

    // Send headers and content using writev
    totalBytesSent +=
        sendWithWritev(client_socket, headerStr, compressedContent, fileContent,
                       isCompressed, cacheHit, clientIp);

    // Handle large file transfer and caching
    if (!isCompressed && !cacheHit && fileGuard.get() != -1)
    {
        // Read file content once for both sending and caching
        std::pmr::vector<char> content{&pool};
        content.reserve(fileSize);
        bool readSuccess = false;

        // Try to read entire file into memory
        if (lseek(fileGuard.get(), 0, SEEK_SET) != -1)
        {
            // Use aligned buffer for optimal read performance
            std::unique_ptr<char[]> alignedBuffer(
                new (std::align_val_t{ALIGNMENT}) char[BUFFER_SIZE]);
            size_t totalRead = 0;

            // Read file in chunks
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

            readSuccess = (totalRead == fileSize);
        }

        // If successfully read the file, send content and update cache
        if (readSuccess)
        {
            // Send file content with retry logic
            size_t sent = 0;
            const char *data = content.data();
            size_t remaining = content.size();

            while (remaining > 0)
            {
                ssize_t byteSent = send(client_socket, data + sent, remaining, MSG_NOSIGNAL);
                if (byteSent <= 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        // Retry after short delay
                        std::this_thread::sleep_for(std::chrono::microseconds(1000));
                        continue;
                    }
                    Logger::getInstance()->error(
                        "Failed to send content: errno=" + std::to_string(errno), clientIp);
                    break;
                }
                sent += byteSent;
                remaining -= byteSent;
            }
            totalBytesSent += sent;

            // Update cache if needed
            if (cache && statusCode == 200)
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
        else
        {
            // Fallback to sendLargeFile if reading fails
            totalBytesSent += sendLargeFile(client_socket, fileGuard, fileSize, clientIp);
        }
    }

    // Record performance metrics
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
bool Http::handleFileContent(FileGuard &fileGuard, const std::string &filePath,
                             size_t &fileSize, time_t &lastModified,
                             const std::string &clientIp)
{
    // Open file with O_DIRECT for large files to bypass system cache
    int flags = O_RDONLY;
    struct stat statBuf;
    if (stat(filePath.c_str(), &statBuf) == 0 &&
        statBuf.st_size > 10 * 1024 * 1024) // 10MB threshold
    {
        flags |= O_DIRECT;
    }

    int fd = open(filePath.c_str(), flags);
    if (fd == -1 &&
        errno == EINVAL) // O_DIRECT not supported, fallback to normal open
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

    // Advise kernel about access pattern for optimal I/O performance
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
        // Use aligned buffer with RAII
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
    // Pre-allocate header string capacity
    std::string headerStr;
    headerStr.reserve(1024); // Reserve reasonable space for headers

    // Generate time strings
    char timeBuffer[128], lastModifiedBuffer[128];
    time_t now = time(nullptr);
    struct tm tmBuf;
    const struct tm *tm_info = gmtime_r(&now, &tmBuf); // Thread-safe version
    strftime(timeBuffer, sizeof(timeBuffer), "%a, %d %b %Y %H:%M:%S GMT",
             tm_info);

    tm_info = gmtime_r(&lastModified, &tmBuf);
    strftime(lastModifiedBuffer, sizeof(lastModifiedBuffer),
             "%a, %d %b %Y %H:%M:%S GMT", tm_info);

    // Status message lookup using array for better performance
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

    // Assemble response headers using string
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
size_t Http::sendWithWritev(int client_socket, const std::string &headerStr,
                            const std::pmr::string &compressedContent,
                            const std::pmr::vector<char> &fileContent,
                            bool isCompressed, bool cacheHit,
                            const std::string &clientIp)
{
    // Optimize writev using maximum allowed iovec structures
    std::array<struct iovec, MAX_IOV> iov;
    int iovcnt = 0;

    // Add header to iovec
    iov[iovcnt].iov_base = const_cast<char *>(headerStr.c_str());
    iov[iovcnt].iov_len = headerStr.size();
    iovcnt++;

    // Add content to iovec if compressed or cached
    if (isCompressed)
    {
        iov[iovcnt].iov_base = const_cast<char *>(compressedContent.c_str());
        iov[iovcnt].iov_len = compressedContent.size();
        iovcnt++;
    }
    else if (cacheHit)
    {
        iov[iovcnt].iov_base = const_cast<char *>(fileContent.data());
        iov[iovcnt].iov_len = fileContent.size();
        iovcnt++;
    }

    // Send headers and content using writev with retry logic
    size_t totalSent = 0;
    const size_t totalSize =
        headerStr.size() + (isCompressed ? compressedContent.size()
                                         : (cacheHit ? fileContent.size() : 0));

    while (totalSent < totalSize)
    {
        ssize_t sent = writev(client_socket, iov.data(), iovcnt);
        if (sent <= 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(1000)); // 1ms sleep
                continue;                             // retry
            }
            Logger::getInstance()->error(
                "Failed to send response: errno=" + std::to_string(errno), clientIp);
            return totalSent;
        }
        totalSent += sent;

        // Update iovec structures with zero-copy approach
        while (sent > 0 && iovcnt > 0)
        {
            if (static_cast<size_t>(sent) >= iov[0].iov_len)
            {
                sent -= iov[0].iov_len;
                iovcnt--;
                std::copy(iov.begin() + 1, iov.begin() + 1 + iovcnt, iov.begin());
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
size_t Http::sendLargeFile(int client_socket, const FileGuard &fileGuard,
                           size_t fileSize, const std::string &clientIp)
{
    size_t totalSent = 0;
    off_t offset = 0;
    bool useMmap = false;

    // Try sendfile with optimal chunk size and retry logic
    while (offset < static_cast<off_t>(fileSize))
    {
        size_t chunk = std::min(SENDFILE_CHUNK, fileSize - offset);
        ssize_t sent = sendfile(client_socket, fileGuard.get(), &offset, chunk);

        if (sent == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(1000)); // 1ms sleep
                continue;                             // retry
            }
            else if (errno == EINVAL || errno == ENOSYS)
            {
                useMmap = true;
                break;
            }
            Logger::getInstance()->error(
                "Failed to send file: errno=" + std::to_string(errno), clientIp);
            return totalSent;
        }
        else if (sent == 0)
        {
            break;
        }
        totalSent += sent;
    }

    // Fall back to mmap for large files with huge pages support
    if (useMmap)
    {
        int flags = MAP_PRIVATE;
        if (fileSize >= 2 * 1024 * 1024) // 2MB threshold for huge pages
        {
            flags |= MAP_HUGETLB;
        }

        void *mmapAddr =
            mmap(nullptr, fileSize, PROT_READ, flags, fileGuard.get(), 0);
        if (mmapAddr == MAP_FAILED && (flags & MAP_HUGETLB))
        {
            flags &= ~MAP_HUGETLB;
            mmapAddr = mmap(nullptr, fileSize, PROT_READ, flags, fileGuard.get(), 0);
        }

        if (mmapAddr == MAP_FAILED)
        {
            Logger::getInstance()->error(
                "Mmap failed: errno=" + std::to_string(errno), clientIp);
            return totalSent;
        }

        // RAII for mmap
        MMapGuard mmapGuard(mmapAddr, fileSize);
        madvise(mmapAddr, fileSize, MADV_SEQUENTIAL);

        const char *fileContent = static_cast<const char *>(mmapAddr);
        size_t remainingBytes = fileSize;
        size_t bytesSent = 0;

        while (remainingBytes > 0)
        {
            size_t chunk = std::min(BUFFER_SIZE, remainingBytes);
            ssize_t sent =
                send(client_socket, fileContent + bytesSent, chunk, MSG_NOSIGNAL);
            if (sent == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(1000)); // 1ms sleep
                    continue;                             // retry
                }
                Logger::getInstance()->error("Failed to send mmap data: errno=" +
                                                 std::to_string(errno),
                                             clientIp);
                return totalSent + bytesSent;
            }
            bytesSent += sent;
            remainingBytes -= sent;
        }
        totalSent += bytesSent;
    }

    return totalSent;
}