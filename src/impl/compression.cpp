#include "compression.hpp"

// Initialize static member variables
const std::unordered_set<std::string> Compression::nonCompressibleTypes = {
    "image/png",
    "image/gif",
    "image/svg+xml",
    "image/x-icon",
    "image/webp",
    "audio/mpeg",
    "video/mp4",
    "video/webm",
    "application/zip",
    "font/woff",
    "font/woff2",
    "font/ttf",
    "application/vnd.ms-fontobject"};

const std::unordered_set<std::string> Compression::compressibleTypes = {
    "text/",
    "application/javascript",
    "application/json",
    "application/xml",
    "application/x-yaml",
    "application/x-www-form-urlencoded"};

bool Compression::shouldCompress(const std::string &mimeType, size_t contentLength)
{
    // return false if it's a non-compressible type or content length is less than 1KB
    if (nonCompressibleTypes.count(mimeType) > 0 || contentLength < MIN_COMPRESSION_SIZE)
    {
        return false;
    }

    // check if MIME type is compressible
    return std::any_of(compressibleTypes.begin(), compressibleTypes.end(),
                       [&](const std::string &type)
                       {
                           return mimeType.find(type) == 0; // Check for prefix match
                       });
}

std::string Compression::process(const std::string &data)
{
    std::string compressed = compressData(data);
    if (!compressed.empty()) // check if compression was successful
    {
        Logger::getInstance()->info(
            "Compressed data: " + std::to_string(data.size()) + " -> " +
            std::to_string(compressed.size()));
        return compressed;
    }
    return data; // ensure a string is returned in all cases
}

std::string Compression::compressData(const std::string &data)
{
    z_stream zs;                // create a z_stream object for compression
    memset(&zs, 0, sizeof(zs)); // zero-initialize z_stream structure

    // initialize the zlib compression
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, // set compression level
                     Z_DEFLATED,                 // use deflate compression method
                     15 | 16,                    // 15 | 16 for gzip encoding
                     8,                          // set window size
                     Z_DEFAULT_STRATEGY) !=      // use default compression strategy
        Z_OK)
    {
        throw std::runtime_error("Failed to initialize zlib");
    }

    // set input data for compression
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data())); // input data
    zs.avail_in = data.size();                                               // size of input data

    int ret;                                 // variable to hold return status of compression
    char outbuffer[COMPRESSION_BUFFER_SIZE]; // buffer for compressed output
    std::string compressed;                  // string to hold final compressed data

    // compress data in a loop until all data is processed
    do
    {
        zs.next_out = reinterpret_cast<Bytef *>(outbuffer); // output buffer for compressed data
        zs.avail_out = COMPRESSION_BUFFER_SIZE;             // size of output buffer

        ret = deflate(&zs, Z_FINISH); // perform compression operation

        if (compressed.size() < zs.total_out) // append compressed data to output string
        {
            compressed.append(outbuffer, zs.total_out - compressed.size());
        }
    } while (ret == Z_OK); // continue until no more data to compress

    // check if compression completed successfully
    if (ret != Z_STREAM_END)
    {
        deflateEnd(&zs); // clean up z_stream object
        throw std::runtime_error("Failed to compress data");
    }

    deflateEnd(&zs);   // clean up and free resources allocated by zlib
    return compressed; // return compressed data
}