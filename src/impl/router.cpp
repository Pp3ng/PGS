#include "router.hpp"

#include "router.hpp"

Router::Router(const std::string &staticFolder)
    : staticFolder(staticFolder)
{
    Logger::getInstance()->success("Router initialized with static folder: " + staticFolder);
}

[[nodiscard]]
std::string Router::getMimeType(const std::string &path)
{
    // cache frequently used MIME types for quick access
    static const std::string TEXT_HTML = "text/html";
    static const std::string TEXT_PLAIN = "text/plain";
    static const std::string IMAGE_JPEG = "image/jpeg";

    // define sets of extensions that map to the same MIME type
    static const std::set<std::string_view> htmlExts = {".html", ".htm"};
    static const std::set<std::string_view> jpegExts = {".jpg", ".jpeg"};

    // define valid extensions set for quick validation
    static const std::set<std::string_view> validExts =
        {
            ".html", ".htm", ".css", ".js", ".json", ".png", ".jpg", ".jpeg",
            ".gif", ".svg", ".ico", ".txt", ".pdf", ".xml", ".zip", ".woff",
            ".woff2", ".ttf", ".eot", ".mp3", ".mp4", ".webm", ".webp"};

    // define MIME type mappings for validated extensions
    static const std::unordered_map<std::string_view, std::string_view>
        mimeTypes = {{".css", "text/css"},
                     {".js", "application/javascript"},
                     {".json", "application/json"},
                     {".png", "image/png"},
                     {".gif", "image/gif"},
                     {".svg", "image/svg+xml"},
                     {".ico", "image/x-icon"},
                     {".txt", "text/plain"},
                     {".pdf", "application/pdf"},
                     {".xml", "application/xml"},
                     {".zip", "application/zip"},
                     {".woff", "font/woff"},
                     {".woff2", "font/woff2"},
                     {".ttf", "font/ttf"},
                     {".eot", "application/vnd.ms-fontobject"},
                     {".mp3", "audio/mpeg"},
                     {".mp4", "video/mp4"},
                     {".webm", "video/webm"},
                     {".webp", "image/webp"}};

    // find last dot in path
    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos)
    {
        return TEXT_PLAIN;
    }

    // extract extension and convert to lowercase using string_view
    // this avoids string allocation for the extension
    char lastChars[10]; // max extension length
    const size_t extLen = std::min(path.length() - dotPos, size_t(10));
    std::transform(path.begin() + dotPos, path.begin() + dotPos + extLen,
                   lastChars, ::tolower);
    std::string_view ext(lastChars, extLen);

    // quick validation check
    if (!validExts.contains(ext))
    {
        return TEXT_PLAIN;
    }

    // fast path for most common types
    if (htmlExts.contains(ext))
    {
        return TEXT_HTML;
    }
    if (jpegExts.contains(ext))
    {
        return IMAGE_JPEG;
    }

    // look up MIME type for other extensions
    auto it = mimeTypes.find(ext);
    return it != mimeTypes.end() ? std::string(it->second) : TEXT_PLAIN;
}

void Router::route(const std::string &path, int client_socket,
                   const std::string &clientIp, Middleware *middleware,
                   Cache *cache)
{
    // pre-allocate string capacity to avoid reallocation
    // +11 accounts for potential "/index.html" addition
    std::string filePath;
    filePath.reserve(staticFolder.size() + path.size() + 11);
    filePath = staticFolder;
    filePath += path;

    // normalize path to lowercase for case-insensitive comparison
    // using a separate string to preserve original path for logging
    std::string normalizedPath = path;
    std::transform(normalizedPath.begin(), normalizedPath.end(),
                   normalizedPath.begin(), ::tolower);

    // use string_view for efficient string operations
    // this avoids string copies during comparisons
    std::string_view pathView(normalizedPath);

    // cache filesystem check results to avoid redundant calls
    bool isDir = fs::is_directory(filePath);
    bool isIndex = (pathView == "/index.html" || pathView == "/" || isDir);
    bool isAsset = Http::isAssetRequest(normalizedPath);

    // append index.html for directory paths
    if (isDir)
    {
        filePath += "/index.html";
    }

    // log non-asset index requests
    if (!isAsset && isIndex)
    {
        Logger::getInstance()->info("Processing request: " + path, clientIp);
    }

    // cache for 404.html content
    // using static variables and std::once_flag for thread-safe initialization
    static std::string cached404Content;
    static std::once_flag flag404;

    // check if file exists and handle 404 errors
    if (!fs::exists(filePath) || (isDir && !fs::exists(filePath)))
    {
        // log warning for non-asset requests
        if (!isAsset)
        {
            Logger::getInstance()->warning("File not found: " + filePath, clientIp);
        }

        static bool has404File = false;
        // initialize 404.html content only once
        // this is thread-safe due to std::call_once
        std::call_once(flag404, [&]()
                       {
      std::ifstream file("404.html", std::ios::binary);
      if (file) {
        has404File = true;
        // efficient file reading using iterators
        cached404Content.assign(std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>());
      } });

        // send simple 404 response if custom 404.html doesn't exist
        if (!has404File)
        {
            // static response for 404 errors
            // this avoids creating the string repeatedly
            static const char *SIMPLE_404_RESPONSE = "HTTP/1.1 404 Not Found\r\n"
                                                     "Content-Type: text/plain\r\n"
                                                     "Content-Length: 9\r\n"
                                                     "\r\n"
                                                     "Not Found";
            // use MSG_NOSIGNAL to prevent SIGPIPE
            send(client_socket, SIMPLE_404_RESPONSE, strlen(SIMPLE_404_RESPONSE),
                 MSG_NOSIGNAL);
            return;
        }

        // static response header for 404 errors
        // this is shared across all 404 responses
        static const std::string RESPONSE_404_HEADER = "HTTP/1.1 404 Not Found\r\n"
                                                       "Content-Type: text/html\r\n"
                                                       "Connection: close\r\n"
                                                       "Content-Length: ";

        // use writev for efficient response sending
        // this minimizes the number of system calls
        struct iovec iov[4]; // array of io vectors for writev
        std::string contentLength = std::to_string(cached404Content.size());
        static const char *CRLF = "\r\n\r\n";

        // set up io vectors for response components
        iov[0].iov_base = const_cast<char *>(RESPONSE_404_HEADER.data());
        iov[0].iov_len = RESPONSE_404_HEADER.size();
        iov[1].iov_base = const_cast<char *>(contentLength.data());
        iov[1].iov_len = contentLength.size();
        iov[2].iov_base = const_cast<char *>(CRLF);
        iov[2].iov_len = 4; // Length of "\r\n\r\n"
        iov[3].iov_base = const_cast<char *>(cached404Content.data());
        iov[3].iov_len = cached404Content.size();

        // send all components in a single system call
        if (writev(client_socket, iov, 4) == -1)
        {
            Logger::getInstance()->error("Failed to send 404 response: " +
                                             std::string(strerror(errno)),
                                         clientIp);
        }
        return;
    }

    // get mime type and send response
    // note: getmimetype is assumed to be case-insensitive
    std::string mimeType = getMimeType(filePath);

    // send the response using the optimized http::sendresponse method
    // the !isasset && isindex parameter determines whether to log the response
    Http::sendResponse(client_socket, filePath, mimeType, 200, clientIp,
                       !isAsset && isIndex, middleware, cache);
}