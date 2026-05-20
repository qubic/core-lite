#pragma once
#include "../utils.h"
#include "extensions/utils.h"
#include "explorer_assets.generated.h"

#include <drogon/HttpController.h>

using namespace drogon;

namespace Explorer
{
class ExplorerController : public HttpController<ExplorerController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ExplorerController::page, "/explorer",            Get);
    ADD_METHOD_TO(ExplorerController::css,  "/explorer/style.css",  Get);
    ADD_METHOD_TO(ExplorerController::js,   "/explorer/app.js",     Get);
    ADD_METHOD_TO(ExplorerController::data, "/explorer/data",       Get);
    METHOD_LIST_END

    inline void page(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& cb)
    { serveEmbedded(cb, EXPLORER_INDEX_HTML, CT_TEXT_HTML); }

    inline void css(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& cb)
    { serveEmbedded(cb, EXPLORER_STYLE_CSS, CT_TEXT_CSS); }

    inline void js(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& cb)
    { serveEmbedded(cb, EXPLORER_APP_JS, CT_TEXT_JAVASCRIPT); }

    inline void data(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& cb)
    {
        Json::Value out;

        // header
        out["header"]["tick"]                = system.tick;
        out["header"]["epoch"]               = system.epoch;
        out["header"]["initialTick"]         = system.initialTick;
        out["header"]["alignedVotes"]        = gTickNumberOfComputors;
        out["header"]["misalignedVotes"]     = gTickTotalNumberOfComputors - gTickNumberOfComputors;
        out["header"]["mainAuxStatus"]       = mainAuxStatus;
        out["header"]["isSavingSnapshot"]    = (bool)persistingNodeStateTickProcWaiting;
        out["header"]["ticksInCurrentEpoch"] = system.tick - system.initialTick;
        out["header"]["latestCreatedTick"]   = latestCreatedTickInfo.tick;

        // recentTicks: last 20 ticks within current epoch only (oldest → newest)
        constexpr unsigned int N = 20;
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
                row["timestamp"] = HttpUtils::formatTimestamp(
                    td->millisecond, td->second, td->minute, td->hour,
                    td->day, td->month, td->year);
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

        // mempool
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

        // computors (676 rows; aligned status not per-row available, omit field)
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

        // network
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

        // mining: top-10 miners
        Json::Value topMiners(Json::arrayValue);
        ACQUIRE(minerScoreArrayLock);
        const unsigned int topN = (numberOfMiners < 10) ? numberOfMiners : 10;
        for (unsigned int i = 0; i < topN; i++)
        {
            Json::Value m;
            CHAR16 id[61] = {};
            getIdentity((unsigned char*)&minerPublicKeys[i], id, false);
            m["publicKey"] = wchar_to_string(id);
            m["score"]     = minerScores[i];
            topMiners.append(m);
        }
        RELEASE(minerScoreArrayLock);
        out["mining"]["numberOfSolutions"] = system.numberOfSolutions;
        out["mining"]["topMiners"]         = topMiners;

        // spectrum
        out["spectrum"]["circulatingSupply"] = std::to_string(spectrumInfo.totalAmount);
        out["spectrum"]["activeAddresses"]   = spectrumInfo.numberOfEntities;

        // node state — etalonTick is the local consensus snapshot for the next tick
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

        cb(HttpResponse::newHttpJsonResponse(out));
    }

private:
    static void serveEmbedded(std::function<void(const HttpResponsePtr&)>& cb,
                              const char* body,
                              ContentType ct)
    {
        auto resp = HttpResponse::newHttpResponse();
        resp->setBody(body);
        resp->setContentTypeCode(ct);
        resp->addHeader("Cache-Control", "no-store, must-revalidate");
        resp->addHeader("Pragma", "no-cache");
        cb(resp);
    }
};
} // namespace Explorer
