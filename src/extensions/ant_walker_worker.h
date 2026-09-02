#pragma once

// The ant score walk, in a process re-exec'd from this binary (--ant-walk-worker): on a node thread
// it would hold the score-engine lock across nearly every fork point. Stateless, holds no node state.

#if defined(ANT_WALKER) && !defined(_WIN32)

#include "ant_walker_proto.h"

#include <deque>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace AntWalkerWorker
{
using Ann = score_engine::ScoreBpp9000T::ANN;

constexpr int NO_TRAFFIC_TIMEOUT_MS = 60'000;
constexpr int POLL_SLICE_MS = 1'000;

struct Options
{
    std::string socketPath;
    unsigned int threadCount = 4;
};

// ── socket helpers ──────────────────────────────────────────────────────────────────────────────

bool readFully(int fd, void* buffer, size_t size)
{
    unsigned char* out = (unsigned char*)buffer;
    size_t done = 0;
    while (done < size)
    {
        const ssize_t got = read(fd, out + done, size - done);
        if (got > 0)
        {
            done += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR)
        {
            continue;
        }
        return false;
    }
    return true;
}

bool writeFully(int fd, const void* buffer, size_t size)
{
    const unsigned char* in = (const unsigned char*)buffer;
    size_t done = 0;
    while (done < size)
    {
        const ssize_t put = write(fd, in + done, size - done);
        if (put > 0)
        {
            done += (size_t)put;
            continue;
        }
        if (put < 0 && errno == EINTR)
        {
            continue;
        }
        return false;
    }
    return true;
}

// Bounded, so a peer that vanished without an EOF cannot park this process forever.
bool waitReadable(int fd, int timeoutMs, const std::atomic<bool>& stop)
{
    int waitedMs = 0;
    while (waitedMs < timeoutMs)
    {
        if (stop.load(std::memory_order_acquire))
        {
            return false;
        }
        struct pollfd entry;
        entry.fd = fd;
        entry.events = POLLIN;
        entry.revents = 0;
        const int slice = (timeoutMs - waitedMs < POLL_SLICE_MS) ? (timeoutMs - waitedMs) : POLL_SLICE_MS;
        const int ready = poll(&entry, 1, slice);
        if (ready > 0)
        {
            return true;
        }
        if (ready < 0 && errno != EINTR)
        {
            return false;
        }
        waitedMs += slice;
    }
    return false;
}

// ── engine ──────────────────────────────────────────────────────────────────────────────────────

unsigned char* gPool = nullptr;
unsigned char gPoolSeed[32] = {};
bool gPoolSeeded = false;

bool ensurePool(const unsigned char* miningSeed)
{
    if (gPool == nullptr)
    {
        gPool = (unsigned char*)malloc((size_t)score_engine::POOL_VEC_PADDING_SIZE);
        if (gPool == nullptr)
        {
            return false;
        }
    }
    if (gPoolSeeded && memcmp(gPoolSeed, miningSeed, 32) == 0)
    {
        return true;
    }
    unsigned char state[score_engine::STATE_SIZE];
    score_engine::generateRandom2Pool(miningSeed, state, gPool);
    memcpy(gPoolSeed, miningSeed, 32);
    gPoolSeeded = true;
    return true;
}

bool loadEmbeddedTask(score_engine::ScoreBpp9000T& engine)
{
    const unsigned int inputTrits = (unsigned int)BPP9000_NUMBER_OF_INPUT_NEURONS;
    const unsigned int outputTrits = (unsigned int)BPP9000_NUMBER_OF_OUTPUT_NEURONS;
    const unsigned int population = (unsigned int)BPP9000_POPULATION_THRESHOLD;
    const unsigned int neighbors = (unsigned int)BPP9000_NUMBER_OF_NEIGHBORS;

    const unsigned long long topologyBytes = score_task_file::topologyBytes(inputTrits, outputTrits, population, neighbors);
    const unsigned char* topologyBlock = BPP9000_TASK_BYTES + sizeof(score_task_file::TaskFileHeader);
    const unsigned char* dataBlock = topologyBlock + topologyBytes;

    engine.initMemory();
    return engine.loadTaskFromMemory(topologyBlock, dataBlock);
}

// ── worker pool ─────────────────────────────────────────────────────────────────────────────────

struct WorkerPool
{
    std::mutex queueMutex;   // SMARTMUTEX-EXEMPT: separate process, holds no node state and never forks
    std::condition_variable queueSignal;
    std::deque<AntWalkProto::JobPayload> queue;

    std::mutex writeMutex;   // SMARTMUTEX-EXEMPT: separate process, holds no node state and never forks
    std::atomic<bool> stopping{ false };
    std::atomic<bool> connected{ false };
    std::atomic<unsigned int> epochId{ 0 };
    std::atomic<int> connectionFd{ -1 };
    std::atomic<unsigned int> busy{ 0 };

    std::vector<std::thread> threads;
};

WorkerPool gPoolOfWorkers;

void runWorker(unsigned int workerIndex)
{
    score_engine::ScoreBpp9000T* engine = (score_engine::ScoreBpp9000T*)aligned_alloc(64, (sizeof(score_engine::ScoreBpp9000T) + 63) / 64 * 64);
    if (engine == nullptr || !loadEmbeddedTask(*engine))
    {
        fprintf(stderr, "[ant-walker] worker %u could not load the embedded task\n", workerIndex);
        fflush(stderr);
        return;
    }
    Ann rootAnn;
    Ann childAnn;

    for (;;)
    {
        AntWalkProto::JobPayload job;
        {
            std::unique_lock<std::mutex> lock(gPoolOfWorkers.queueMutex);
            gPoolOfWorkers.queueSignal.wait(lock, [] {
                return gPoolOfWorkers.stopping.load(std::memory_order_acquire)
                    || !gPoolOfWorkers.queue.empty();
            });
            if (gPoolOfWorkers.stopping.load(std::memory_order_acquire) && gPoolOfWorkers.queue.empty())
            {
                break;
            }
            job = gPoolOfWorkers.queue.front();
            gPoolOfWorkers.queue.pop_front();
            gPoolOfWorkers.busy.fetch_add(1, std::memory_order_acq_rel);
        }

        struct BusyGuard
        {
            ~BusyGuard() { gPoolOfWorkers.busy.fetch_sub(1, std::memory_order_acq_rel); }
        } busyGuard;

        AntWalkProto::ResultPayload result;
        memset(&result, 0, sizeof(result));
        result.jobId = job.jobId;
        result.epochId = job.epochId;

        if (job.epochId != gPoolOfWorkers.epochId.load(std::memory_order_acquire))
        {
            result.status = AntWalkProto::ResultStaleEpoch;
        }
        else
        {
            const Ann* parentAnn;
            if (job.isRoot)
            {
                engine->deriveRootANN(job.pubkey, gPool, rootAnn);
                parentAnn = &rootAnn;
            }
            else
            {
                parentAnn = (const Ann*)job.parentAnn;
            }

            const unsigned int score = engine->computeScoreFromParent(*parentAnn, job.pubkey, job.nonce, job.anchorDigest, gPool);
            if (score == score_engine::INVALID_SCORE_VALUE)
            {
                result.status = AntWalkProto::ResultUnscorable;
            }
            else
            {
                engine->getBestANN(childAnn);
                result.status = AntWalkProto::ResultOk;
                result.score = score;
                memcpy(result.childAnn, &childAnn, sizeof(childAnn));
            }
        }

        // A walk cannot be aborted, so one that outlived its connection finishes and is dropped.
        if (!gPoolOfWorkers.connected.load(std::memory_order_acquire))
        {
            continue;
        }
        std::lock_guard<std::mutex> writeGuard(gPoolOfWorkers.writeMutex);
        const int fd = gPoolOfWorkers.connectionFd.load(std::memory_order_acquire);
        if (fd < 0)
        {
            continue;
        }
        AntWalkProto::FrameHeader header;
        header.magic = AntWalkProto::MAGIC;
        header.type = AntWalkProto::MsgResult;
        header.payloadSize = (unsigned int)sizeof(result);
        header.reserved = 0;
        if (!writeFully(fd, &header, sizeof(header)) || !writeFully(fd, &result, sizeof(result)))
        {
            gPoolOfWorkers.connected.store(false, std::memory_order_release);
        }
    }

    free(engine);
}

// ── connection ──────────────────────────────────────────────────────────────────────────────────

bool sendReady(int fd, unsigned int status, unsigned int threadCount, unsigned int epochId)
{
    AntWalkProto::ReadyPayload ready;
    memset(&ready, 0, sizeof(ready));
    ready.version = AntWalkProto::VERSION;
    ready.status = status;
    ready.threadCount = threadCount;
    ready.annBytes = AntWalkProto::ANN_BYTES;
    ready.epochId = epochId;
    ready.walkerPid = (unsigned int)getpid();

    AntWalkProto::FrameHeader header;
    header.magic = AntWalkProto::MAGIC;
    header.type = AntWalkProto::MsgReady;
    header.payloadSize = (unsigned int)sizeof(ready);
    header.reserved = 0;

    std::lock_guard<std::mutex> writeGuard(gPoolOfWorkers.writeMutex);
    return writeFully(fd, &header, sizeof(header)) && writeFully(fd, &ready, sizeof(ready));
}

unsigned int validateHello(const AntWalkProto::HelloPayload& hello)
{
    if (hello.version != AntWalkProto::VERSION || hello.annBytes != AntWalkProto::ANN_BYTES)
    {
        return AntWalkProto::ReadyVersionMismatch;
    }
    if (memcmp(hello.topologyHash, BPP9000_TOPOLOGY_HASH, 32) != 0 || memcmp(hello.dataHash, BPP9000_DATA_HASH, 32) != 0)
    {
        return AntWalkProto::ReadyTaskMismatch;
    }
    if (hello.configHash != AntWalkProto::CONFIG_HASH)
    {
        fprintf(stderr, "[ant-walker] scorer config %08x does not match the node's %08x\n", AntWalkProto::CONFIG_HASH, hello.configHash);
        fflush(stderr);
        return AntWalkProto::ReadyConfigMismatch;
    }
    return AntWalkProto::ReadyOk;
}

bool hasWorkOutstanding()
{
    if (gPoolOfWorkers.busy.load(std::memory_order_acquire) > 0)
    {
        return true;
    }
    std::lock_guard<std::mutex> queueGuard(gPoolOfWorkers.queueMutex);
    return !gPoolOfWorkers.queue.empty();
}

void serveConnection(int fd, unsigned int threadCount)
{
    gPoolOfWorkers.connectionFd.store(fd, std::memory_order_release);
    gPoolOfWorkers.connected.store(true, std::memory_order_release);

    unsigned char payload[AntWalkProto::MAX_PAYLOAD_BYTES];
    bool handshaken = false;

    while (gPoolOfWorkers.connected.load(std::memory_order_acquire))
    {
        if (!waitReadable(fd, NO_TRAFFIC_TIMEOUT_MS, gPoolOfWorkers.stopping))
        {
            // A walk outlasts this timeout, so silence only means a dead peer when nothing is queued.
            if (hasWorkOutstanding())
            {
                continue;
            }
            fprintf(stderr, "[ant-walker] no traffic for %d ms and nothing in flight, dropping the connection\n", NO_TRAFFIC_TIMEOUT_MS);
            fflush(stderr);
            break;
        }

        AntWalkProto::FrameHeader header;
        if (!readFully(fd, &header, sizeof(header)))
        {
            break;
        }
        if (header.magic != AntWalkProto::MAGIC || header.payloadSize > sizeof(payload))
        {
            fprintf(stderr, "[ant-walker] bad frame (magic %08x size %u), dropping the connection\n", header.magic, header.payloadSize);
            fflush(stderr);
            break;
        }
        if (header.payloadSize && !readFully(fd, payload, header.payloadSize))
        {
            break;
        }

        if (header.type == AntWalkProto::MsgHello)
        {
            if (header.payloadSize != sizeof(AntWalkProto::HelloPayload))
            {
                break;
            }
            AntWalkProto::HelloPayload hello;
            memcpy(&hello, payload, sizeof(hello));

            unsigned int status = validateHello(hello);
            if (status == AntWalkProto::ReadyOk && !ensurePool(hello.miningSeed))
            {
                status = AntWalkProto::ReadySeedFailed;
            }
            if (status == AntWalkProto::ReadyOk)
            {
                gPoolOfWorkers.epochId.store(hello.epochId, std::memory_order_release);
                handshaken = true;
            }
            if (!sendReady(fd, status, threadCount, hello.epochId) || status != AntWalkProto::ReadyOk)
            {
                fprintf(stderr, "[ant-walker] handshake refused (status %u)\n", status);
                fflush(stderr);
                break;
            }
            fprintf(stderr, "[ant-walker] ready, %u threads, epoch %u\n", threadCount, hello.epochId);
            fflush(stderr);
        }
        else if (header.type == AntWalkProto::MsgJob)
        {
            if (!handshaken || header.payloadSize != sizeof(AntWalkProto::JobPayload))
            {
                break;
            }
            std::lock_guard<std::mutex> queueGuard(gPoolOfWorkers.queueMutex);
            gPoolOfWorkers.queue.emplace_back();
            memcpy(&gPoolOfWorkers.queue.back(), payload, sizeof(AntWalkProto::JobPayload));
            gPoolOfWorkers.queueSignal.notify_one();
        }
        else if (header.type == AntWalkProto::MsgPing)
        {
            AntWalkProto::FrameHeader pong;
            pong.magic = AntWalkProto::MAGIC;
            pong.type = AntWalkProto::MsgPong;
            pong.payloadSize = 0;
            pong.reserved = 0;
            std::lock_guard<std::mutex> writeGuard(gPoolOfWorkers.writeMutex);
            if (!writeFully(fd, &pong, sizeof(pong)))
            {
                break;
            }
        }
    }

    gPoolOfWorkers.connected.store(false, std::memory_order_release);
    {
        // Queued jobs belong to a connection that is gone; only the walks already running are paid for.
        std::lock_guard<std::mutex> queueGuard(gPoolOfWorkers.queueMutex);
        gPoolOfWorkers.queue.clear();
    }
    {
        // Held until no worker can still be inside writeFully on this descriptor.
        std::lock_guard<std::mutex> writeGuard(gPoolOfWorkers.writeMutex);
        gPoolOfWorkers.connectionFd.store(-1, std::memory_order_release);
    }
    close(fd);
}

// ── listener ────────────────────────────────────────────────────────────────────────────────────

// Competing with a live server is how an orphaned sidecar ends up answering for a running one.
bool socketPathIsServed(const char* path)
{
    const int probeFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probeFd < 0)
    {
        return false;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    const bool served = connect(probeFd, (struct sockaddr*)&address, sizeof(address)) == 0;
    close(probeFd);
    return served;
}

int openListener(const char* path)
{
    if (socketPathIsServed(path))
    {
        fprintf(stderr, "[ant-walker] %s is already served, refusing to compete\n", path);
        fflush(stderr);
        return -1;
    }
    unlink(path);

    const int listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd < 0)
    {
        perror("[ant-walker] socket");
        return -1;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    if (bind(listenFd, (struct sockaddr*)&address, sizeof(address)) != 0 || listen(listenFd, 4) != 0)
    {
        perror("[ant-walker] bind/listen");
        close(listenFd);
        return -1;
    }
    return listenFd;
}

bool parseOptions(int argc, const char* argv[], Options& options)
{
    for (int i = 1; i + 1 < argc; i++)
    {
        const std::string argument = argv[i];
        if (argument == "--socket")
        {
            options.socketPath = argv[i + 1];
        }
        else if (argument == "--threads")
        {
            options.threadCount = (unsigned int)strtoul(argv[i + 1], nullptr, 10);
        }
    }
    return !options.socketPath.empty() && options.threadCount > 0;
}

// Checked before any node setup: the node half of main() must not run in the walker.
inline bool requested(int argc, const char* argv[])
{
    for (int i = 1; i < argc; i++)
    {
        if (std::string(argv[i]) == "--ant-walk-worker")
        {
            return true;
        }
    }
    return false;
}

inline int run(int argc, const char* argv[])
{
    Options options;
    if (!parseOptions(argc, argv, options))
    {
        fprintf(stderr, "[ant-walker] --ant-walk-worker needs --socket <path> --threads <n>\n");
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
#if defined(__linux__)
    // The node is the only reason this process exists; outliving it would just squat the socket.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1)
    {
        return 0;
    }
#endif

    const int listenFd = openListener(options.socketPath.c_str());
    if (listenFd < 0)
    {
        return 3;
    }

    for (unsigned int i = 0; i < options.threadCount; i++)
    {
        gPoolOfWorkers.threads.emplace_back(runWorker, i);
    }
    fprintf(stderr, "[ant-walker] listening on %s, %u threads, pid %d\n", options.socketPath.c_str(), options.threadCount, (int)getpid());
    fflush(stderr);

    while (!gPoolOfWorkers.stopping.load(std::memory_order_acquire))
    {
        struct pollfd entry;
        entry.fd = listenFd;
        entry.events = POLLIN;
        entry.revents = 0;
        const int ready = poll(&entry, 1, POLL_SLICE_MS);
#if defined(__linux__)
        if (getppid() == 1)
        {
            break;
        }
#endif
        if (ready <= 0)
        {
            continue;
        }
        const int connectionFd = accept(listenFd, nullptr, nullptr);
        if (connectionFd < 0)
        {
            continue;
        }
        serveConnection(connectionFd, options.threadCount);
    }

    gPoolOfWorkers.stopping.store(true, std::memory_order_release);
    gPoolOfWorkers.queueSignal.notify_all();
    for (std::thread& worker : gPoolOfWorkers.threads)
    {
        worker.join();
    }
    close(listenFd);
    unlink(options.socketPath.c_str());
    return 0;
}
}

#else

namespace AntWalkerWorker
{
inline bool requested(int, const char*[]) { return false; }
inline int run(int, const char*[]) { return 0; }
}

#endif // ANT_WALKER && !_WIN32
