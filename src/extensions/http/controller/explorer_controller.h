#pragma once

// Explorer endpoints as RpcReq->RpcResp router handlers.

#if defined(__linux__) || defined(LITE_WASM_SC)

#include <string>
#include <vector>
#include <memory>
#include <cmath>
#include <algorithm>
#include "extensions/rpc/rpc_core.h"
#include "extensions/http/controller/explorer_assets.generated.h"

static RpcResp rpcEmbedded(const char* body, const char* contentType)
{
    RpcResp r;
    r.contentType = contentType;
    r.body = body;
    r.headers.push_back({ "Cache-Control", "no-store, must-revalidate" });
    r.headers.push_back({ "Pragma", "no-cache" });
    return r;
}

// ============================ explorer ============================
RPC_ROUTE("GET", "/explorer")        { (void)req; return rpcEmbedded(EXPLORER_INDEX_HTML, "text/html; charset=utf-8"); }
RPC_ROUTE("GET", "/explorer/")       { (void)req; return rpcEmbedded(EXPLORER_INDEX_HTML, "text/html; charset=utf-8"); }
RPC_ROUTE("GET", "/explorer/style.css") { (void)req; return rpcEmbedded(EXPLORER_STYLE_CSS, "text/css"); }
RPC_ROUTE("GET", "/explorer/app.js") { (void)req; return rpcEmbedded(EXPLORER_APP_JS, "text/javascript; charset=utf-8"); }

RPC_ROUTE("GET", "/explorer/data")
{
    (void)req;
    Json::Value out;

    out["header"]["tick"]                = system.tick;
    out["header"]["epoch"]               = system.epoch;
    out["header"]["initialTick"]         = system.initialTick;
    out["header"]["alignedVotes"]        = gTickNumberOfComputors;
    out["header"]["misalignedVotes"]     = gTickTotalNumberOfComputors - gTickNumberOfComputors;
    out["header"]["mainAuxStatus"]       = mainAuxStatus;
    out["header"]["isSavingSnapshot"]    = (bool)persistingNodeStateTickProcWaiting;
    out["header"]["ticksInCurrentEpoch"] = system.tick - system.initialTick;
    out["header"]["latestCreatedTick"]   = latestCreatedTickInfo.tick;

    constexpr unsigned int N = 200;
    Json::Value recent(Json::arrayValue);
    TickStorage::tickData.acquireLock();
    unsigned int start = (system.tick >= N) ? (system.tick - N + 1) : 0;
    if (start < system.initialTick) start = system.initialTick;
    for (unsigned int t = start; t <= system.tick; t++)
    {
        TickData* td = TickStorage::tickData.getByTickIfNotEmpty(t);
        Json::Value row;
        row["tick"] = t;
        const m256i& leaderKey = broadcastedComputors.computors.publicKeys[t % NUMBER_OF_COMPUTORS];
        CHAR16 leaderId[61] = {};
        getIdentity((unsigned char*)&leaderKey, leaderId, false);
        row["leader"] = wchar_to_string(leaderId);
        if (td)
        {
            unsigned int txc = 0;
            for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
                if (!isZero(td->transactionDigests[i])) txc++;
            row["empty"]     = false;
            row["txCount"]   = txc;
            row["timestamp"] = HttpUtils::formatTimestamp(td->millisecond, td->second, td->minute, td->hour, td->day, td->month, td->year);
        }
        else
        {
            row["empty"]     = true;
            row["txCount"]   = 0;
            row["timestamp"] = "";
        }
        recent.append(row);
    }
    TickStorage::tickData.releaseLock();
    out["recentTicks"] = recent;

    Json::Value perTick(Json::arrayValue);
    for (unsigned int t = system.tick; t <= system.tick + 10; t++)
    {
        Json::Value e;
        e["tick"]  = t;
        e["count"] = pendingTxsPool.getNumberOfPendingTickTxs(t);
        perTick.append(e);
    }
    out["mempool"]["totalPending"] = pendingTxsPool.getTotalNumberOfPendingTxs(system.tick);
    out["mempool"]["perTick"]      = perTick;

    Json::Value computors(Json::arrayValue);
    for (unsigned int i = 0; i < NUMBER_OF_COMPUTORS; i++)
    {
        Json::Value c;
        c["index"] = i;
        CHAR16 id[61] = {};
        getIdentity((unsigned char*)&broadcastedComputors.computors.publicKeys[i], id, false);
        c["publicKey"] = wchar_to_string(id);
        computors.append(c);
    }
    out["computors"] = computors;

    unsigned int connected = 0, outg = 0, inc = 0;
    constexpr unsigned int peersLen = sizeof(peers) / sizeof(peers[0]);
    for (unsigned int i = 0; i < peersLen; i++)
    {
        if (peers[i].isConnectedAccepted && !peers[i].isClosing)
        {
            connected++;
            if (peers[i].isIncommingConnection) inc++; else outg++;
        }
    }
    out["network"]["connectedPeers"] = connected;
    out["network"]["outgoing"]       = outg;
    out["network"]["incoming"]       = inc;

    Json::Value topMiners(Json::arrayValue);
    ACQUIRE(minerScoreArrayLock);
    const unsigned int topN = (numberOfMiners < 10) ? numberOfMiners : 10;
    for (unsigned int i = 0; i < topN; i++)
    {
        Json::Value m;
        CHAR16 id[61] = {};
        getIdentity((unsigned char*)&minerPublicKeys[i], id, false);
        m["publicKey"] = wchar_to_string(id);
        // The ranking key holds the reach in its high half, inverted so that further sorts first.
        const unsigned int rankingKey = minerScores[i];
        const bool hasSolution = (rankingKey != NO_MINER_SCORE);
        m["score"]     = hasSolution ? (rankingKey & 0xFFFFU) : rankingKey;
        m["shift"]     = hasSolution ? (0xFFFFU - (rankingKey >> 16)) : 0U;
        topMiners.append(m);
    }
    RELEASE(minerScoreArrayLock);
    out["mining"]["numberOfSolutions"] = system.numberOfSolutions;
    out["mining"]["topMiners"]         = topMiners;

    out["spectrum"]["circulatingSupply"] = std::to_string(spectrumInfo.totalAmount);
    out["spectrum"]["activeAddresses"]   = spectrumInfo.numberOfEntities;

    m256i sSpec = etalonTick.saltedSpectrumDigest;
    m256i sUniv = etalonTick.saltedUniverseDigest;
    m256i sComp = etalonTick.saltedComputerDigest;
    m256i pSpec = etalonTick.prevSpectrumDigest;
    m256i pUniv = etalonTick.prevUniverseDigest;
    m256i pComp = etalonTick.prevComputerDigest;
    out["state"]["saltedSpectrumDigest"]      = base64_encode(sSpec.m256i_u8, 32);
    out["state"]["saltedUniverseDigest"]      = base64_encode(sUniv.m256i_u8, 32);
    out["state"]["saltedComputerDigest"]      = base64_encode(sComp.m256i_u8, 32);
    out["state"]["prevSpectrumDigest"]        = base64_encode(pSpec.m256i_u8, 32);
    out["state"]["prevUniverseDigest"]        = base64_encode(pUniv.m256i_u8, 32);
    out["state"]["prevComputerDigest"]        = base64_encode(pComp.m256i_u8, 32);
    out["state"]["saltedResourceTestingDigest"] = etalonTick.saltedResourceTestingDigest;
    out["state"]["prevResourceTestingDigest"]   = etalonTick.prevResourceTestingDigest;
    out["state"]["saltedTransactionBodyDigest"] = etalonTick.saltedTransactionBodyDigest;
    out["state"]["prevTransactionBodyDigest"]   = etalonTick.prevTransactionBodyDigest;
    out["state"]["resourceTestingDigest"]       = (Json::UInt64)resourceTestingDigest;

    return jsonResp(out);
}

#endif // __linux__ || LITE_WASM_SC
