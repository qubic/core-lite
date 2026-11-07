#pragma once

#if defined(__linux__) && !defined(LITE_WASM_SC)

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace tickForkControl
{
    inline constexpr char promoteTag = 'P';
    inline constexpr char retireTag = 'R';

    enum class ChildAction
    {
        Promote,
        Retire,
    };

    struct ChildCommand
    {
        ChildAction action;
        unsigned int targetTick;
    };

    class BspRetireHandoff
    {
    public:
        enum class State
        {
            Idle,
            Requested,
            ShutdownRequested,
            Running,
            Succeeded,
            Failed,
        };

        bool requestAndWait(unsigned int timeoutMs, bool shutDownAfterCommit = false)
        {
            const State requestState = shutDownAfterCommit ? State::ShutdownRequested : State::Requested;
            State expected = State::Idle;
            if (!_state.compare_exchange_strong(expected, requestState, std::memory_order_acq_rel))
            {
                return false;
            }

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            for (;;)
            {
                const State state = _state.load(std::memory_order_acquire);
                if (state == State::Succeeded || state == State::Failed)
                {
                    _state.store(State::Idle, std::memory_order_release);
                    return state == State::Succeeded;
                }

                if (state == requestState && std::chrono::steady_clock::now() >= deadline)
                {
                    expected = requestState;
                    if (_state.compare_exchange_strong(expected, State::Idle, std::memory_order_acq_rel))
                    {
                        return false;
                    }
                }
                std::this_thread::yield();
            }
        }

        bool tryStart(bool& shutDownAfterCommit)
        {
            State expected = State::Requested;
            if (_state.compare_exchange_strong(expected, State::Running, std::memory_order_acq_rel))
            {
                shutDownAfterCommit = false;
                return true;
            }

            expected = State::ShutdownRequested;
            if (_state.compare_exchange_strong(expected, State::Running, std::memory_order_acq_rel))
            {
                shutDownAfterCommit = true;
                return true;
            }
            return false;
        }

        bool tryStart()
        {
            bool shutDownAfterCommit;
            return tryStart(shutDownAfterCommit);
        }

        bool finish(bool succeeded)
        {
            State expected = State::Running;
            return _state.compare_exchange_strong(expected, succeeded ? State::Succeeded : State::Failed, std::memory_order_acq_rel);
        }

        State state() const
        {
            return _state.load(std::memory_order_acquire);
        }

        void resetForChild()
        {
            _state.store(State::Idle, std::memory_order_release);
        }

    private:
        std::atomic<State> _state{ State::Idle };
    };

    inline BspRetireHandoff gBspRetireHandoff;

    // Accepted node-side RPC sockets retain the listener path after fork.
    inline unsigned int closeInheritedRpcUnixSocketsForPromote(int listenerFd, const char* rpcPath)
    {
        unsigned int closedCount = 0;
        if (listenerFd >= 0 && close(listenerFd) == 0)
        {
            closedCount++;
        }

        DIR* directory = opendir("/proc/self/fd");
        if (!directory)
        {
            const int error = errno;
            fprintf(stderr, "[RPC] promote opendir failed: errno=%d (%s)\n", error, strerror(error));
            fflush(stderr);
            return closedCount;
        }

        const int directoryFd = dirfd(directory);
        for (;;)
        {
            errno = 0;
            dirent* entry = readdir(directory);
            if (!entry)
            {
                const int error = errno;
                closedir(directory);
                if (error)
                {
                    fprintf(stderr, "[RPC] promote readdir failed: errno=%d (%s)\n", error, strerror(error));
                }
                fprintf(stderr, "[RPC] promote closed %u inherited RPC AF_UNIX fd(s)\n", closedCount);
                fflush(stderr);
                return closedCount;
            }

            const int fd = atoi(entry->d_name);
            if (fd <= STDERR_FILENO || fd == directoryFd || fd == listenerFd)
            {
                continue;
            }

            int domain = 0;
            socklen_t domainSize = sizeof(domain);
            if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &domainSize) != 0 || domain != AF_UNIX)
            {
                continue;
            }

            sockaddr_un address{};
            socklen_t addressSize = sizeof(address);
            if (getsockname(fd, (sockaddr*)&address, &addressSize) != 0 || strncmp(address.sun_path, rpcPath, sizeof(address.sun_path)) != 0)
            {
                continue;
            }

            if (close(fd) == 0)
            {
                closedCount++;
            }
        }
    }

    inline ssize_t readRetryOnInterrupt(int pipeFd, void* buffer, size_t size)
    {
        ssize_t readSize;
        do
        {
            readSize = read(pipeFd, buffer, size);
        }
        while (readSize < 0 && errno == EINTR);
        return readSize;
    }

    inline bool waitForPipeEof(int pipeFd)
    {
        char discarded[64];
        ssize_t readSize;
        while ((readSize = readRetryOnInterrupt(pipeFd, discarded, sizeof(discarded))) > 0)
        {
        }
        return readSize == 0;
    }

    inline bool writeRetireCommand(int pipeFd)
    {
        ssize_t writeSize;
        do
        {
            writeSize = write(pipeFd, &retireTag, 1);
        }
        while (writeSize < 0 && errno == EINTR);
        return writeSize == 1;
    }

    // crashTargetTick() is evaluated only if the parent failed, after its pipe closed, so it can read
    // how far the parent got.
    template <typename CrashTarget>
    inline ChildCommand readChildCommandWithCrashTarget(int pipeFd, CrashTarget crashTargetTick)
    {
        char tag = 0;
        unsigned int targetTick = 0;
        const ssize_t readSize = readRetryOnInterrupt(pipeFd, &tag, 1);

        if (readSize == 1 && tag == retireTag)
            return { ChildAction::Retire, 0 };

        if (readSize == 1 && tag == promoteTag)
        {
            if (readRetryOnInterrupt(pipeFd, &targetTick, sizeof(targetTick)) != (ssize_t)sizeof(targetTick))
                targetTick = 0;
        }

        // EOF, a short frame, or an unknown tag means the parent failed unexpectedly.
        if (!waitForPipeEof(pipeFd))
            _exit(72);
        if (targetTick == 0)
            targetTick = crashTargetTick();
        return { ChildAction::Promote, targetTick };
    }

    inline ChildCommand readChildCommand(int pipeFd, unsigned int crashTargetTick)
    {
        return readChildCommandWithCrashTarget(pipeFd, [crashTargetTick]() { return crashTargetTick; });
    }
}

#endif
