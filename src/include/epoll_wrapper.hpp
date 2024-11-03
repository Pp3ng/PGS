#ifndef PGS_EPOLL_WRAPPER_HPP
#define PGS_EPOLL_WRAPPER_HPP

#include "common.hpp"

class EpollWrapper
{
public:
    // constructor with minimal but essential error handling
    EpollWrapper() : epoll_fd(epoll_create1(EPOLL_CLOEXEC)),
                     fd_status(new std::atomic<bool>[MAX_FDS])
    {
        if (epoll_fd == -1)
        {
            throw std::system_error(errno, std::system_category(),
                                    "Failed to create epoll file descriptor");
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
        if (static_cast<size_t>(fd) >= MAX_FDS)
            return false;

        bool expected = false;
        if (!fd_status[fd].compare_exchange_strong(expected, true))
            return true;

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

        if (static_cast<size_t>(fd) >= MAX_FDS)
            return false;

        bool expected = true;
        if (!fd_status[fd].compare_exchange_strong(expected, false))
            return true;

        return epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == 0;
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

                // update fd status for non-delete operations
                if (op != EPOLL_CTL_DEL && static_cast<size_t>(fd) < MAX_FDS)
                {
                    fd_status[fd].store(true, std::memory_order_release);
                }
            }
            else if (errno != EBADF && errno != ENOENT)
            {
                // store failed fd for retry if error is recoverable
                failed_fds[failed_count++] = fd;
            }

            // reset batch counter when reaching batch size
            if (++current_batch == BATCH_SIZE)
            {
                current_batch = 0;
            }
        }

        // retry failed operations once
        if (failed_count > 0)
        {
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

                    if (op != EPOLL_CTL_DEL && static_cast<size_t>(fd) < MAX_FDS)
                    {
                        fd_status[fd].store(true, std::memory_order_release);
                    }
                }
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
    int epoll_fd;                                   // epoll file descriptor
    static constexpr size_t MAX_FDS = 65536;        // maximum number of file descriptors
    std::unique_ptr<std::atomic<bool>[]> fd_status; // status of file descriptors

    // Static assertions for compile-time checks
    static_assert(
        sizeof(int) >= sizeof(void *) ||
            sizeof(int64_t) >=
                sizeof(void *), // check if int or int64_t can store a pointer
        "Platform must support storing pointers in epoll_data");
};

#endif // PGS_EPOLL_WRAPPER_HPP