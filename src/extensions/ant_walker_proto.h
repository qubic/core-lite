#pragma once

// Wire format shared by the walker client and the walker process. Fixed-size payloads only, so a
// frame is one read of a known length.

#include "score.h"
#include "public_settings.h"

namespace AntWalkProto
{
static constexpr unsigned int MAGIC = 0x57544E41u;   // "ANTW"
static constexpr unsigned int VERSION = 1;

static constexpr unsigned int ANN_BYTES = (unsigned int)sizeof(score_engine::ScoreBpp9000T::ANN);

// Both sides compile the same scorer, so only a build disagreeing on these params scores
// differently. Compared at the handshake, before a job runs.
constexpr unsigned int mixConfig(unsigned int accumulated, unsigned long long value)
{
    for (unsigned int byteIndex = 0; byteIndex < 8; byteIndex++)
    {
        accumulated = (accumulated ^ (unsigned int)((value >> (byteIndex * 8)) & 0xFF)) * 16777619u;
    }
    return accumulated;
}

constexpr unsigned int CONFIG_HASH =
    mixConfig(mixConfig(mixConfig(mixConfig(mixConfig(mixConfig(mixConfig(mixConfig(mixConfig(mixConfig(
        2166136261u,
        BPP9000_NUMBER_OF_INPUT_NEURONS),
        BPP9000_NUMBER_OF_OUTPUT_NEURONS),
        BPP9000_SEQUENCE_LENGTH),
        BPP9000_WINDOW_WIDTH),
        BPP9000_MAX_NUMBER_OF_TICKS),
        BPP9000_NUMBER_OF_NEIGHBORS),
        BPP9000_POPULATION_THRESHOLD),
        BPP9000_NUMBER_OF_MUTATIONS),
        BPP9000_SOLUTION_THRESHOLD_DEFAULT),
        (unsigned long long)ANN_BYTES);

enum MessageType : unsigned int
{
    MsgHello = 1,      // node -> walker
    MsgReady = 2,      // walker -> node
    MsgJob = 3,        // node -> walker
    MsgResult = 4,     // walker -> node
    MsgPing = 5,       // node -> walker
    MsgPong = 6,       // walker -> node
};

enum ResultStatus : unsigned int
{
    ResultOk = 0,
    ResultUnscorable = 1,   // the engine returned INVALID_SCORE_VALUE, childAnn is not written
    ResultStaleEpoch = 2,   // the job named an epoch the walker is no longer seeded for
};

enum ReadyStatus : unsigned int
{
    ReadyOk = 0,
    ReadyVersionMismatch = 1,
    ReadyTaskMismatch = 2,
    ReadySeedFailed = 3,
    ReadyConfigMismatch = 4,
};

struct FrameHeader
{
    unsigned int magic;
    unsigned int type;
    unsigned int payloadSize;
    unsigned int reserved;
};

struct HelloPayload
{
    unsigned int version;
    unsigned int epochId;
    unsigned int annBytes;
    unsigned int configHash;
    unsigned char miningSeed[32];
    // The pinned task identity both binaries were compiled against; a mismatch means the two would
    // score the same nonce differently, which is worth refusing before a single job runs.
    unsigned char topologyHash[32];
    unsigned char dataHash[32];
};

struct ReadyPayload
{
    unsigned int version;
    unsigned int status;
    unsigned int threadCount;
    unsigned int annBytes;
    unsigned int epochId;
    unsigned int walkerPid;   // lets the node refuse a walker it has already found to be wrong
};

struct JobPayload
{
    unsigned long long jobId;
    unsigned int epochId;
    unsigned int isRoot;             // parentAnn is unused when set; the walker derives the miner's root
    unsigned char pubkey[32];
    unsigned char nonce[32];
    unsigned char anchorDigest[32];
    unsigned char parentAnn[ANN_BYTES];
};

struct ResultPayload
{
    unsigned long long jobId;
    unsigned int epochId;
    unsigned int status;
    unsigned int score;
    unsigned int reserved;
    unsigned char childAnn[ANN_BYTES];
};

static_assert(sizeof(FrameHeader) == 16, "FrameHeader must stay 16 bytes on the wire");
static_assert(sizeof(HelloPayload) == 112, "HelloPayload layout changed - bump VERSION");
static_assert(sizeof(ReadyPayload) == 24, "ReadyPayload layout changed - bump VERSION");
static_assert(sizeof(JobPayload) == 16 + 96 + ANN_BYTES, "JobPayload layout changed - bump VERSION");
static_assert(sizeof(ResultPayload) == 24 + ANN_BYTES, "ResultPayload layout changed - bump VERSION");

// The largest frame either side ever reads, so both can size one static buffer.
static constexpr unsigned int MAX_PAYLOAD_BYTES =
    (sizeof(JobPayload) > sizeof(ResultPayload)) ? (unsigned int)sizeof(JobPayload) : (unsigned int)sizeof(ResultPayload);
}
