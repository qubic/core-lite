#pragma once

// Solution-check endpoints for the guardian team: what this node stored for an ant solution, and a signed
// request to score one on demand.

#if defined(__linux__) || defined(LITE_WASM_SC)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include "extensions/rpc/rpc_core.h"
#include "extensions/ant_solution_check.h"
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// False on a node that takes claimed scores on trust. Such a node answers no score request.
static bool antNodeVerifiesEveryScore()
{
    return !tickFork::gRollbackAvailable || isMainMode() || forceVerifySolutions;
}

static Json::Value antCheckRecordJson(unsigned int recordIndex, const AntSolutionRecord& record)
{
    CHAR16 identity[61] = {};
    getIdentity(record.pubkey.m256i_u8, identity, false);

    Json::Value item;
    item["recordIndex"] = recordIndex;
    item["identity"] = wchar_to_string(identity);
    item["nonce"] = byteToHex(record.nonce.m256i_u8, 32);
    item["tick"] = record.selfRef.tick;
    item["index"] = record.selfRef.solutionIndexInTick;
    item["parentTick"] = record.parentRef.tick;
    item["parentIndex"] = record.parentRef.solutionIndexInTick;
    item["anchorTick"] = record.anchorTick;
    item["depth"] = record.depth;
    item["score"] = record.score;
    item["shift"] = record.shift;
    item["networkHash"] = record.childAnnHash;
    item["networkHeld"] = gAntColony.isAnnMaterialised(recordIndex);
    item["trustedAtCommit"] = (record.padding == ANT_RECORD_TRUSTED_AT_COMMIT);
    return item;
}

static RpcResp antCheckStatus(const char* status, int httpStatus = 200)
{
    Json::Value json;
    json["status"] = status;
    return jsonResp(json, httpStatus);
}

// Stored solutions of one tick (tick, optional index) or of one identity (identity, optional nonce). The scan
// starts at recordIndexFrom, skips the first offset matches and returns up to limit. Reads the store without a
// lock, as every off-thread reader of it does.
RPC_ROUTE("GET", "/v1/ant-solutions")
{
    const std::string identityText = req.getParameter("identity");
    const std::string nonceText = req.getParameter("nonce");
    const std::string tickText = req.getParameter("tick");
    const std::string indexText = req.getParameter("index");
    const std::string recordIndexFromText = req.getParameter("recordIndexFrom");
    const std::string offsetText = req.getParameter("offset");
    const std::string limitText = req.getParameter("limit");

    m256i identityKey = m256i::zero();
    m256i nonce = m256i::zero();
    unsigned int tick = 0;
    unsigned int index = 0;
    unsigned int recordIndexFrom = 0;
    unsigned int offset = 0;
    unsigned int limit = ANT_IDENTITY_TREE_NODES_PER_RESPONSE;

    const bool parametersValid = (!identityText.empty() || !tickText.empty())
        && (identityText.empty() || antParseIdentity(identityText, identityKey))
        && (nonceText.empty() || antParseHex(nonceText, nonce.m256i_u8, 32))
        && (tickText.empty() || antParseUnsigned(tickText, tick))
        && (indexText.empty() || antParseUnsigned(indexText, index))
        && (recordIndexFromText.empty() || antParseUnsigned(recordIndexFromText, recordIndexFrom))
        && (offsetText.empty() || antParseUnsigned(offsetText, offset))
        && (limitText.empty() || (antParseUnsigned(limitText, limit) && limit >= 1 && limit <= ANT_IDENTITY_TREE_NODES_PER_RESPONSE));
    if (!parametersValid)
    {
        return antCheckStatus("bad-request", 400);
    }

    unsigned int rangeStart = 0;
    unsigned int rangeEnd = gAntColony.solutionCount();
    if (!tickText.empty())
    {
        unsigned int tickRecordCount = 0;
        if (!gAntColony.tickRun(tick, rangeStart, tickRecordCount))
        {
            rangeStart = 0;
        }
        rangeEnd = rangeStart + tickRecordCount;
    }

    Json::Value solutions(Json::arrayValue);
    unsigned int recordIndex = std::max(recordIndexFrom, rangeStart);
    unsigned int matchesToSkip = offset;
    while (recordIndex < rangeEnd && solutions.size() < limit)
    {
        const AntSolutionRecord* record = gAntColony.recordAt(recordIndex);
        if (record == nullptr)
        {
            recordIndex = rangeEnd;
            break;
        }

        const bool matches = (identityText.empty() || record->pubkey == identityKey)
            && (nonceText.empty() || record->nonce == nonce)
            && (indexText.empty() || record->selfRef.solutionIndexInTick == index);
        if (matches && matchesToSkip > 0)
        {
            matchesToSkip--;
        }
        else if (matches)
        {
            solutions.append(antCheckRecordJson(recordIndex, *record));
        }
        recordIndex++;
    }

    Json::Value result;
    result["epoch"] = (unsigned int)system.epoch;
    result["nodeVerifiesScores"] = antNodeVerifiesEveryScore();
    result["nextRecordIndexFrom"] = (recordIndex < rangeEnd) ? recordIndex : 0u;
    result["solutions"] = solutions;
    return jsonResp(result);
}

// Only one request walks at a time; a second one is told so instead of queueing behind it.
static std::atomic_flag gAntCheckWalkInFlight = ATOMIC_FLAG_INIT;

struct AntCheckWalkSlot
{
    const bool acquired = !gAntCheckWalkInFlight.test_and_set(std::memory_order_acquire);

    ~AntCheckWalkSlot()
    {
        if (acquired)
        {
            gAntCheckWalkInFlight.clear(std::memory_order_release);
        }
    }
};

#ifdef __linux__
// Nice is per thread on Linux, so only the walk yields the CPU to node threads. Raising it back can be
// refused without privilege; the handler thread serves one request, so nothing else runs at that level.
struct AntCheckLowPriority
{
    const int threadId = (int)syscall(SYS_gettid);
    const int previousNice = getpriority(PRIO_PROCESS, (id_t)threadId);

    AntCheckLowPriority()
    {
        setpriority(PRIO_PROCESS, (id_t)threadId, 19);
    }

    ~AntCheckLowPriority()
    {
        setpriority(PRIO_PROCESS, (id_t)threadId, previousNice);
    }
};
#endif

static RpcResp antCheckScoreReply(const score_engine::Rating& rating, const AntColonyBpp9000T::Ann& childAnn, bool cached, long long walkMs)
{
    Json::Value json;
    json["status"] = rating.isValid() ? "ok" : "no-score";
    json["cached"] = cached;
    if (rating.isValid())
    {
        unsigned int networkHash = 0;
        KangarooTwelve(&childAnn, sizeof(childAnn), &networkHash, sizeof(networkHash));
        json["score"] = rating.error;
        json["shift"] = rating.shift;
        json["networkHash"] = networkHash;
    }
    if (!cached)
    {
        json["walkMs"] = (Json::Int64)walkMs;
    }
    return jsonResp(json);
}

// Scores one child of a stored parent for the guardian identity, the way an incoming solution is scored, and
// changes nothing but the score cache. It walks on the engine no node thread maps to and reads the anchor from
// the recent-ticks ring only, so it holds nothing the tick or request processors wait for.
RPC_ROUTE("GET", "/v1/ant-score")
{
    static m256i guardianKey = m256i::zero();
    static const bool guardianSet = antParseIdentity(ANT_CHECK_GUARDIAN_IDENTITY, guardianKey);
    if (!guardianSet)
    {
        return antCheckStatus("disabled", 503);
    }

    m256i publicKey = m256i::zero();
    m256i nonce = m256i::zero();
    unsigned int anchorTick = 0;
    SolutionRef parentRef = ROOT_REF;
    unsigned char signature[SIGNATURE_SIZE];
    const bool parametersValid = antParseIdentity(req.getParameter("identity"), publicKey)
        && antParseHex(req.getParameter("nonce"), nonce.m256i_u8, 32)
        && antParseUnsigned(req.getParameter("anchorTick"), anchorTick)
        && antParseUnsigned(req.getParameter("parentTick"), parentRef.tick)
        && antParseUnsigned(req.getParameter("parentIndex"), parentRef.solutionIndexInTick)
        && antParseHex(req.getParameter("signature"), signature, SIGNATURE_SIZE);
    if (!parametersValid)
    {
        return antCheckStatus("bad-request", 400);
    }

    if (!antChallengeSignatureValid(guardianKey, publicKey, nonce, anchorTick, parentRef.tick, parentRef.solutionIndexInTick, signature))
    {
        return antCheckStatus("bad-signature", 401);
    }

    if (!antNodeVerifiesEveryScore())
    {
        return antCheckStatus("node-trusts-scores");
    }

    if (!score_engine::ScoreEngineT::isCanonicalAntNonce(nonce.m256i_u8))
    {
        return antCheckStatus("no-score");
    }

    m256i anchorDigest;
    if (!gAntColony.getAnchorDigest(anchorTick, anchorDigest))
    {
        return antCheckStatus("anchor-unavailable");
    }

    const AntSolutionRecord* parentRecord = nullptr;
    if (gAntColony.tryGetParent(parentRef, &parentRecord) != ValidityResult::Valid)
    {
        return antCheckStatus("parent-unknown");
    }
    if (parentRecord != nullptr && !(parentRecord->pubkey == publicKey))
    {
        return antCheckStatus("wrong-tree");
    }

    const auto parentAnn = std::make_unique<AntColonyBpp9000T::Ann>();
    const auto childAnn = std::make_unique<AntColonyBpp9000T::Ann>();
    // A root parent sits at frame 0 and is derived by the scorer from the identity.
    const unsigned int parentShift = (parentRecord != nullptr) ? parentRecord->shift : 0u;
    if (parentRecord != nullptr && !gAntColony.annOfNonRoot(*parentRecord, *parentAnn))
    {
        return antCheckStatus("parent-network-not-held");
    }

    const AntColonyBpp9000T::ReplayKey replayKey = makeAntReplayKey(publicKey, nonce, parentRef, anchorDigest);
    score_engine::Rating rating = score_engine::Rating::worst();
    if (gAntColony.tryGetReplayScore(replayKey, rating, *childAnn))
    {
        return antCheckScoreReply(rating, *childAnn, true, 0);
    }

    std::vector<unsigned char> targets(BPP9000_SEQUENCE_LENGTH);
    unsigned int shiftLimit = 0;
    unsigned int baseHour = 0;
    const unsigned int targetCount =
        gBtcPriceFeed.taskDataAt(anchorTick, targets.data(), (unsigned int)BPP9000_SEQUENCE_LENGTH, shiftLimit, baseHour);
    if (targetCount == 0)
    {
        return antCheckStatus("no-price-series");
    }

    const AntCheckWalkSlot walkSlot;
    if (!walkSlot.acquired)
    {
        return antCheckStatus("busy", 429);
    }

    const auto walkStart = std::chrono::steady_clock::now();
    {
#ifdef __linux__
        const AntCheckLowPriority lowPriority;
#endif
        rating = score->computeAntChildScore(score->RESERVED_ENGINE, (parentRecord != nullptr) ? parentAnn.get() : nullptr, parentShift,
            targets.data(), targetCount, publicKey, nonce, anchorDigest, *childAnn);
    }
    // Kept whatever the outcome: the walk is deterministic, so the same request never walks twice.
    gAntColony.putReplayScore(replayKey, rating, *childAnn);
    const long long walkMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - walkStart).count();

    return antCheckScoreReply(rating, *childAnn, false, walkMs);
}

#endif // __linux__ || LITE_WASM_SC
