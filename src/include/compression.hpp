#ifndef PGS_COMPRESSION_HPP
#define PGS_COMPRESSION_HPP

#include "middleware.hpp"
#include "logger.hpp"

// Middleware for compressing response data
class Compression : public Middleware
{
public:
    // Check if content should be compressed based on MIME type and length
    [[nodiscard]]
    static bool shouldCompress(const std::string &mimeType, size_t contentLength);

    // Process and compress the input data
    [[nodiscard]]
    std::string process(const std::string &data) override;

private:
    // Compress data using zlib
    [[nodiscard]]
    static std::string compressData(const std::string &data);

    // Non-compressible MIME types
    static const std::unordered_set<std::string> nonCompressibleTypes;

    // Compressible MIME types
    static const std::unordered_set<std::string> compressibleTypes;

    // Buffer size for compression (32KB)
    static constexpr size_t COMPRESSION_BUFFER_SIZE = 32768;

    // Minimum size for compression (1KB)
    static constexpr size_t MIN_COMPRESSION_SIZE = 1024;
};

#endif // PGS_COMPRESSION_HPP