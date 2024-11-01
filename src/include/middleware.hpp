#ifndef PGS_MIDDLEWARE_HPP
#define PGS_MIDDLEWARE_HPP

#include "common.hpp"

// abstract base class for middleware components
class Middleware
{
public:
    // virtual destructor for proper cleanup in derived classes
    virtual ~Middleware() = default;

    // process the input data and return the processed result
    virtual std::string process(const std::string &data) = 0;

    // disable copy operations
    Middleware(const Middleware &) = delete;
    Middleware &operator=(const Middleware &) = delete;

    // enable move operations
    Middleware(Middleware &&) = default;
    Middleware &operator=(Middleware &&) = default;

protected:
    // protected constructor to prevent direct instantiation
    Middleware() = default;
};

#endif // PGS_MIDDLEWARE_HPP