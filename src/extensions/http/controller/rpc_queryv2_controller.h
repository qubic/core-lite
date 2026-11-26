#pragma once

// Query-v2 endpoints as RpcReq->RpcResp router handlers.

#if defined(__linux__) || defined(LITE_WASM_SC)

#include <optional>
#include <deque>
#include <algorithm>
#include <vector>
#include <string>
#include <fmt/format.h>
#include "extensions/rpc/rpc_core.h"
#include "extensions/http/queryv2_helpers.h"

namespace QV2 = RpcQueryV2;
static constexpr int QV2_BAD = 3;   // RpcQueryV2::StatusCode::BadRequest
static constexpr int QV2_NF  = 5;   // RpcQueryV2::StatusCode::NotFound

// Tick number bounds check; returns false + fills errOut on failure.
static bool rpcTickNumberVerify(const RpcReq& req, RpcResp& errOut)
{
    Json::Value result;
    auto json = rpcJsonBody(req.body);
    if (!json)
    {
        result["code"] = QV2_BAD;
        result["message"] = "Invalid JSON";
        errOut = jsonResp(result, 400);
        return false;
    }
    if (!(*json).isMember("tickNumber"))
    {
        result["code"] = QV2_BAD;
        result["message"] = "Missing tickNumber field";
        errOut = jsonResp(result, 400);
        return false;
    }
    unsigned int tickNumber = (*json)["tickNumber"].asUInt64();
    if (tickNumber > system.tick)
    {
        result["code"] = QV2_BAD;
        result["message"] = fmt::format("invalid tick number: rpc error: code = FailedPrecondition desc = requested tick number {} is greater than last processed tick {}", tickNumber, system.tick);
        errOut = jsonResp(result, 400);
        return false;
    }
    else if (tickNumber < system.initialTick)
    {
        result["code"] = QV2_BAD;
        result["message"] = fmt::format("invalid tick number: rpc error: code = OutOfRange desc = provided tick number {} was skipped by the system, next available tick is {}", tickNumber, system.initialTick);
        errOut = jsonResp(result, 400);
        return false;
    }
    return true;
}

RPC_ROUTE("POST", "/query/v1/getComputorListsForEpoch")
{
    Json::Value result;
    auto json = rpcJsonBody(req.body);
    if (!json)
    {
        result["code"] = -1;
        result["message"] = "Invalid JSON";
        return jsonResp(result, 400);
    }
    if (!(*json).isMember("epoch"))
    {
        result["code"] = -1;
        result["message"] = "Missing epoch field";
        return jsonResp(result, 400);
    }

    unsigned int epoch = (*json)["epoch"].asUInt64();
    Json::Value computorLists(Json::arrayValue);
    Json::Value computorObject;
    Json::Value idLists(Json::arrayValue);
    for (unsigned int i = 0; i < NUMBER_OF_COMPUTORS; i++)
    {
        m256i &pubKey = broadcastedComputors.computors.publicKeys[i];
        CHAR16 id[61] = {};
        getIdentity((const unsigned char *)&pubKey, id, false);
        idLists.append(wchar_to_string(id));
    }
    computorObject["epoch"] = epoch;
    computorObject["tickNumber"] = 0;
    computorObject["identities"] = idLists;
    computorObject["signature"] = base64_encode(broadcastedComputors.computors.signature, SIGNATURE_SIZE);
    computorLists.append(computorObject);
    result["computorsLists"] = computorLists;
    return jsonResp(result);
}

RPC_ROUTE("GET", "/query/v1/getLastProcessedTick")
{
    (void)req;
    Json::Value result;
    result["tickNumber"] = system.tick;
    result["epoch"] = system.epoch;
    result["intervalInitialTick"] = system.initialTick;
    return jsonResp(result);
}

RPC_ROUTE("GET", "/query/v1/getProcessedTickIntervals")
{
    (void)req;
    Json::Value result(Json::arrayValue);
    Json::Value tickInterval;
    tickInterval["epoch"] = system.epoch;
    tickInterval["firstTick"] = system.initialTick;
    tickInterval["lastTick"] = system.tick;
    result.append(tickInterval);
    return jsonResp(result);
}

RPC_ROUTE("POST", "/query/v1/getTickData")
{
    RpcResp err;
    if (!rpcTickNumberVerify(req, err)) return err;

    Json::Value result;
    auto json = rpcJsonBody(req.body);
    unsigned int tickNumber = (*json)["tickNumber"].asUInt64();
    TickData localTickData;
    TickStorage::tickData.acquireLock();
    TickData *tickData = TickStorage::tickData.getByTickIfNotEmpty(tickNumber);
    if (tickData)
        copyMem(&localTickData, tickData, sizeof(TickData));
    TickStorage::tickData.releaseLock();
    if (!tickData)
    {
        result["code"] = QV2_NF;
        result["message"] = "Tick data not found";
        return jsonResp(result, 404);
    }

    Json::Value jsonObject;
    jsonObject["tickNumber"] = localTickData.tick;
    jsonObject["epoch"] = localTickData.epoch;
    jsonObject["computorIndex"] = localTickData.computorIndex;
    jsonObject["timeLock"] = base64_encode(localTickData.timelock.m256i_u8, 32);
    jsonObject["timestamp"] = HttpUtils::formatTimestamp(localTickData.millisecond, localTickData.second, localTickData.minute,
        localTickData.hour, localTickData.day, localTickData.month, localTickData.year);
    jsonObject["varStruct"] = "";
    Json::Value txDigestsJson(Json::arrayValue);
    for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
    {
        if (localTickData.transactionDigests[i] != m256i::zero())
        {
            CHAR16 id[61] = {};
            getIdentity((unsigned char *)&localTickData.transactionDigests[i], id, true);
            txDigestsJson.append(wchar_to_string(id));
        }
    }
    jsonObject["transactionHashes"] = txDigestsJson;
    Json::Value contractFeesJson(Json::arrayValue);
    // Zero fees are dropped, as go-archiver's contractFeesToProto() does.
    for (unsigned int i = 0; i < MAX_NUMBER_OF_CONTRACTS; i++)
        if (localTickData.contractFees[i] != 0)
            contractFeesJson.append(std::to_string(localTickData.contractFees[i]));
    jsonObject["contractFees"] = contractFeesJson;
    jsonObject["signature"] = base64_encode(localTickData.signature, SIGNATURE_SIZE);
    return jsonResp(jsonObject);
}

RPC_ROUTE("POST", "/query/v1/getTransactionByHash")
{
    Json::Value result;
    auto json = rpcJsonBody(req.body);
    if (!json)
    {
        result["code"] = -1;
        result["message"] = "Invalid JSON";
        return jsonResp(result, 400);
    }
    if (!(*json).isMember("hash"))
    {
        result["code"] = -1;
        result["message"] = "Missing hash field";
        return jsonResp(result, 400);
    }

    std::string txHash = (*json)["hash"].asString();
    if (txHash.length() != 60)
    {
        result["code"] = QV2_BAD;
        result["message"] = fmt::format("invalid id format: converting id to pubkey: invalid ID length, expected 60, found {}", txHash.length());
        return jsonResp(result, 400);
    }
    std::transform(txHash.begin(), txHash.end(), txHash.begin(), ::toupper);
    m256i txDigest = {};
    if (!getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(txHash.c_str()), txDigest.m256i_u8))
    {
        result["code"] = QV2_BAD;
        result["message"] = fmt::format("invalid id format: invalid hash [{}]", txHash);
        return jsonResp(result, 400);
    }

    unsigned int foundTick = 0;
    unsigned int foundSlot = 0;
    bool found = false;
    TickData localTickData;
    const bool hasTickHint = (*json).isMember("tickNumber");
    const unsigned int scanLo = hasTickHint ? (*json)["tickNumber"].asUInt() : system.initialTick;
    const unsigned int scanHi = hasTickHint ? scanLo : system.tick;
    // Prior-epoch ticks index the current-epoch offsets out of range; far-future ones pin a frontier page for the epoch.
    if (hasTickHint && (scanLo < system.initialTick || scanLo > system.tick + 3))
    {
        result["code"] = QV2_NF;
        result["message"] = "Transaction not found";
        return jsonResp(result, 404);
    }
    for (unsigned int tick = scanLo; tick <= scanHi && !found; tick++)
    {
        PinScope _pinScope;
        TickStorage::tickData.acquireLock();
        TickData *tickData = TickStorage::tickData.getByTickIfNotEmpty(tick);
        if (tickData)
            copyMem(&localTickData, tickData, sizeof(TickData));
        TickStorage::tickData.releaseLock();
        if (!tickData)
            continue;
        for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
        {
            if (localTickData.transactionDigests[i] == txDigest)
            {
                foundTick = tick;
                foundSlot = i;
                found = true;
                break;
            }
        }
    }
    if (!found)
    {
        result["code"] = QV2_NF;
        result["message"] = "Transaction not found";
        return jsonResp(result, 404);
    }

    ts.tickTransactions.acquireLock();
    unsigned long long txOffset = ts.tickTransactionOffsets(foundTick, foundSlot);
    if (!txOffset)
    {
        ts.tickTransactions.releaseLock();
        result["code"] = QV2_NF;
        result["message"] = "Transaction not found";
        return jsonResp(result, 404);
    }
    Transaction *txPtr = ts.tickTransactions(txOffset);
    const unsigned int txTotalSize = txPtr->totalSize();
    std::vector<unsigned char> txBuf(txTotalSize);
    copyMem(txBuf.data(), txPtr, txTotalSize);
    ts.tickTransactions.releaseLock();

    Json::Value jsonObject = HttpUtils::transactionToJson(reinterpret_cast<Transaction *>(txBuf.data()));
    return jsonResp(jsonObject);
}

RPC_ROUTE("POST", "/query/v1/getTransactionsForIdentity")
{
    try
    {
        Json::Value result;
        auto json = rpcJsonBody(req.body);
        if (!json)
        {
            result["code"] = QV2_BAD;
            result["message"] = "Invalid JSON";
            return jsonResp(result, 400);
        }
        if (!(*json).isMember("identity"))
        {
            result["code"] = QV2_BAD;
            result["message"] = "Missing identity field";
            return jsonResp(result, 400);
        }

        auto filters = (*json)["filters"];
        auto ranges = (*json)["ranges"];
        auto pagination = (*json)["pagination"];

        std::string identityStr = (*json)["identity"].asString();
        m256i publicKey{};
        if (!getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(identityStr.c_str()), publicKey.m256i_u8))
        {
            result["code"] = QV2_BAD;
            result["message"] = fmt::format("invalid id format: invalid identity [{}]", identityStr);
            return jsonResp(result, 400);
        }

        Json::Value transactions(Json::arrayValue);
        std::vector<std::vector<unsigned char>> matchedBufs;
        for (unsigned int tick = system.initialTick; tick <= system.tick; tick++)
        {
            PinScope _pinScope;
            TickData localTickData;
            TickStorage::tickData.acquireLock();
            TickData *tickData = TickStorage::tickData.getByTickIfNotEmpty(tick);
            if (tickData)
                copyMem(&localTickData, tickData, sizeof(TickData));
            TickStorage::tickData.releaseLock();
            if (!tickData)
                continue;

            ts.tickTransactions.acquireLock();
            unsigned long long *offsets = ts.tickTransactionOffsets.getByTickInCurrentEpoch(tick);
            for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
            {
                if (isZero(localTickData.transactionDigests[i]) || !offsets[i])
                    continue;
                Transaction *txPtr = ts.tickTransactions(offsets[i]);
                if (txPtr->sourcePublicKey == publicKey)
                {
                    const unsigned int txTotalSize = txPtr->totalSize();
                    matchedBufs.emplace_back(txTotalSize);
                    copyMem(matchedBufs.back().data(), txPtr, txTotalSize);
                }
            }
            ts.tickTransactions.releaseLock();
        }
        for (auto &buf : matchedBufs)
            transactions.append(HttpUtils::transactionToJson(reinterpret_cast<Transaction *>(buf.data())));

        if (filters.isObject())
        {
            Json::Value filteredTransactions(Json::arrayValue);
            for (unsigned int i = 0; i < transactions.size(); i++)
            {
                Json::Value tx = transactions[i];
                bool match = true;
                for (const auto &key : filters.getMemberNames())
                {
                    if (tx.isMember(key))
                    {
                        if (tx[key].asString() != filters[key].asString())
                        {
                            match = false;
                            break;
                        }
                    }
                }
                if (match)
                    filteredTransactions.append(tx);
            }
            transactions = filteredTransactions;
        }
        if (ranges.isObject())
        {
            Json::Value rangedTransactions(Json::arrayValue);
            for (unsigned int i = 0; i < transactions.size(); i++)
            {
                Json::Value tx = transactions[i];
                bool match = true;
                for (const auto &key : ranges.getMemberNames())
                {
                    if (tx.isMember(key))
                    {
                        Json::Value range = ranges[key];
                        if (range.isObject())
                        {
                            if (range.isMember("lt"))
                            {
                                if (!(std::stoull(tx[key].asString()) < std::stoull(range["lt"].asString())))
                                {
                                    match = false;
                                    break;
                                }
                            }
                            if (range.isMember("gt"))
                            {
                                if (!(std::stoull(tx[key].asString()) > std::stoull(range["gt"].asString())))
                                {
                                    match = false;
                                    break;
                                }
                            }
                            if (range.isMember("lte"))
                            {
                                if (!(std::stoull(tx[key].asString()) <= std::stoull(range["lte"].asString())))
                                {
                                    match = false;
                                    break;
                                }
                            }
                            if (range.isMember("gte"))
                            {
                                if (!(std::stoull(tx[key].asString()) >= std::stoull(range["gte"].asString())))
                                {
                                    match = false;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (match)
                    rangedTransactions.append(tx);
            }
            transactions = rangedTransactions;
        }
        if (pagination.isObject())
        {
            unsigned int offset = 0;
            unsigned int size = 0;
            if (pagination.isMember("offset"))
                offset = pagination["offset"].asUInt64();
            offset = std::min(offset, (unsigned int)10000);
            if (pagination.isMember("size"))
                size = pagination["size"].asUInt64();
            else
                size = 10;
            size = std::min(size, (unsigned int)1000);
            Json::Value paginatedTransactions(Json::arrayValue);
            for (unsigned int i = offset; i < transactions.size() && i < offset + size; i++)
                paginatedTransactions.append(transactions[i]);
            transactions = paginatedTransactions;
        }

        result["transactions"] = transactions;
        result["validForTick"] = 0;
        result["hits"]["total"] = transactions.size();
        result["hits"]["from"] = 0;
        result["hits"]["size"] = transactions.size();
        return jsonResp(result);
    }
    catch (const std::exception &e)
    {
        Json::Value result;
        result["code"] = -1;
        result["message"] = std::string("Internal server error: ") + e.what();
        return jsonResp(result, 500);
    }
}

RPC_ROUTE("POST", "/query/v1/getTransfersForIdentity")
{
    Json::Value result;
    auto json = rpcJsonBody(req.body);
    if (!json || !(*json).isMember("identity"))
    {
        result["code"] = QV2_BAD;
        result["message"] = "Missing identity field";
        return jsonResp(result, 400);
    }

    std::string identityStr = (*json)["identity"].asString();
    m256i publicKey{};
    if (!getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(identityStr.c_str()), publicKey.m256i_u8))
    {
        result["code"] = QV2_BAD;
        result["message"] = fmt::format("invalid identity [{}]", identityStr);
        return jsonResp(result, 400);
    }

    std::string direction = (*json).get("direction", "both").asString();
    const bool wantIn  = (direction == "in"  || direction == "both");
    const bool wantOut = (direction == "out" || direction == "both");
    const unsigned int limit = std::min((*json).get("limit", 50).asUInt(), 1000u);

    Json::Value transactions(Json::arrayValue);
    struct Hit { std::vector<unsigned char> buf; const char* dir; };
    std::vector<Hit> hits;
    hits.reserve(limit);

    for (unsigned int tick = system.tick; tick + 1 > system.initialTick && hits.size() < limit; tick--)
    {
        PinScope _pinScope;
        TickData localTickData;
        TickStorage::tickData.acquireLock();
        TickData *tickData = TickStorage::tickData.getByTickIfNotEmpty(tick);
        if (tickData) copyMem(&localTickData, tickData, sizeof(TickData));
        TickStorage::tickData.releaseLock();
        if (!tickData)
        {
            if (tick == 0) break;
            continue;
        }

        ts.tickTransactions.acquireLock();
        unsigned long long *offsets = ts.tickTransactionOffsets.getByTickInCurrentEpoch(tick);
        for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK && hits.size() < limit; i++)
        {
            if (isZero(localTickData.transactionDigests[i]) || !offsets[i]) continue;
            Transaction *txPtr = ts.tickTransactions(offsets[i]);
            const bool isOut = (txPtr->sourcePublicKey      == publicKey);
            const bool isIn  = (txPtr->destinationPublicKey == publicKey);
            const char* dir = nullptr;
            if (isOut && wantOut)      dir = "out";
            else if (isIn && wantIn)   dir = "in";
            if (dir)
            {
                const unsigned int sz = txPtr->totalSize();
                Hit h{ std::vector<unsigned char>(sz), dir };
                copyMem(h.buf.data(), txPtr, sz);
                hits.push_back(std::move(h));
            }
        }
        ts.tickTransactions.releaseLock();
        if (tick == 0) break;
    }

    for (auto &h : hits)
    {
        Json::Value txJson = HttpUtils::transactionToJson(reinterpret_cast<Transaction *>(h.buf.data()));
        txJson["direction"] = h.dir;
        transactions.append(txJson);
    }
    result["identity"] = identityStr;
    result["count"]    = (unsigned int)hits.size();
    result["transactions"] = transactions;
    return jsonResp(result);
}

RPC_ROUTE("POST", "/query/v1/getContractCalls")
{
    Json::Value result;
    auto json = rpcJsonBody(req.body);
    if (!json)
    {
        result["code"] = QV2_BAD;
        result["message"] = "Invalid JSON";
        return jsonResp(result, 400);
    }

    unsigned int toTick = (*json).get("toTick", system.tick).asUInt();
    unsigned int fromTick = (*json).get("fromTick", 0).asUInt();
    const bool hasFilter = (*json).isMember("contractIndex");
    const unsigned int filterIdx = hasFilter ? (*json)["contractIndex"].asUInt() : 0;
    const unsigned int page = (*json).get("page", 0).asUInt();
    const unsigned int pageSize = std::min((unsigned int)200, (*json).get("pageSize", 50).asUInt());

    if (hasFilter && (filterIdx < 1 || filterIdx >= contractCount))
    {
        result["code"] = QV2_BAD;
        result["message"] = "contractIndex out of range";
        return jsonResp(result, 400);
    }

    if (toTick > system.tick) toTick = system.tick;
    if (fromTick < system.initialTick) fromTick = system.initialTick;
    if (fromTick > toTick) {
        result["fromTick"] = fromTick;
        result["toTick"] = toTick;
        result["total"] = 0;
        result["page"] = page;
        result["pageSize"] = pageSize;
        result["transactions"] = Json::Value(Json::arrayValue);
        return jsonResp(result);
    }
    if (toTick - fromTick + 1 > 1000)
        fromTick = toTick - 1000 + 1;

    struct Hit { std::vector<unsigned char> buf; unsigned int idx; };
    std::vector<Hit> hits;
    hits.reserve(256);

    for (unsigned int tick = toTick; tick + 1 > fromTick; tick--)
    {
        PinScope _pinScope;
        TickData localTickData;
        TickStorage::tickData.acquireLock();
        TickData *tickData = TickStorage::tickData.getByTickIfNotEmpty(tick);
        if (tickData) copyMem(&localTickData, tickData, sizeof(TickData));
        TickStorage::tickData.releaseLock();
        if (!tickData) {
            if (tick == 0) break;
            continue;
        }

        ts.tickTransactions.acquireLock();
        unsigned long long *offsets = ts.tickTransactionOffsets.getByTickInCurrentEpoch(tick);
        for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
        {
            if (isZero(localTickData.transactionDigests[i]) || !offsets[i]) continue;
            Transaction *txPtr = ts.tickTransactions(offsets[i]);
            const m256i &dest = txPtr->destinationPublicKey;
            if (dest.m256i_u64[1] != 0 || dest.m256i_u64[2] != 0 || dest.m256i_u64[3] != 0) continue;
            const unsigned long long idx64 = dest.m256i_u64[0];
            if (idx64 < 1 || idx64 >= contractCount) continue;
            const unsigned int idx = (unsigned int)idx64;
            if (hasFilter && idx != filterIdx) continue;
            const unsigned int sz = txPtr->totalSize();
            Hit h{ std::vector<unsigned char>(sz), idx };
            copyMem(h.buf.data(), txPtr, sz);
            hits.push_back(std::move(h));
        }
        ts.tickTransactions.releaseLock();
        if (tick == 0) break;
    }

    const unsigned int total = (unsigned int)hits.size();
    const unsigned int start = page * pageSize;
    const unsigned int end = std::min(total, start + pageSize);

    Json::Value transactions(Json::arrayValue);
    for (unsigned int k = start; k < end; k++)
    {
        Json::Value txJson = HttpUtils::transactionToJson(reinterpret_cast<Transaction *>(hits[k].buf.data()));
        txJson["contractIndex"] = hits[k].idx;
        transactions.append(txJson);
    }

    result["fromTick"] = fromTick;
    result["toTick"] = toTick;
    result["total"] = total;
    result["page"] = page;
    result["pageSize"] = pageSize;
    result["transactions"] = transactions;
    return jsonResp(result);
}

RPC_ROUTE("POST", "/query/v1/getVotesForTick")
{
    Json::Value result;
    auto json = rpcJsonBody(req.body);
    if (!json || !(*json).isMember("tickNumber"))
    {
        result["code"] = QV2_BAD;
        result["message"] = "Missing tickNumber";
        return jsonResp(result, 400);
    }
    unsigned int tickNumber = (*json)["tickNumber"].asUInt();
    if (tickNumber < system.initialTick || tickNumber > system.tick)
    {
        result["code"] = QV2_NF;
        result["message"] = fmt::format("tick {} out of epoch range [{}, {}]", tickNumber, system.initialTick, system.tick);
        return jsonResp(result, 404);
    }

    Json::Value votes(Json::arrayValue);

    Tick localCopy[NUMBER_OF_COMPUTORS];
    for (unsigned int i = 0; i < NUMBER_OF_COMPUTORS; i++)
    {
        ts.ticks.acquireLock(i);
        const Tick *src = TickStorage::ticks.getByTickInCurrentEpoch(tickNumber) + i;
        copyMem(&localCopy[i], src, sizeof(Tick));
        ts.ticks.releaseLock(i);
    }

    unsigned int count = 0;
    for (unsigned int i = 0; i < NUMBER_OF_COMPUTORS; i++)
    {
        Tick &t = localCopy[i];
        if (t.epoch != system.epoch) continue;
        count++;
        Json::Value v;
        v["computorIndex"] = t.computorIndex;
        v["epoch"] = t.epoch;
        v["tick"] = t.tick;
        v["timestamp"] = HttpUtils::formatTimestamp(t.millisecond, t.second, t.minute, t.hour, t.day, t.month, t.year);
        v["prevSpectrumDigest"]       = base64_encode(t.prevSpectrumDigest.m256i_u8, 32);
        v["saltedSpectrumDigest"]     = base64_encode(t.saltedSpectrumDigest.m256i_u8, 32);
        v["prevUniverseDigest"]       = base64_encode(t.prevUniverseDigest.m256i_u8, 32);
        v["saltedUniverseDigest"]     = base64_encode(t.saltedUniverseDigest.m256i_u8, 32);
        v["prevComputerDigest"]       = base64_encode(t.prevComputerDigest.m256i_u8, 32);
        v["saltedComputerDigest"]     = base64_encode(t.saltedComputerDigest.m256i_u8, 32);
        v["transactionDigest"]        = base64_encode(t.transactionDigest.m256i_u8, 32);
        v["expectedNextTickTransactionDigest"] = base64_encode(t.expectedNextTickTransactionDigest.m256i_u8, 32);
        v["prevResourceTestingDigest"]   = t.prevResourceTestingDigest;
        v["saltedResourceTestingDigest"] = t.saltedResourceTestingDigest;
        v["prevTransactionBodyDigest"]   = t.prevTransactionBodyDigest;
        v["saltedTransactionBodyDigest"] = t.saltedTransactionBodyDigest;
        v["signature"] = base64_encode(t.signature, SIGNATURE_SIZE);
        votes.append(v);
    }

    result["tickNumber"] = tickNumber;
    result["count"]      = count;
    result["votes"]      = votes;
    return jsonResp(result);
}

RPC_ROUTE("GET", "/query/v1/getContracts")
{
    (void)req;
    Json::Value result;
    Json::Value arr(Json::arrayValue);
    for (unsigned int i = 1; i < contractCount; i++)
    {
        const auto &cd = contractDescriptions[i];
        char name[8] = {0};
        for (int k = 0; k < 7 && cd.assetName[k]; k++) name[k] = cd.assetName[k];
        Json::Value c;
        c["index"] = i;
        c["name"] = std::string(name);
        c["constructionEpoch"] = cd.constructionEpoch;
        c["destructionEpoch"] = cd.destructionEpoch;
        c["stateSize"] = (Json::UInt64)cd.stateSize;
        arr.append(c);
    }
    result["contracts"] = arr;
    result["count"] = contractCount - 1;
    return jsonResp(result);
}

RPC_ROUTE("POST", "/query/v1/getTransactionsForTick")
{
    RpcResp err;
    if (!rpcTickNumberVerify(req, err)) return err;

    Json::Value result;
    auto json = rpcJsonBody(req.body);
    unsigned int tickNumber = (*json)["tickNumber"].asUInt64();
    TickData localTickData;
    TickStorage::tickData.acquireLock();
    TickData *tickData = TickStorage::tickData.getByTickIfNotEmpty(tickNumber);
    if (tickData)
        copyMem(&localTickData, tickData, sizeof(TickData));
    TickStorage::tickData.releaseLock();
    if (!tickData)
    {
        result["code"] = QV2_NF;
        result["message"] = "Tick data not found";
        return jsonResp(result, 404);
    }

    Json::Value transactions(Json::arrayValue);
    ts.tickTransactions.acquireLock();
    unsigned long long *offsets = ts.tickTransactionOffsets.getByTickInCurrentEpoch(tickNumber);
    std::vector<std::vector<unsigned char>> txBufs;
    txBufs.reserve(NUMBER_OF_TRANSACTIONS_PER_TICK);
    for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
    {
        if (isZero(localTickData.transactionDigests[i]) || !offsets[i])
            continue;
        Transaction *txPtr = ts.tickTransactions(offsets[i]);
        const unsigned int txTotalSize = txPtr->totalSize();
        txBufs.emplace_back(txTotalSize);
        copyMem(txBufs.back().data(), txPtr, txTotalSize);
    }
    ts.tickTransactions.releaseLock();

    for (auto &buf : txBufs)
        transactions.append(HttpUtils::transactionToJson(reinterpret_cast<Transaction *>(buf.data())));
    result["transactions"] = transactions;
    return jsonResp(result);
}

RPC_ROUTE("POST", "/query/v1/getEventLogs")
{
#if !ENABLED_LOGGING
    (void)req;
    Json::Value result;
    result["code"] = -1;
    result["message"] = "event logs disabled in this build";
    return jsonResp(result, 503);
#else
    if (qLogger::isDisabled)
    {
        Json::Value result;
        result["code"] = -1;
        result["message"] = "event logs disabled on this node";
        return jsonResp(result, 503);
    }
    try
    {
        Json::Value result;
        auto json = rpcJsonBody(req.body);
        if (!json && !req.body.empty())
        {
            result["code"] = QV2_BAD;
            result["message"] = "Invalid JSON";
            return jsonResp(result, 400);
        }
        Json::Value emptyObj(Json::objectValue);
        Json::Value filters = (json && (*json).isMember("filters")) ? (*json)["filters"] : emptyObj;
        Json::Value exclude = (json && (*json).isMember("exclude")) ? (*json)["exclude"] : emptyObj;
        Json::Value should = (json && (*json).isMember("should")) ? (*json)["should"] : Json::Value(Json::arrayValue);
        Json::Value ranges = (json && (*json).isMember("ranges")) ? (*json)["ranges"] : emptyObj;
        Json::Value pagination = (json && (*json).isMember("pagination")) ? (*json)["pagination"] : emptyObj;

        unsigned int offset = 0;
        unsigned int size = 10;
        if (pagination.isObject())
        {
            if (pagination.isMember("offset")) offset = pagination["offset"].asUInt();
            if (pagination.isMember("size"))
            {
                unsigned int s = pagination["size"].asUInt();
                if (s != 0) size = s;
            }
        }
        if (offset > 10000)
        {
            result["code"] = QV2_BAD;
            result["message"] = "pagination.offset must be <= 10000";
            return jsonResp(result, 400);
        }
        if (size > 1000) size = 1000;

        bool descOrder = false;
        if (json && (*json).isMember("order"))
        {
            std::string ord = (*json)["order"].asString();
            descOrder = (ord == "desc" || ord == "DESC");
        }

        const unsigned int validForTick = system.tick;

        auto epochInScope = [&]() -> bool
        {
            if (filters.isObject() && filters.isMember("epoch"))
            {
                try { if (std::stoul(filters["epoch"].asString()) != system.epoch) return false; }
                catch (...) { return false; }
            }
            if (ranges.isObject() && ranges.isMember("epoch"))
            {
                if (!QV2::rangeMatches(std::to_string(system.epoch), ranges["epoch"])) return false;
            }
            return true;
        };

        auto emitEmpty = [&]() -> RpcResp
        {
            Json::Value hits;
            hits["total"] = 0u;
            hits["from"] = offset;
            hits["size"] = 0u;
            result["hits"] = hits;
            result["eventLogs"] = Json::Value(Json::arrayValue);
            result["validForTick"] = validForTick;
            return jsonResp(result);
        };

        if (!epochInScope())
            return emitEmpty();

        unsigned long long tickLo = system.initialTick;
        unsigned long long tickHi = system.tick;
        if (qLogger::lastUpdatedTick != 0 && qLogger::lastUpdatedTick < tickHi)
            tickHi = qLogger::lastUpdatedTick;
        if (filters.isObject() && filters.isMember("tickNumber"))
        {
            try
            {
                unsigned long long t = std::stoull(filters["tickNumber"].asString());
                tickLo = tickHi = t;
            }
            catch (...) { return emitEmpty(); }
        }
        if (ranges.isObject() && ranges.isMember("tickNumber"))
        {
            const Json::Value &r = ranges["tickNumber"];
            try
            {
                if (r.isMember("gt")) tickLo = std::max(tickLo, std::stoull(r["gt"].asString()) + 1);
                if (r.isMember("gte")) tickLo = std::max(tickLo, std::stoull(r["gte"].asString()));
                if (r.isMember("lt") && std::stoull(r["lt"].asString()) > 0)
                    tickHi = std::min(tickHi, std::stoull(r["lt"].asString()) - 1);
                if (r.isMember("lte")) tickHi = std::min(tickHi, std::stoull(r["lte"].asString()));
            }
            catch (...) { return emitEmpty(); }
        }

        unsigned long long logIdLo = 0;
        unsigned long long logIdHi = qLogger::logId;
        if (filters.isObject() && filters.isMember("logId"))
        {
            try
            {
                unsigned long long l = std::stoull(filters["logId"].asString());
                logIdLo = logIdHi = l;
            }
            catch (...) { return emitEmpty(); }
        }
        if (ranges.isObject() && ranges.isMember("logId"))
        {
            const Json::Value &r = ranges["logId"];
            try
            {
                if (r.isMember("gt")) logIdLo = std::max(logIdLo, std::stoull(r["gt"].asString()) + 1);
                if (r.isMember("gte")) logIdLo = std::max(logIdLo, std::stoull(r["gte"].asString()));
                if (r.isMember("lt") && std::stoull(r["lt"].asString()) > 0)
                    logIdHi = std::min(logIdHi, std::stoull(r["lt"].asString()) - 1);
                if (r.isMember("lte")) logIdHi = std::min(logIdHi, std::stoull(r["lte"].asString()));
            }
            catch (...) { return emitEmpty(); }
        }

        auto tickSpan = [](unsigned int tick, unsigned long long &lo, unsigned long long &hi) -> bool
        {
            qLogger::TickBlobInfo tbi;
            qLogger::tx.getTickLogIdInfo(&tbi, tick);
            bool any = false;
            unsigned long long mn = ~0ULL, mx = 0;
            for (int i = 0; i < LOG_TX_PER_TICK; i++)
            {
                if (tbi.fromLogId[i] < 0 || tbi.length[i] <= 0) continue;
                any = true;
                unsigned long long f = (unsigned long long)tbi.fromLogId[i];
                unsigned long long l = (unsigned long long)(tbi.fromLogId[i] + tbi.length[i] - 1);
                if (f < mn) mn = f;
                if (l > mx) mx = l;
            }
            if (any)
            {
                lo = mn;
                hi = mx;
            }
            return any;
        };

        {
            unsigned long long slo = 0, shi = 0;
            if (tickSpan((unsigned int)tickLo, slo, shi)) logIdLo = std::max(logIdLo, slo);
            if (tickSpan((unsigned int)tickHi, slo, shi)) logIdHi = std::min(logIdHi, shi);
        }

        bool useHashAnchor = false;
        unsigned int hashAnchorTick = 0;
        unsigned int hashAnchorTxId = 0;
        if (filters.isObject() && filters.isMember("transactionHash"))
        {
            if (!filters.isMember("tickNumber"))
            {
                result["code"] = QV2_BAD;
                result["message"] = "tickNumber filter is required when filtering by transactionHash";
                return jsonResp(result, 400);
            }
            std::string txHash = filters["transactionHash"].asString();
            if (txHash.length() != 60)
            {
                result["code"] = QV2_BAD;
                result["message"] = fmt::format("invalid id format: converting hash to digest: invalid hash length, expected 60, found {}", txHash.length());
                return jsonResp(result, 400);
            }
            std::transform(txHash.begin(), txHash.end(), txHash.begin(), ::toupper);
            m256i txDigest = {};
            if (!getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(txHash.c_str()), txDigest.m256i_u8))
            {
                result["code"] = QV2_BAD;
                result["message"] = fmt::format("invalid id format: invalid hash [{}]", txHash);
                return jsonResp(result, 400);
            }
            bool found = false;
            TickData scanLocal;
            for (unsigned long long t = tickLo; t <= tickHi && !found; t++)
            {
                TickStorage::tickData.acquireLock();
                TickData *td = TickStorage::tickData.getByTickIfNotEmpty((unsigned int)t);
                if (td) copyMem(&scanLocal, td, sizeof(TickData));
                TickStorage::tickData.releaseLock();
                if (!td) continue;
                for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
                {
                    if (scanLocal.transactionDigests[i] == txDigest)
                    {
                        hashAnchorTick = (unsigned int)t;
                        hashAnchorTxId = i;
                        found = true;
                        break;
                    }
                }
            }
            if (!found)
                return emitEmpty();
            useHashAnchor = true;
            tickLo = tickHi = hashAnchorTick;
            qLogger::TickBlobInfo tbi;
            qLogger::tx.getTickLogIdInfo(&tbi, hashAnchorTick);
            if (hashAnchorTxId < LOG_TX_PER_TICK && tbi.fromLogId[hashAnchorTxId] >= 0 && tbi.length[hashAnchorTxId] > 0)
            {
                unsigned long long f = (unsigned long long)tbi.fromLogId[hashAnchorTxId];
                logIdLo = std::max(logIdLo, f);
                logIdHi = std::min(logIdHi, f + (unsigned long long)tbi.length[hashAnchorTxId] - 1);
            }
        }

        if (tickLo > tickHi)
            return emitEmpty();

        if (qLogger::logId == 0) return emitEmpty();
        logIdHi = std::min(logIdHi, qLogger::logId - 1);
        if (logIdLo > logIdHi) return emitEmpty();

        bool haveLogTypeFilter = filters.isObject() && filters.isMember("logType");
        unsigned int wantedLogType = 0;
        if (haveLogTypeFilter)
        {
            try { wantedLogType = (unsigned int)std::stoul(filters["logType"].asString()); }
            catch (...) { return emitEmpty(); }
        }

        unsigned long long totalMatched = 0;
        const unsigned long long pageEndExclusive = (unsigned long long)offset + (unsigned long long)size;
        std::vector<Json::Value> page;
        page.reserve(size);
        std::deque<Json::Value> dtail;

        unsigned int cachedTick = (unsigned int)-1;
        TickData cachedTickData;
        qLogger::TickBlobInfo cachedTbi;
        std::vector<std::string> cachedTxHashes;

        bool stopAll = false;
        for (unsigned long long lid = logIdLo; lid <= logIdHi && !stopAll; lid++)
        {
            PinScope _pinScope;
            qLogger::BlobInfo bi = qLogger::logBuf.getBlobInfo(lid);
            if (bi.startIndex < 0 || bi.length <= 0) continue;
            unsigned long long entryLen = (unsigned long long)bi.length;
            if (entryLen < LOG_HEADER_SIZE) continue;
            static constexpr unsigned long long kMaxEntryLen = LOG_HEADER_SIZE + (1ULL << 24);
            if (entryLen > kMaxEntryLen) continue;

            std::vector<unsigned char> blob(entryLen);
            qLogger::logBuffer.getMany(reinterpret_cast<char *>(blob.data()), bi.startIndex, entryLen);
            const unsigned char *hp = blob.data();
            unsigned int headerEpoch = QV2::readUnaligned<unsigned short>(hp, 0);
            unsigned int headerTick = QV2::readUnaligned<unsigned int>(hp, 2);
            unsigned int sizeAndType = QV2::readUnaligned<unsigned int>(hp, 6);
            unsigned long long headerLogId = QV2::readUnaligned<unsigned long long>(hp, 10);
            unsigned int payloadSize = sizeAndType & 0xFFFFFF;
            unsigned char logType = (unsigned char)(sizeAndType >> 24);
            if ((unsigned long long)LOG_HEADER_SIZE + payloadSize > entryLen) continue;

            if (headerTick < tickLo || headerTick > tickHi) continue;
            if (haveLogTypeFilter && logType != wantedLogType) continue;

            if (headerTick != cachedTick)
            {
                cachedTick = headerTick;
                setMem(&cachedTickData, sizeof(TickData), 0);
                TickStorage::tickData.acquireLock();
                TickData *td = TickStorage::tickData.getByTickIfNotEmpty(headerTick);
                if (td) copyMem(&cachedTickData, td, sizeof(TickData));
                TickStorage::tickData.releaseLock();

                qLogger::tx.getTickLogIdInfo(&cachedTbi, headerTick);

                cachedTxHashes.assign(NUMBER_OF_TRANSACTIONS_PER_TICK, std::string());
                if (td)
                {
                    ts.tickTransactions.acquireLock();
                    unsigned long long *offsetsArr = ts.tickTransactionOffsets.getByTickInCurrentEpoch(headerTick);
                    for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
                    {
                        if (isZero(cachedTickData.transactionDigests[i]) || !offsetsArr[i])
                            continue;
                        Transaction *txPtr = ts.tickTransactions(offsetsArr[i]);
                        unsigned int txTotalSize = txPtr->totalSize();
                        unsigned char digest[32];
                        KangarooTwelve(txPtr, txTotalSize, digest, 32);
                        cachedTxHashes[i] = QV2::identityFromBytes(digest, true);
                    }
                    ts.tickTransactions.releaseLock();
                }
            }

            int eventTxId = -1;
            for (int i = 0; i < LOG_TX_PER_TICK; i++)
            {
                long long fl = cachedTbi.fromLogId[i];
                long long ln = cachedTbi.length[i];
                if (fl < 0 || ln <= 0) continue;
                if ((long long)headerLogId >= fl && (long long)headerLogId < fl + ln)
                {
                    eventTxId = i;
                    break;
                }
            }

            std::string txHashForEvent;
            Json::Value categories(Json::arrayValue);
            if (eventTxId >= 0 && eventTxId < (int)NUMBER_OF_TRANSACTIONS_PER_TICK)
            {
                txHashForEvent = cachedTxHashes[eventTxId];
            }
            else if (eventTxId >= (int)NUMBER_OF_TRANSACTIONS_PER_TICK)
            {
                categories.append((int)(eventTxId - NUMBER_OF_TRANSACTIONS_PER_TICK + 1));
            }

            if (useHashAnchor && (headerTick != hashAnchorTick || eventTxId != (int)hashAnchorTxId))
                continue;

            const unsigned char *payload = blob.data() + LOG_HEADER_SIZE;
            if (!QV2::eventMatchesFilters(logType, headerEpoch, headerTick, headerLogId, txHashForEvent, categories, payload, payloadSize, cachedTickData,
                                          filters, exclude, should, ranges))
                continue;

            bool wantBuild = descOrder || (totalMatched >= (unsigned long long)offset && totalMatched < pageEndExclusive);
            if (wantBuild)
            {
                Json::Value je = QV2::eventLogToJson(reinterpret_cast<const char *>(blob.data()), (unsigned int)entryLen,
                                                     cachedTickData, txHashForEvent, categories);
                if (descOrder)
                {
                    dtail.push_back(std::move(je));
                    if (dtail.size() > (size_t)pageEndExclusive) dtail.pop_front();
                }
                else
                {
                    page.push_back(std::move(je));
                }
            }
            totalMatched++;
            if (!descOrder && totalMatched >= 10000 && totalMatched >= pageEndExclusive) stopAll = true;
        }

        unsigned long long capped = std::min<unsigned long long>(totalMatched, 10000ULL);
        Json::Value eventLogs(Json::arrayValue);
        if (descOrder)
        {
            long long K = (long long)dtail.size();
            long long ls = K - (long long)offset - (long long)size; if (ls < 0) ls = 0;
            long long le = K - (long long)offset; if (le < 0) le = 0; if (le > K) le = K;
            for (long long i = le - 1; i >= ls; i--) eventLogs.append(dtail[(size_t)i]);
        }
        else
        {
            for (auto &e : page) eventLogs.append(std::move(e));
        }

        Json::Value hits;
        hits["total"] = (Json::UInt)capped;
        hits["from"] = offset;
        hits["size"] = (Json::UInt)eventLogs.size();
        result["hits"] = hits;
        result["eventLogs"] = eventLogs;
        result["validForTick"] = validForTick;
        return jsonResp(result);
    }
    catch (const std::exception &e)
    {
        Json::Value result;
        result["code"] = -1;
        result["message"] = std::string("Internal server error: ") + e.what();
        return jsonResp(result, 500);
    }
#endif
}

#endif // __linux__ || LITE_WASM_SC
