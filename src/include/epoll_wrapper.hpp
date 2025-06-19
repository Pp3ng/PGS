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
                                    "Failed to create epoll file descriptor");
        }
        
        // Initialize bitset for efficient fd tracking
        fd_status.reset();
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
        : epoll_fd(other.epoll_fd), fd_status(std::move(other.fd_status))
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
            fd_status = std::move(other.fd_status);
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
        if (static_cast<size_t>(fd) >= MAX_FDS)
            return false;

        // Use bitset for efficient status tracking
        if (fd_status.test(fd))
            return true; // already added

        struct epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0)
        {
            fd_status.set(fd);
            return true;
        }
        return false;
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
        if (static_cast<size_t>(fd) >= MAX_FDS)
            return false;

        if (!fd_status.test(fd))
            return true; // already removed

        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == 0)
        {
            fd_status.reset(fd);
            return true;
        }
        return false;
    }

    template <typename Iterator>
    inline size_t batch_operation(Iterator begin, Iterator end, int op,
                                  uint32_t events = 0) noexcept
    {
        // early return if empty range
        if (begin == end)
        {
            return 0;
        }

        size_t success_count = 0;
        static constexpr size_t BATCH_SIZE = 64;

        // pre-allocate events array for batch processing
        struct epoll_event evs[BATCH_SIZE];
        size_t current_batch = 0;

        // track failed operations for retry
        std::array<int, BATCH_SIZE> failed_fds;
        size_t failed_count = 0;

        for (Iterator it = begin; it != end; ++it)
        {
            const int fd = *it;

            // validate file descriptor
            if (fd < 0 || static_cast<size_t>(fd) >= MAX_FDS)
            {
                continue;
            }

            // skip if already in desired state
            if (op == EPOLL_CTL_ADD && fd_status.test(fd))
            {
                ++success_count;
                continue;
            }
            if (op == EPOLL_CTL_DEL && !fd_status.test(fd))
            {
                ++success_count;
                continue;
            }

            // prepare event structure
            if (op != EPOLL_CTL_DEL)
            {
                evs[current_batch].events = events;
                evs[current_batch].data.fd = fd;
            }

            // perform epoll operation
            if (epoll_ctl(epoll_fd, op, fd,
                          (op == EPOLL_CTL_DEL) ? nullptr : &evs[current_batch]) == 0)
            {
                ++success_count;

                // update fd status efficiently
                if (op == EPOLL_CTL_ADD)
                    fd_status.set(fd);
                else if (op == EPOLL_CTL_DEL)
                    fd_status.reset(fd);
            }
            else if (errno != EBADF && errno != ENOENT)
            {
                // store failed fd for retry if error is recoverable
                if (failed_count < BATCH_SIZE)
                    failed_fds[failed_count++] = fd;
            }

            // reset batch counter when reaching batch size
            if (++current_batch == BATCH_SIZE)
            {
                current_batch = 0;
            }
        }

        // retry failed operations once
        for (size_t i = 0; i < failed_count; ++i)
        {
            const int fd = failed_fds[i];
            struct epoll_event ev;
            ev.events = events;
            ev.data.fd = fd;

            if (epoll_ctl(epoll_fd, op, fd,
                          (op == EPOLL_CTL_DEL) ? nullptr : &ev) == 0)
            {
                ++success_count;

                if (op == EPOLL_CTL_ADD)
                    fd_status.set(fd);
                else if (op == EPOLL_CTL_DEL)
                    fd_status.reset(fd);
            }
        }

        return success_count;
    }
    
    [[nodiscard]] inline bool
    is_monitored(int fd) const noexcept // check if fd is monitored by epoll
    {
        if (static_cast<size_t>(fd) >= MAX_FDS)
            return false;
        return fd_status.test(fd);
    }

private:
    int epoll_fd;                                   // epoll file descriptor
    static constexpr size_t MAX_FDS = 65536;        // maximum number of file descriptors
    std::bitset<MAX_FDS> fd_status;                 // efficient fd status tracking using bitset

    // Static assertions for compile-time checks
    static_assert(
        sizeof(int) >= sizeof(void *) ||
            sizeof(int64_t) >=
                sizeof(void *), // check if int or int64_t can store a pointer
        "Platform must support storing pointers in epoll_data");
};

#endif // PGS_EPOLL_WRAPPER_HPP