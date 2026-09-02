#pragma once

// Node side of the ant walker: picks records committed without a network, sends the walk to a
// separate process, publishes the result after re-verifying it against the record.

#if defined(ANT_WALKER) && !defined(_WIN32)

#include "ant_walker_proto.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace AntWalker
{
enum class LinkState
{
    Disabled,
    Disconnected,
    Connecting,
    Handshaking,
    Ready,
};

// A wrong walker fails every job, and marking each would poison good records permanently, so a
// streak is read as a broken walker instead.
static constexpr unsigned int DISAGREEMENT_STREAK_LIMIT = 3;
static constexpr unsigned int DEADLINE_STREAK_LIMIT = 3;
// "Gone", not "slow": a walk runs for minutes and the walker answers PING throughout.
static constexpr long long MIN_JOB_DEADLINE_MS = 600'000;
static constexpr long long PING_INTERVAL_MS = 10'000;
static constexpr long long BACKOFF_MIN_MS = 100;
static constexpr long long BACKOFF_MAX_MS = 30'000;
static constexpr int POLL_SLICE_MS = 100;
static constexpr long long HEARTBEAT_INTERVAL_MS = 60'000;
// Covers a legitimate 512 MB pool derive without hiding a wedged walker.
static constexpr long long HANDSHAKE_DEADLINE_MS = 120'000;
// A record the tick is blocked on is handed back, but only once the job is old enough to be stuck:
// yanking a healthy walk throws its progress away and the node pays the whole walk again.
static constexpr long long PREEMPT_MIN_AGE_MS = 180'000;
static constexpr long long PREEMPT_ACK_WAIT_MS = 500;
static constexpr unsigned int PREEMPT_NONE = 0xFFFFFFFFu;

struct InFlight
{
    unsigned long long jobId;
    unsigned int recordIndex;
    long long sentAtMs;
    // Kept for the replay key: re-resolving the anchor later could pick a different digest.
    m256i anchorDigest;
};

struct State
{
    std::string socketPath;
    unsigned int threadCount = 0;
    bool debug = false;

    std::atomic<int> link{ (int)LinkState::Disabled };
    std::atomic<int> fd{ -1 };
    std::atomic<bool> stopping{ false };
    std::atomic<bool> quiesceRequested{ false };
    std::atomic<bool> quiesceAcknowledged{ true };
    std::atomic<unsigned int> seedGeneration{ 0 };
    std::atomic<int> walkerPid{ -1 };
    std::atomic<int> suspectWalkerPid{ -1 };
    std::atomic<unsigned int> preemptRequest{ PREEMPT_NONE };
    std::atomic<unsigned int> handedBack{ PREEMPT_NONE };

    std::atomic<unsigned long long> jobsSent{ 0 };
    std::atomic<unsigned long long> materialised{ 0 };
    std::atomic<unsigned long long> memoHits{ 0 };
    std::atomic<unsigned long long> preempted{ 0 };
    std::atomic<unsigned long long> disagreements{ 0 };
    std::atomic<unsigned long long> deadlineExpiries{ 0 };
    std::atomic<unsigned long long> staleDropped{ 0 };
    std::atomic<unsigned long long> reconnects{ 0 };
    std::atomic<unsigned long long> failedCount{ 0 };
    std::atomic<unsigned long long> walkMsEma{ 0 };
    std::atomic<unsigned long long> lastResultAtMs{ 0 };
    std::atomic<unsigned int> inFlightCount{ 0 };
    std::atomic<unsigned long long> backlog{ 0 };

    std::vector<unsigned char> failedBits;
    std::vector<unsigned int> rolledBackCandidates;
    std::vector<InFlight> inFlight;
    unsigned long long nextJobId = 1;
    unsigned int cursor = 0;
    unsigned int disagreementStreak = 0;
    unsigned int deadlineStreak = 0;
    unsigned int helloGeneration = 0;
    long long handshakeStartedAtMs = 0;

    std::thread* dispatcher = nullptr;
};

inline State gState;

inline const char* linkName(LinkState state)
{
    switch (state)
    {
    case LinkState::Disabled: return "disabled";
    case LinkState::Disconnected: return "disconnected";
    case LinkState::Connecting: return "connecting";
    case LinkState::Handshaking: return "handshaking";
    case LinkState::Ready: return "ready";
    }
    return "?";
}

inline long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void logLine(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fprintf(stderr, "[ant-walk] ");
    vfprintf(stderr, format, arguments);
    fprintf(stderr, "\n");
    va_end(arguments);
    fflush(stderr);
}

inline bool isEnabled()
{
    return gState.threadCount > 0 && !gState.socketPath.empty();
}

// ── failed-record bitmap ────────────────────────────────────────────────────────────────────────
// Scheduling only: the on-demand rebuild path ignores this and still walks.

inline void ensureFailedBits()
{
    const size_t needed = (size_t)ANT_MAX_NODES_PER_EPOCH / 8;
    if (gState.failedBits.size() != needed)
    {
        gState.failedBits.assign(needed, 0);
    }
}

inline bool isFailed(unsigned int index)
{
    ensureFailedBits();
    return (gState.failedBits[index >> 3] >> (index & 7)) & 1;
}

inline void markFailed(unsigned int index)
{
    ensureFailedBits();
    if (!isFailed(index))
    {
        gState.failedCount.fetch_add(1, std::memory_order_relaxed);
    }
    gState.failedBits[index >> 3] |= (unsigned char)(1u << (index & 7));
}

inline void clearFailed(unsigned int index)
{
    ensureFailedBits();
    if (isFailed(index))
    {
        gState.failedCount.fetch_sub(1, std::memory_order_relaxed);
    }
    gState.failedBits[index >> 3] &= (unsigned char)~(1u << (index & 7));
}

// ── framing ─────────────────────────────────────────────────────────────────────────────────────

inline bool writeFully(int fd, const void* buffer, size_t size)
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

inline bool readFully(int fd, void* buffer, size_t size)
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

inline bool sendFrame(int fd, unsigned int type, const void* payload, unsigned int payloadSize)
{
    AntWalkProto::FrameHeader header;
    header.magic = AntWalkProto::MAGIC;
    header.type = type;
    header.payloadSize = payloadSize;
    header.reserved = 0;
    if (!writeFully(fd, &header, sizeof(header)))
    {
        return false;
    }
    return payloadSize == 0 || writeFully(fd, payload, payloadSize);
}

// ── link ────────────────────────────────────────────────────────────────────────────────────────

// Releases every in-flight claim: the on-demand path would otherwise wait on a dead connection.
inline void dropLink(const char* reason)
{
    const int fd = gState.fd.exchange(-1);
    if (fd >= 0)
    {
        close(fd);
    }
    unsigned int released = 0;
    for (const InFlight& job : gState.inFlight)
    {
        gAntColony.releaseAnnClaim(job.recordIndex);
        released++;
    }
    gState.inFlight.clear();
    gState.inFlightCount.store(0, std::memory_order_release);
    if (gState.link.load(std::memory_order_acquire) != (int)LinkState::Disconnected)
    {
        logLine("sidecar lost (%s), %u claims released", reason, released);
    }
    gState.link.store((int)LinkState::Disconnected, std::memory_order_release);
}

inline bool sendHello(int fd)
{
    AntWalkProto::HelloPayload hello;
    memset(&hello, 0, sizeof(hello));
    hello.version = AntWalkProto::VERSION;
    hello.epochId = gState.seedGeneration.load(std::memory_order_acquire);
    hello.annBytes = AntWalkProto::ANN_BYTES;
    hello.configHash = AntWalkProto::CONFIG_HASH;
    gState.helloGeneration = hello.epochId;
    copyMem(hello.miningSeed, score->currentRandomSeed.m256i_u8, 32);
    copyMem(hello.topologyHash, BPP9000_TOPOLOGY_HASH, 32);
    copyMem(hello.dataHash, BPP9000_DATA_HASH, 32);
    return sendFrame(fd, AntWalkProto::MsgHello, &hello, (unsigned int)sizeof(hello));
}

inline bool tryConnect()
{
    gState.link.store((int)LinkState::Connecting, std::memory_order_release);
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return false;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", gState.socketPath.c_str());
    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0)
    {
        close(fd);
        gState.link.store((int)LinkState::Disconnected, std::memory_order_release);
        return false;
    }

    gState.fd.store(fd, std::memory_order_release);
    gState.link.store((int)LinkState::Handshaking, std::memory_order_release);
    gState.handshakeStartedAtMs = nowMs();
    if (!sendHello(fd))
    {
        dropLink("hello write failed");
        return false;
    }
    return true;
}

// ── selection ───────────────────────────────────────────────────────────────────────────────────

// Commit order is topological, so this walks each lineage bottom-up without repeating a level.
inline bool selectNextRecord(unsigned int& outIndex)
{
    const unsigned int recordCount = gAntColony.solutionCount();
    if (recordCount == 0)
    {
        return false;
    }

    for (unsigned int scanned = 0; scanned < recordCount; scanned++)
    {
        const unsigned int index = (gState.cursor + scanned) % recordCount;
        if (index == gState.handedBack.load(std::memory_order_acquire))
        {
            // Held until the tick has actually taken it, so the walker cannot re-take what it gave up.
            if (gAntColony.isAnnClaimHeld(index) || gAntColony.isAnnMaterialised(index))
            {
                gState.handedBack.store(PREEMPT_NONE, std::memory_order_release);
            }
            continue;
        }
        if (isFailed(index) || !AntColonyMaintenance::isRebuildableNow(gAntColony, index))
        {
            continue;
        }
        gState.cursor = (index + 1) % recordCount;
        outIndex = index;
        return true;
    }
    return false;
}

// A full record scan (2^23 on mainnet), so it runs on the heartbeat rather than per dispatch.
inline void refreshBacklog()
{
    const unsigned int recordCount = gAntColony.solutionCount();
    unsigned int backlogCount = 0;
    for (unsigned int index = 0; index < recordCount; index++)
    {
        if (!gAntColony.isAnnMaterialised(index))
        {
            backlogCount++;
        }
    }
    gState.backlog.store(backlogCount, std::memory_order_release);
}

inline bool dispatchOne()
{
    unsigned int index;
    if (!selectNextRecord(index))
    {
        return false;
    }
    if (gAntColony.tryClaimAnn(index) != AntColonyBpp9000T::AnnClaimOwned)
    {
        return false;
    }
    const AntSolutionRecord* record = gAntColony.recordAt(index);
    if (record == nullptr)
    {
        gAntColony.releaseAnnClaim(index);
        return false;
    }

    AntWalkProto::JobPayload job;
    memset(&job, 0, sizeof(job));
    job.jobId = gState.nextJobId++;
    job.epochId = gState.seedGeneration.load(std::memory_order_acquire);
    job.isRoot = record->parentRef.isRoot() ? 1u : 0u;
    copyMem(job.pubkey, record->pubkey.m256i_u8, 32);
    copyMem(job.nonce, record->nonce.m256i_u8, 32);

    m256i anchorDigest;
    if (!getAntAnchorDigestForRebuild(record->anchorTick, anchorDigest))
    {
        gAntColony.releaseAnnClaim(index);
        markFailed(index);
        return false;
    }
    copyMem(job.anchorDigest, anchorDigest.m256i_u8, 32);

    // Every other scoring path memoises its walk, so a score already held here costs no job at all.
    const AntColonyBpp9000T::ReplayKey replayKey = makeAntReplayKey(record->pubkey, record->nonce, record->parentRef, anchorDigest);
    AntColonyBpp9000T::Ann memoAnn;
    unsigned int memoScore;
    if (gAntColony.tryGetReplayScore(replayKey, memoScore, memoAnn))
    {
        if (memoScore != record->score)
        {
            // The node's own cache disagreeing is the record's fault, not the walker's, so no streak.
            gAntColony.releaseAnnClaim(index);
            markFailed(index);
            logLine("record %u memo %u != accepted %u, marked failed", index, memoScore, record->score);
            return false;
        }
        unsigned int memoHash;
        KangarooTwelve(&memoAnn, sizeof(memoAnn), &memoHash, sizeof(memoHash));
        gAntColony.publishAnn(index, memoAnn, memoHash);
        gState.memoHits.fetch_add(1, std::memory_order_relaxed);
        gState.materialised.fetch_add(1, std::memory_order_relaxed);
        if (gState.debug)
        {
            logLine("record %u from the replay cache, no job", index);
        }
        return true;
    }

    if (!job.isRoot)
    {
        const long long parentIndex = gAntColony.findIndexBySolutionRef(record->parentRef);
        const AntSolutionRecord* parentRecord = (parentIndex == ANT_INVALID_INDEX) ? nullptr : gAntColony.recordAt(parentIndex);
        AntColonyBpp9000T::Ann parentAnn;
        if (parentRecord == nullptr || !gAntColony.annOfNonRoot(*parentRecord, parentAnn))
        {
            gAntColony.releaseAnnClaim(index);
            return false;
        }
        copyMem(job.parentAnn, &parentAnn, sizeof(parentAnn));
    }

    const int fd = gState.fd.load(std::memory_order_acquire);
    if (fd < 0 || !sendFrame(fd, AntWalkProto::MsgJob, &job, (unsigned int)sizeof(job)))
    {
        gAntColony.releaseAnnClaim(index);
        dropLink("job write failed");
        return false;
    }

    gState.inFlight.push_back(InFlight{ job.jobId, index, nowMs(), anchorDigest });
    gState.inFlightCount.store((unsigned int)gState.inFlight.size(), std::memory_order_release);
    gState.jobsSent.fetch_add(1, std::memory_order_relaxed);
    if (gState.debug)
    {
        logLine("job %llu record %u depth %u sent", (unsigned long long)job.jobId, index, (unsigned)record->depth);
    }
    return true;
}

// ── results ─────────────────────────────────────────────────────────────────────────────────────

inline bool takeInFlight(unsigned long long jobId, InFlight& out)
{
    for (size_t i = 0; i < gState.inFlight.size(); i++)
    {
        if (gState.inFlight[i].jobId == jobId)
        {
            out = gState.inFlight[i];
            gState.inFlight.erase(gState.inFlight.begin() + (long)i);
            gState.inFlightCount.store((unsigned int)gState.inFlight.size(), std::memory_order_release);
            return true;
        }
    }
    return false;
}

inline void noteDisagreement(unsigned int index)
{
    markFailed(index);
    gState.rolledBackCandidates.push_back(index);
    gState.disagreements.fetch_add(1, std::memory_order_relaxed);
    gState.disagreementStreak++;
    if (gState.disagreementStreak < DISAGREEMENT_STREAK_LIMIT)
    {
        return;
    }
    for (unsigned int candidate : gState.rolledBackCandidates)
    {
        clearFailed(candidate);
    }
    logLine("%u consecutive disagreements - walker suspect, %u marks rolled back, disconnecting",
        gState.disagreementStreak, (unsigned int)gState.rolledBackCandidates.size());
    gState.rolledBackCandidates.clear();
    gState.disagreementStreak = 0;
    gState.suspectWalkerPid.store(gState.walkerPid.load(std::memory_order_acquire), std::memory_order_release);
    dropLink("walker disagrees on every job");
}

inline void noteSuccess()
{
    gState.disagreementStreak = 0;
    gState.deadlineStreak = 0;
    gState.rolledBackCandidates.clear();
}

inline void applyResult(const AntWalkProto::ResultPayload& result)
{
    InFlight job;
    if (!takeInFlight(result.jobId, job))
    {
        return;
    }

    const long long walkMs = nowMs() - job.sentAtMs;
    const unsigned long long previousEma = gState.walkMsEma.load(std::memory_order_acquire);
    gState.walkMsEma.store(previousEma ? (previousEma * 3 + (unsigned long long)walkMs) / 4 : (unsigned long long)walkMs, std::memory_order_release);
    gState.lastResultAtMs.store((unsigned long long)nowMs(), std::memory_order_release);

    if (result.epochId != gState.seedGeneration.load(std::memory_order_acquire) || result.status == AntWalkProto::ResultStaleEpoch)
    {
        gAntColony.releaseAnnClaim(job.recordIndex);
        gState.staleDropped.fetch_add(1, std::memory_order_relaxed);
        logLine("job %llu dropped, epoch %u is not %u", (unsigned long long)result.jobId,
            result.epochId, gState.seedGeneration.load(std::memory_order_acquire));
        return;
    }

    const AntSolutionRecord* record = gAntColony.recordAt(job.recordIndex);
    if (record == nullptr)
    {
        gAntColony.releaseAnnClaim(job.recordIndex);
        return;
    }

    if (result.status != AntWalkProto::ResultOk || result.score != record->score)
    {
        gAntColony.releaseAnnClaim(job.recordIndex);
        logLine("record %u walked %u != accepted %u, marked failed", job.recordIndex, result.score, record->score);
        noteDisagreement(job.recordIndex);
        return;
    }

    AntColonyBpp9000T::Ann childAnn;
    copyMem(&childAnn, result.childAnn, sizeof(childAnn));
    unsigned int annHash;
    KangarooTwelve(&childAnn, sizeof(childAnn), &annHash, sizeof(annHash));

    gAntColony.publishAnn(job.recordIndex, childAnn, annHash);
    // The cache every scoring path consults, so a strict replay of this solution is a lookup.
    const AntColonyBpp9000T::ReplayKey replayKey = makeAntReplayKey(record->pubkey, record->nonce, record->parentRef, job.anchorDigest);
    gAntColony.putReplayScore(replayKey, result.score, childAnn);
    gState.materialised.fetch_add(1, std::memory_order_relaxed);
    noteSuccess();
    if (gState.debug)
    {
        logLine("job %llu record %u score %u in %lld ms", (unsigned long long)result.jobId, job.recordIndex, result.score, walkMs);
    }
}

inline void checkDeadlines()
{
    const unsigned long long ema = gState.walkMsEma.load(std::memory_order_acquire);
    const long long deadlineMs = (long long)(ema * 3) > MIN_JOB_DEADLINE_MS
        ? (long long)(ema * 3) : MIN_JOB_DEADLINE_MS;
    const long long now = nowMs();

    for (size_t i = 0; i < gState.inFlight.size();)
    {
        if (now - gState.inFlight[i].sentAtMs < deadlineMs)
        {
            i++;
            continue;
        }
        const unsigned int index = gState.inFlight[i].recordIndex;
        const unsigned long long jobId = gState.inFlight[i].jobId;
        gState.inFlight.erase(gState.inFlight.begin() + (long)i);
        gAntColony.releaseAnnClaim(index);
        gState.deadlineExpiries.fetch_add(1, std::memory_order_relaxed);
        // A missing result says nothing about the record, so the bitmap is left alone.
        logLine("job %llu record %u no result in %lld ms, claim released, not marked", jobId, index, deadlineMs);
        gState.deadlineStreak++;
    }
    gState.inFlightCount.store((unsigned int)gState.inFlight.size(), std::memory_order_release);

    if (gState.deadlineStreak >= DEADLINE_STREAK_LIMIT)
    {
        gState.deadlineStreak = 0;
        dropLink("walker alive but not answering");
    }
}

// ── dispatcher ──────────────────────────────────────────────────────────────────────────────────

// Answers the on-demand path waiting on a record this link claimed. Only a job past the stale age is
// handed back: a healthy walk is worth waiting out, since taking it back discards all of its progress.
inline void servePreempt()
{
    const unsigned int index = gState.preemptRequest.load(std::memory_order_acquire);
    if (index == PREEMPT_NONE)
    {
        return;
    }

    const unsigned long long ema = gState.walkMsEma.load(std::memory_order_acquire);
    const long long staleMs = (long long)(ema * 2) > PREEMPT_MIN_AGE_MS ? (long long)(ema * 2) : PREEMPT_MIN_AGE_MS;
    for (size_t i = 0; i < gState.inFlight.size(); i++)
    {
        if (gState.inFlight[i].recordIndex != index || nowMs() - gState.inFlight[i].sentAtMs < staleMs)
        {
            continue;
        }
        const unsigned long long jobId = gState.inFlight[i].jobId;
        const long long ageMs = nowMs() - gState.inFlight[i].sentAtMs;
        gState.inFlight.erase(gState.inFlight.begin() + (long)i);
        gState.inFlightCount.store((unsigned int)gState.inFlight.size(), std::memory_order_release);
        gState.handedBack.store(index, std::memory_order_release);
        gAntColony.releaseAnnClaim(index);
        gState.preempted.fetch_add(1, std::memory_order_relaxed);
        logLine("job %llu record %u handed back to the tick after %lld ms, its result will be dropped", jobId, index, ageMs);
        break;
    }
    gState.preemptRequest.store(PREEMPT_NONE, std::memory_order_release);
}

// Reseeding invalidates every index in flight, so claims and scan state go before it proceeds.
inline void serveQuiesce()
{
    for (const InFlight& job : gState.inFlight)
    {
        gAntColony.releaseAnnClaim(job.recordIndex);
    }
    gState.inFlight.clear();
    gState.inFlightCount.store(0, std::memory_order_release);
    gState.preemptRequest.store(PREEMPT_NONE, std::memory_order_release);
    gState.handedBack.store(PREEMPT_NONE, std::memory_order_release);
    gState.cursor = 0;
    gState.disagreementStreak = 0;
    gState.deadlineStreak = 0;
    gState.rolledBackCandidates.clear();
    gState.failedBits.assign((size_t)ANT_MAX_NODES_PER_EPOCH / 8, 0);
    gState.failedCount.store(0, std::memory_order_release);
    gState.quiesceAcknowledged.store(true, std::memory_order_release);

    while (gState.quiesceRequested.load(std::memory_order_acquire) && !gState.stopping.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const int fd = gState.fd.load(std::memory_order_acquire);
    if (fd >= 0 && !sendHello(fd))
    {
        dropLink("hello write failed after epoch reset");
    }
}

inline bool readOneFrame(int fd, unsigned char* payload)
{
    AntWalkProto::FrameHeader header;
    if (!readFully(fd, &header, sizeof(header)))
    {
        dropLink("connection closed");
        return false;
    }
    if (header.magic != AntWalkProto::MAGIC || header.payloadSize > AntWalkProto::MAX_PAYLOAD_BYTES)
    {
        dropLink("bad frame");
        return false;
    }
    if (header.payloadSize && !readFully(fd, payload, header.payloadSize))
    {
        dropLink("short frame");
        return false;
    }

    if (header.type == AntWalkProto::MsgReady)
    {
        if (header.payloadSize != sizeof(AntWalkProto::ReadyPayload))
        {
            dropLink("bad ready frame");
            return false;
        }
        AntWalkProto::ReadyPayload ready;
        memcpy(&ready, payload, sizeof(ready));
        if ((int)ready.walkerPid == gState.suspectWalkerPid.load(std::memory_order_acquire))
        {
            logLine("walker pid %u already disagreed on every job, not using it", ready.walkerPid);
            dropLink("suspect walker");
            return false;
        }
        if (ready.status != AntWalkProto::ReadyOk)
        {
            logLine("handshake refused by the walker (status %u) - not retrying until it changes", ready.status);
            dropLink("handshake refused");
            return false;
        }
        // Also answers the hello resent on a seed rotation, which is a re-seed, not a new link.
        const bool wasReady = (LinkState)gState.link.load(std::memory_order_acquire) == LinkState::Ready;
        gState.threadCount = ready.threadCount;
        gState.walkerPid.store((int)ready.walkerPid, std::memory_order_release);
        gState.link.store((int)LinkState::Ready, std::memory_order_release);
        if (wasReady)
        {
            logLine("sidecar re-seeded for epoch %u", ready.epochId);
        }
        else
        {
            gState.reconnects.fetch_add(1, std::memory_order_relaxed);
            logLine("sidecar connected, pid %u, %u threads, epoch %u", ready.walkerPid, ready.threadCount, ready.epochId);
        }
    }
    else if (header.type == AntWalkProto::MsgResult)
    {
        if (header.payloadSize != sizeof(AntWalkProto::ResultPayload))
        {
            dropLink("bad result frame");
            return false;
        }
        AntWalkProto::ResultPayload result;
        memcpy(&result, payload, sizeof(result));
        applyResult(result);
    }
    return true;
}

inline void heartbeat()
{
    logLine("backlog %llu, done %llu (memo %llu), failed %llu, inflight %u/%u, walk avg %llu ms, link %s",
        (unsigned long long)gState.backlog.load(std::memory_order_acquire), (unsigned long long)gState.materialised.load(std::memory_order_acquire),
        (unsigned long long)gState.memoHits.load(std::memory_order_acquire), (unsigned long long)gState.failedCount.load(std::memory_order_acquire),
        gState.inFlightCount.load(std::memory_order_acquire), gState.threadCount, (unsigned long long)gState.walkMsEma.load(std::memory_order_acquire),
        linkName((LinkState)gState.link.load(std::memory_order_acquire)));
}

inline void dispatcherLoop()
{
    long long backoffMs = BACKOFF_MIN_MS;
    long long reconnectAtMs = 0;
    long long nextPingAtMs = nowMs() + PING_INTERVAL_MS;
    long long nextHeartbeatAtMs = nowMs() + HEARTBEAT_INTERVAL_MS;
    std::vector<unsigned char> payload(AntWalkProto::MAX_PAYLOAD_BYTES);

    while (!gState.stopping.load(std::memory_order_acquire))
    {
        servePreempt();

        if (nowMs() >= nextHeartbeatAtMs)
        {
            nextHeartbeatAtMs = nowMs() + HEARTBEAT_INTERVAL_MS;
            refreshBacklog();
            heartbeat();
        }

        if (gState.quiesceRequested.load(std::memory_order_acquire))
        {
            serveQuiesce();
            continue;
        }

        // A walker seeded before the first mining seed exists would disagree on every record.
        if (score == nullptr || isZero(score->currentRandomSeed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_SLICE_MS));
            continue;
        }

        const LinkState link = (LinkState)gState.link.load(std::memory_order_acquire);
        if (link != LinkState::Ready && link != LinkState::Handshaking)
        {
            const long long now = nowMs();
            if (now < reconnectAtMs)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_SLICE_MS));
                continue;
            }
            if (!tryConnect())
            {
                backoffMs = (backoffMs * 2 < BACKOFF_MAX_MS) ? backoffMs * 2 : BACKOFF_MAX_MS;
                reconnectAtMs = nowMs() + backoffMs;
                continue;
            }
            backoffMs = BACKOFF_MIN_MS;
            continue;
        }

        if (link == LinkState::Handshaking && nowMs() - gState.handshakeStartedAtMs > HANDSHAKE_DEADLINE_MS)
        {
            dropLink("no ready frame within the handshake deadline");
            continue;
        }

        const int fd = gState.fd.load(std::memory_order_acquire);
        if (fd < 0)
        {
            continue;
        }

        struct pollfd entry;
        entry.fd = fd;
        entry.events = POLLIN;
        entry.revents = 0;
        const int ready = poll(&entry, 1, POLL_SLICE_MS);
        if (ready > 0 && !readOneFrame(fd, payload.data()))
        {
            continue;
        }
        if (ready < 0 && errno != EINTR)
        {
            dropLink("poll failed");
            continue;
        }

        checkDeadlines();

        // Deeper only lets a wedged walker hold claims the on-demand path may need.
        if ((LinkState)gState.link.load(std::memory_order_acquire) == LinkState::Ready)
        {
            while (gState.inFlight.size() < gState.threadCount && dispatchOne())
            {
            }
        }

        const unsigned int generation = gState.seedGeneration.load(std::memory_order_acquire);
        if (generation != gState.helloGeneration && (LinkState)gState.link.load(std::memory_order_acquire) == LinkState::Ready)
        {
            const int helloFd = gState.fd.load(std::memory_order_acquire);
            if (helloFd < 0 || !sendHello(helloFd))
            {
                dropLink("hello write failed on seed change");
                continue;
            }
        }

        const long long now = nowMs();
        if (now >= nextPingAtMs)
        {
            nextPingAtMs = now + PING_INTERVAL_MS;
            const int liveFd = gState.fd.load(std::memory_order_acquire);
            if (liveFd >= 0 && !sendFrame(liveFd, AntWalkProto::MsgPing, nullptr, 0))
            {
                dropLink("ping write failed");
            }
        }
    }

    dropLink("shutting down");
}

// ── node-facing api ─────────────────────────────────────────────────────────────────────────────

inline void configure(const std::string& socketPath, unsigned int threadCount, bool debug)
{
    gState.socketPath = socketPath;
    gState.threadCount = threadCount;
    gState.debug = debug;
}

inline void start()
{
    if (!isEnabled() || gState.dispatcher != nullptr)
    {
        return;
    }
    gState.stopping.store(false, std::memory_order_release);
    gState.link.store((int)LinkState::Disconnected, std::memory_order_release);
    gState.dispatcher = new std::thread(dispatcherLoop);
}

inline void stop()
{
    if (gState.dispatcher == nullptr)
    {
        return;
    }
    gState.stopping.store(true, std::memory_order_release);
    gState.dispatcher->join();
    delete gState.dispatcher;
    gState.dispatcher = nullptr;
}

// The pool seed changed, so results still in flight were computed against the old one.
inline void onEpochBegin()
{
    gState.seedGeneration.fetch_add(1, std::memory_order_acq_rel);
}

inline void quiesceBegin()
{
    if (gState.dispatcher == nullptr)
    {
        return;
    }
    gState.quiesceAcknowledged.store(false, std::memory_order_release);
    gState.quiesceRequested.store(true, std::memory_order_release);

    const long long startedAtMs = nowMs();
    while (!gState.quiesceAcknowledged.load(std::memory_order_acquire))
    {
        if (nowMs() - startedAtMs > 5'000)
        {
            logLine("quiesce not acknowledged in 5000 ms, continuing");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    logLine("quiesce for epoch reset, waited %lld ms", nowMs() - startedAtMs);
}

inline void quiesceEnd()
{
    gState.quiesceRequested.store(false, std::memory_order_release);
}

// Asks the dispatcher to hand back a record the on-demand path is stuck behind. The caller re-checks
// the claim afterwards: a claim held by another node processor is not the walker's to release.
inline void preemptClaim(unsigned int index)
{
    if (!isEnabled() || gState.inFlightCount.load(std::memory_order_acquire) == 0)
    {
        return;
    }
    unsigned int slot = PREEMPT_NONE;
    if (!gState.preemptRequest.compare_exchange_strong(slot, index, std::memory_order_acq_rel))
    {
        return;
    }

    const long long deadlineMs = nowMs() + PREEMPT_ACK_WAIT_MS;
    while (gState.preemptRequest.load(std::memory_order_acquire) == index && nowMs() < deadlineMs)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // A dispatcher that never answered must not leave the slot taken for every later request.
    slot = index;
    gState.preemptRequest.compare_exchange_strong(slot, PREEMPT_NONE, std::memory_order_acq_rel);
}

// The fork child inherits the descriptor; two readers on one socket interleave results.
inline void closeInheritedSocket()
{
    const int fd = gState.fd.exchange(-1);
    if (fd >= 0)
    {
        close(fd);
    }
    gState.link.store((int)LinkState::Disconnected, std::memory_order_release);
}

// The promote path swept the claims, so only the client's own view is rebuilt here.
inline void restartAfterPromote()
{
    closeInheritedSocket();
    // Only the handle came through fork(), so it is abandoned rather than joined or destroyed.
    gState.dispatcher = nullptr;
    gState.inFlight.clear();
    gState.inFlightCount.store(0, std::memory_order_release);
    gState.preemptRequest.store(PREEMPT_NONE, std::memory_order_release);
    gState.handedBack.store(PREEMPT_NONE, std::memory_order_release);
    gState.quiesceRequested.store(false, std::memory_order_release);
    gState.quiesceAcknowledged.store(true, std::memory_order_release);
    gState.cursor = 0;
    gState.disagreementStreak = 0;
    gState.deadlineStreak = 0;
    gState.rolledBackCandidates.clear();
    start();
    if (isEnabled())
    {
        logLine("dispatcher restarted after promote");
    }
}

inline std::string statsJson()
{
    char buffer[896];
    snprintf(buffer, sizeof(buffer),
        "{\"enabled\":%s,\"state\":\"%s\",\"socket\":\"%s\",\"threads\":%u,"
        "\"inflight\":%u,\"backlog\":%llu,\"materialised\":%llu,\"failed\":%llu,"
        "\"jobsSent\":%llu,\"disagreements\":%llu,\"deadlineExpiries\":%llu,"
        "\"staleDropped\":%llu,\"reconnects\":%llu,\"walkAvgMs\":%llu,"
        "\"memoHits\":%llu,\"preempted\":%llu,\"epochId\":%u}",
        isEnabled() ? "true" : "false", linkName((LinkState)gState.link.load(std::memory_order_acquire)), gState.socketPath.c_str(), gState.threadCount,
        gState.inFlightCount.load(std::memory_order_acquire), (unsigned long long)gState.backlog.load(std::memory_order_acquire),
        (unsigned long long)gState.materialised.load(std::memory_order_acquire), (unsigned long long)gState.failedCount.load(std::memory_order_acquire),
        (unsigned long long)gState.jobsSent.load(std::memory_order_acquire), (unsigned long long)gState.disagreements.load(std::memory_order_acquire),
        (unsigned long long)gState.deadlineExpiries.load(std::memory_order_acquire), (unsigned long long)gState.staleDropped.load(std::memory_order_acquire),
        (unsigned long long)gState.reconnects.load(std::memory_order_acquire), (unsigned long long)gState.walkMsEma.load(std::memory_order_acquire),
        (unsigned long long)gState.memoHits.load(std::memory_order_acquire), (unsigned long long)gState.preempted.load(std::memory_order_acquire),
        gState.seedGeneration.load(std::memory_order_acquire));
    return std::string(buffer);
}
}

#else

#include <string>

namespace AntWalker
{
inline void configure(const std::string&, unsigned int, bool) {}
inline void start() {}
inline void stop() {}
inline void onEpochBegin() {}
inline void quiesceBegin() {}
inline void quiesceEnd() {}
inline void preemptClaim(unsigned int) {}
inline void closeInheritedSocket() {}
inline void restartAfterPromote() {}
inline std::string statsJson() { return std::string("{\"enabled\":false}"); }
}

#endif // ANT_WALKER && !_WIN32
