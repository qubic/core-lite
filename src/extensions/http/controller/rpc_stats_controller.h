#pragma once

// Stats endpoints as RpcReq->RpcResp router handlers.

#if defined(__linux__) || defined(LITE_WASM_SC)

#include <string>
#include <vector>
#include <memory>
#include <cmath>
#include <algorithm>
#include "extensions/rpc/rpc_core.h"
#include "extensions/fork_stats.h"

// ============================ stats (/v1/...) ============================
RPC_ROUTE("GET", "/v1/peer-stats")
{
    (void)req;
    using namespace std;
    Json::Value result;

    Json::Value reasons;
    for (unsigned int r = 0; r < PeerDisc::REASON_COUNT; r++)
        reasons[PeerDisc::kName[r]] = Json::UInt64(PeerDisc::gReasonCount[r].load(memory_order_relaxed));
    result["disconnectReasons"] = reasons;
    result["disconnectTotal"] = Json::UInt64(PeerDisc::gTotal.load(memory_order_relaxed));
    unsigned int last = PeerDisc::gLastReason.load(memory_order_relaxed);
    result["lastReason"] = PeerDisc::kName[last < PeerDisc::REASON_COUNT ? last : 0];

    unsigned int connected = 0, handshaked = 0;
    Json::Value slots(Json::arrayValue);
    unsigned int n = NUMBER_OF_OUTGOING_CONNECTIONS + NUMBER_OF_INCOMING_CONNECTIONS;
    for (unsigned int i = 0; i < n; i++)
    {
        auto &p = peers[i];
        Json::Value e;
        e["slot"] = i;
        e["outgoing"] = (i < NUMBER_OF_OUTGOING_CONNECTIONS);
        e["hasConn"] = (((unsigned long long)p.tcp4Protocol) > 1);
        e["connected"] = (bool)p.isConnectedAccepted;
        e["handshaked"] = (bool)p.exchangedPublicPeers;
        e["closing"] = (bool)p.isClosing;
        e["incoming"] = (bool)p.isIncommingConnection;
        e["peerReportedTick"] = p.peerReportedTick;
        e["lastActiveTick"] = p.lastActiveTick;
        e["ip"] = std::to_string(p.address.u8[0]) + "." + std::to_string(p.address.u8[1]) + "." +
                  std::to_string(p.address.u8[2]) + "." + std::to_string(p.address.u8[3]);
        unsigned int sc = (i < PeerDisc::MAX_SLOTS) ? PeerDisc::gSlotCount[i].load(memory_order_relaxed) : 0;
        unsigned int sr = (i < PeerDisc::MAX_SLOTS) ? PeerDisc::gSlotLastReason[i].load(memory_order_relaxed) : 0;
        e["disconnects"] = Json::UInt(sc);
        e["lastReason"] = PeerDisc::kName[sr < PeerDisc::REASON_COUNT ? sr : 0];
        e["rxBytes"] = Json::UInt64((i < PeerDisc::MAX_SLOTS) ? PeerDisc::gSlotRxBytes[i].load(memory_order_relaxed) : 0);
        e["txBytes"] = Json::UInt64((i < PeerDisc::MAX_SLOTS) ? PeerDisc::gSlotTxBytes[i].load(memory_order_relaxed) : 0);
        if (p.isConnectedAccepted) connected++;
        if (p.exchangedPublicPeers) handshaked++;
        slots.append(e);
    }
    result["connectedCount"] = connected;
    result["handshakedCount"] = handshaked;
    result["peers"] = slots;
    result["currentTick"] = system.tick;
    return jsonResp(result);
}

RPC_ROUTE("GET", "/v1/tick-bench")
{
    using namespace std;
    unsigned long long freq = frequency;
    auto toUs = [freq](uint64_t t) -> Json::UInt64
    { return Json::UInt64(freq ? (t * 1000000ull / freq) : 0); };

    Json::Value result;
    Json::Value phases(Json::arrayValue);
    for (unsigned int p = 0; p < TickBench::PHASE_COUNT; p++)
    {
        auto &s = TickBench::gStat[p];
        uint64_t c = s.count.load(memory_order_relaxed);
        uint64_t sum = s.sumTsc.load(memory_order_relaxed);
        Json::Value e;
        e["phase"] = TickBench::kPhaseName[p];
        e["count"] = Json::UInt64(c);
        e["sumUs"] = toUs(sum);
        e["avgUs"] = toUs(c ? sum / c : 0);
        e["maxUs"] = toUs(s.maxTsc.load(memory_order_relaxed));
        e["lastUs"] = toUs(s.lastTsc.load(memory_order_relaxed));
        phases.append(e);
    }
    result["frequencyHz"] = Json::UInt64(freq);
    result["currentTick"] = system.tick;
    result["phases"] = phases;

    if (req.getParameter("reset") == "1" || req.getParameter("reset") == "true")
        TickBench::reset();

    return jsonResp(result);
}

RPC_ROUTE("GET", "/v1/tx-stats")
{
    using namespace std;
    Json::Value result;
    Json::Value data;
    data["totalReceived"] = Json::UInt64(TxStats::gTotalReceived.load(memory_order_relaxed));
    data["totalValid"] = Json::UInt64(TxStats::gTotalValid.load(memory_order_relaxed));
    data["totalStored"] = Json::UInt64(TxStats::gTotalStored.load(memory_order_relaxed));
    uint32_t last = TxStats::gLastTick.load(memory_order_relaxed);
    data["lastTick"] = Json::UInt(last);
    data["currentTick"] = system.tick;

    long long count = 20;
    if (req.getParameter("count") != "")
        count = std::stoll(req.getParameter("count"));
    if (count < 0) count = 0;
    if (count > (long long)TxStats::RING) count = TxStats::RING;

    Json::Value perTick(Json::arrayValue);
    for (long long i = count - 1; i >= 0; i--)
    {
        if ((long long)last - i < 0) continue;
        uint32_t t = (uint32_t)(last - i);
        TxStats::TickSlot &s = TxStats::gRing[t & TxStats::RING_MASK];
        if (s.tick.load(memory_order_relaxed) != t) continue;
        Json::Value e;
        e["tick"] = Json::UInt(t);
        e["received"] = Json::UInt(s.received.load(memory_order_relaxed));
        e["stored"] = Json::UInt(s.stored.load(memory_order_relaxed));
        perTick.append(e);
    }
    data["perTick"] = perTick;
    result["data"] = data;
    return jsonResp(result);
}

// Fork-rollback observability. Summary counters; the COMPLETE unforkable-tick record is on disk.
RPC_ROUTE("GET", "/v1/fork-stats")
{
    (void)req;
    RpcResp r;
    r.body = ForkStats::summaryJson();   // contentType defaults to application/json
    return r;
}

// Ant walker health: separates a walker chewing through work from one up but delivering nothing.
RPC_ROUTE("GET", "/v1/ant-walker")
{
    (void)req;
    RpcResp r;
    r.body = AntWalker::statsJson();
    return r;
}

// The full durable record of every unforkable tick (not a recent ring) — one line per skipped fork.
RPC_ROUTE("GET", "/v1/unforkable-ticks")
{
    (void)req;
    RpcResp r;
    r.contentType = "text/plain";
    r.body = ForkStats::readLogAll();
    return r;
}

RPC_ROUTE("GET", "/v1/issuers/:issuerIdentity/assets/:assetName/owners")
{
    std::string issuerIdentity = req.getParameter("issuerIdentity");
    std::string assetName = req.getParameter("assetName");
    Json::Value result;
    Json::Value ownersArray(Json::arrayValue);
    m256i issuerPublicKey;
    getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(issuerIdentity.c_str()), issuerPublicKey.m256i_u8);
    auto targetIssuanceIndex = issuanceIndex(issuerPublicKey, HttpUtils::assetNameFromString(assetName.c_str()));

    long long page = 0;
    long long pageSize = 10;
    long long currentIndex = 0;
    if (req.getParameter("page") != "")
        page = std::stoll(req.getParameter("page"));
    if (req.getParameter("pageSize") != "")
        pageSize = std::stoll(req.getParameter("pageSize"));

    for (unsigned long long i = 0; i < ASSETS_CAPACITY; i++)
    {
        if (assets[i].varStruct.ownership.type == OWNERSHIP)
        {
            auto &asset = assets[i].varStruct.ownership;
            unsigned int currentIssuanceIndex = asset.issuanceIndex;
            if (targetIssuanceIndex >= 0 && currentIssuanceIndex != targetIssuanceIndex)
                continue;
            if (currentIndex < (page + 1) * pageSize && currentIndex >= page * pageSize)
            {
                CHAR16 identity[61] = {};
                getIdentity((unsigned char *)&asset.publicKey, identity, false);
                std::string identityStr = wchar_to_string(identity);
                Json::Value ownerJson;
                ownerJson["identity"] = identityStr;
                ownerJson["numberOfShares"] = std::to_string(asset.numberOfShares);
                ownersArray.append(ownerJson);
            }
            currentIndex++;
        }
    }

    Json::Value pagination;
    pagination["totalRecords"] = Json::UInt64(currentIndex);
    pagination["currentPage"] = Json::UInt64(page);
    pagination["totalPages"] = Json::UInt64(std::ceil((float)currentIndex / pageSize));
    pagination["pageSize"] = Json::UInt64(pageSize);

    result["pagination"] = pagination;
    result["owners"] = ownersArray;
    result["tick"] = system.tick;
    return jsonResp(result);
}

RPC_ROUTE("GET", "/v1/latest-stats")
{
    (void)req;
    Json::Value result;
    Json::Value data;
    TickStorage::tickData.acquireLock();
    TickData *tickData = TickStorage::tickData.getByTickIfNotEmpty(system.tick - 1);
    if (tickData)
    {
        data["timestamp"] = HttpUtils::formatTimestamp(tickData->millisecond, tickData->second, tickData->minute,
            tickData->hour, tickData->day, tickData->month, tickData->year);
    } else
    {
        data["timestamp"] = "0";
    }
    TickStorage::tickData.releaseLock();

    data["circulatingSupply"] = std::to_string(spectrumInfo.totalAmount);
    data["activeAddresses"] = spectrumInfo.numberOfEntities;
    data["price"] = 0;
    data["marketCap"] = "0";
    data["epoch"] = system.epoch;
    data["currentTick"] = system.tick;
    data["ticksInCurrentEpoch"] = system.tick - system.initialTick;
    unsigned int emptyTicks = 0;
    for (unsigned int tick = system.initialTick; tick < system.tick; tick++)
    {
        PinScope _pinScope;
        TickStorage::tickData.acquireLock();
        const bool empty = (TickStorage::tickData.getByTickIfNotEmpty(tick) == nullptr);
        TickStorage::tickData.releaseLock();
        if (empty) emptyTicks++;
    }
    data["emptyTicksInCurrentEpoch"] = emptyTicks;
    data["epochTickQuality"] = system.tick - system.initialTick == 0 ? 0 : std::roundf((float)(system.tick - system.initialTick - emptyTicks) / (float)(system.tick - system.initialTick) * 100000.0f) / 100000.0f;
    data["burnedQus"] = "0";
    result["data"] = data;
    return jsonResp(result);
}

RPC_ROUTE("GET", "/v1/rich-list")
{
    Json::Value result;
    Json::Value richListArray(Json::arrayValue);

    long long page = 0;
    long long pageSize = 10;
    if (req.getParameter("page") != "")
        page = std::stoll(req.getParameter("page"));
    if (req.getParameter("pageSize") != "")
        pageSize = std::stoll(req.getParameter("pageSize"));

    std::vector<std::pair<m256i, long long>> balances;
    for (unsigned int i = 0; i < SPECTRUM_CAPACITY; i++)
    {
        const long long balance = spectrum[i].incomingAmount - spectrum[i].outgoingAmount;
        if (balance > 0)
            balances.emplace_back(spectrum[i].publicKey, balance);
    }

    std::sort(balances.begin(), balances.end(), [](const std::pair<m256i, long long> &a, const std::pair<m256i, long long> &b)
              { return a.second > b.second; });

    long long start = page * pageSize;
    long long end = std::min(start + pageSize, (long long)balances.size());
    for (long long i = start; i < end; i++)
    {
        Json::Value entry;
        CHAR16 identity[61] = {};
        getIdentity((unsigned char *)&balances[i].first, identity, false);
        std::string identityStr = wchar_to_string(identity);
        entry["identity"] = identityStr;
        entry["balance"] = std::to_string(balances[i].second);
        richListArray.append(entry);
    }

    Json::Value pagination;
    pagination["totalRecords"] = Json::UInt64(balances.size());
    pagination["currentPage"] = Json::UInt64(page);
    pagination["totalPages"] = Json::UInt64(std::ceil((float)balances.size() / pageSize));
    pagination["pageSize"] = Json::UInt64(pageSize);
    result["pagination"] = pagination;
    result["richList"]["entities"] = richListArray;
    result["epoch"] = system.epoch;
    return jsonResp(result);
}

#endif // __linux__ || LITE_WASM_SC
