#ifndef PGS_EPOLL_WRAPPER_HPP
#define PGS_EPOLL_WRAPPER_HPP

#include "common.hpp"

class EpollWrapper
{
public:
    // constructor with minimal but essential error handling
    EpollWrapper() : epoll_fd(epoll_create1(EPOLL_CLOEXEC))
    {
        if (epoll_fd == -1)
        {
            throw std::system_error(errno, std::system_category(),
                                    "epoll_create1 failed");
        }
    }

    ~EpollWrapper() noexcept
    {
        if (epoll_fd != -1)
            close(epoll_fd);
    }

    // delete copy operations
    EpollWrapper(const EpollWrapper &) = delete;
    EpollWrapper &operator=(const EpollWrapper &) = delete;

    // optimized move operations
    inline EpollWrapper(EpollWrapper &&other) noexcept
        : epoll_fd(other.epoll_fd)
    {
        other.epoll_fd = -1;
    }

    inline EpollWrapper &
    operator=(EpollWrapper &&other) noexcept // move assignment operator
    {
        if (this != &other)
        {
            if (epoll_fd != -1)
                close(epoll_fd);
            epoll_fd = std::exchange(other.epoll_fd, -1);
        }
        return *this;
    }

    [[nodiscard]] inline int get() const noexcept // get epoll file descriptor
    {
        return epoll_fd;
    }

    [[nodiscard]] inline int wait(struct epoll_event *events, int maxEvents,
                                  int timeout) noexcept
    {
        return epoll_wait(epoll_fd, events, maxEvents, timeout);
    }

    [[nodiscard]] inline bool add(int fd, uint32_t events) noexcept // add fd to epoll set with
                                                                    // optimized error handling
    {

        struct epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    [[nodiscard]] inline bool modify(int fd,
                                     uint32_t events) noexcept // modify fd in epoll set with
                                                               // optimized error handling
    {
        struct epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0;
    }

    [[nodiscard]] inline bool remove(int fd) noexcept // remove fd from epoll set with optimized error handling
    {

        return epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == 0;
    }

    template <typename Iterator>
    inline size_t batch_operation(Iterator begin, Iterator end, int op,
                                  uint32_t events = 0) noexcept
    {
        size_t success_count = 0;
        struct epoll_event ev;
        ev.events = events;

        for (Iterator it = begin; it != end; ++it)
        {
            const int fd = *it; // get file descriptor
            ev.data.fd = fd;
            if (epoll_ctl(epoll_fd, op, fd, (op == EPOLL_CTL_DEL) ? nullptr : &ev) ==
                0)
            {
                ++success_count;
            }
        }
        return success_count;
    }

    [[nodiscard]] inline bool
    is_monitored(int fd) const noexcept // check if fd is monitored by epoll
    {
        struct epoll_event ev;
        return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) != -1 || errno != ENOENT;
    }

private:
    int epoll_fd; // epoll file descriptor

    // Static assertions for compile-time checks
    static_assert(
        sizeof(int) >= sizeof(void *) ||
            sizeof(int64_t) >=
                sizeof(void *), // check if int or int64_t can store a pointer
        "Platform must support storing pointers in epoll_data");
};

#endif // PGS_EPOLL_WRAPPER_HPP