#pragma once
#include "../utils.h"
#include "extensions/utils.h"
#include <drogon/HttpController.h>
#include <fmt/format.h>
#include <optional>
#include <deque>
using namespace drogon;

namespace RpcQueryV2
{

enum StatusCode
{
    BadRequest = 3,
    NotFound = 5,
};

namespace MiddleWare
{
class TickNumberVerifier : public HttpMiddleware<TickNumberVerifier>
{
  public:
    TickNumberVerifier() {}

    void invoke(const HttpRequestPtr &req,
                MiddlewareNextCallback &&nextCb,
                MiddlewareCallback &&mcb) override
    {
        Json::Value result;
        auto json = req->getJsonObject();
        if (!json)
        {
            result["code"] = StatusCode::BadRequest;
            result["message"] = "Invalid JSON";
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            mcb(res);
            return;
        }

        // check if tickNumber field exists
        if (!(*json).isMember("tickNumber"))
        {
            result["code"] = StatusCode::BadRequest;
            result["message"] = "Missing tickNumber field";
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            mcb(res);
            return;
        }

        unsigned int tickNumber = (*json)["tickNumber"].asUInt64();
        if (tickNumber > system.tick)
        {
            result["code"] = StatusCode::BadRequest;
            result["message"] = fmt::format("invalid tick number: rpc error: code = FailedPrecondition desc = requested tick number {} is greater than last processed tick {}", tickNumber, system.tick);
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            mcb(res);
            return;
        }
        else if (tickNumber < system.initialTick)
        {
            result["code"] = StatusCode::BadRequest;
            result["message"] = fmt::format("invalid tick number: rpc error: code = OutOfRange desc = provided tick number {} was skipped by the system, next available tick is {}", tickNumber, system.initialTick);
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            mcb(res);
            return;
        }

        nextCb([mcb = std::move(mcb)](const HttpResponsePtr &resp)
               { mcb(resp); });
    }
};
} // namespace MiddleWare

class RpcQueryV2Controller : public HttpController<RpcQueryV2Controller>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RpcQueryV2Controller::getComputorListsForEpoch, "/query/v1/getComputorListsForEpoch", Post);
    ADD_METHOD_TO(RpcQueryV2Controller::getLastProcessedTick, "/query/v1/getLastProcessedTick", Get);
    ADD_METHOD_TO(RpcQueryV2Controller::getProcessedTickIntervals, "/query/v1/getProcessedTickIntervals", Get);
    ADD_METHOD_TO(RpcQueryV2Controller::getTickData, "/query/v1/getTickData", Post, "RpcQueryV2::MiddleWare::TickNumberVerifier");
    ADD_METHOD_TO(RpcQueryV2Controller::getTransactionByHash, "/query/v1/getTransactionByHash", Post);
    ADD_METHOD_TO(RpcQueryV2Controller::getTransactionsForIdentity, "/query/v1/getTransactionsForIdentity", Post);
    ADD_METHOD_TO(RpcQueryV2Controller::getTransactionsForTick, "/query/v1/getTransactionsForTick", Post, "RpcQueryV2::MiddleWare::TickNumberVerifier");
    ADD_METHOD_TO(RpcQueryV2Controller::getEventLogs, "/query/v1/getEventLogs", Post);
    ADD_METHOD_TO(RpcQueryV2Controller::getTransfersForIdentity, "/query/v1/getTransfersForIdentity", Post);
    ADD_METHOD_TO(RpcQueryV2Controller::getContractCalls,        "/query/v1/getContractCalls",        Post);
    ADD_METHOD_TO(RpcQueryV2Controller::getContracts,            "/query/v1/getContracts",            Get);
    ADD_METHOD_TO(RpcQueryV2Controller::getVotesForTick,         "/query/v1/getVotesForTick",         Post);
    METHOD_LIST_END

    inline void getComputorListsForEpoch(const HttpRequestPtr &req,
                                         std::function<void(const HttpResponsePtr &)> &&cb)
    {
        Json::Value result;
        auto json = req->getJsonObject();
        if (!json)
        {
            result["code"] = -1;
            result["message"] = "Invalid JSON";
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            cb(res);
            return;
        }

        // check if epoch field exists
        if (!(*json).isMember("epoch"))
        {
            result["code"] = -1;
            result["message"] = "Missing epoch field";
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            cb(res);
            return;
            return;
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

        cb(HttpResponse::newHttpJsonResponse(result));
    }

    inline void getLastProcessedTick(const HttpRequestPtr &req,
                                     std::function<void(const HttpResponsePtr &)> &&cb)
    {
        Json::Value result;
        result["tickNumber"] = system.tick;
        result["epoch"] = system.epoch;
        result["intervalInitialTick"] = system.initialTick;
        cb(HttpResponse::newHttpJsonResponse(result));
    }

    inline void getProcessedTickIntervals(const HttpRequestPtr &req,
                                          std::function<void(const HttpResponsePtr &)> &&cb)
    {
        Json::Value result(Json::arrayValue);
        Json::Value tickInterval;
        tickInterval["epoch"] = system.epoch;
        tickInterval["firstTick"] = system.initialTick;
        tickInterval["lastTick"] = system.tick;
        result.append(tickInterval);
        cb(HttpResponse::newHttpJsonResponse(result));
    }

    inline void getTickData(const HttpRequestPtr &req,
                            std::function<void(const HttpResponsePtr &)> &&cb)
    {
        Json::Value result;
        auto json = req->getJsonObject();
        unsigned int tickNumber = (*json)["tickNumber"].asUInt64();
        TickData localTickData;
        TickStorage::tickData.acquireLock();
        auto tickData = TickStorage::tickData.getByTickIfNotEmpty(tickNumber);
        if (tickData)
        {
            copyMem(&localTickData, tickData.get(), sizeof(TickData));
        }
        TickStorage::tickData.releaseLock();
        if (!tickData)
        {
            result["code"] = StatusCode::NotFound;
            result["message"] = "Tick data not found";
            auto resp = HttpResponse::newHttpJsonResponse(result);
            resp->setStatusCode(k404NotFound);
            cb(resp);
            return;
        }

        Json::Value jsonObject;
        jsonObject["tickNumber"] = localTickData.tick;
        jsonObject["epoch"] = localTickData.epoch;
        jsonObject["computorIndex"] = localTickData.computorIndex;
        jsonObject["timelock"] = base64_encode(localTickData.timelock.m256i_u8, 32);
        jsonObject["timestamp"] = HttpUtils::formatTimestamp(
            localTickData.millisecond,
            localTickData.second,
            localTickData.minute,
            localTickData.hour,
            localTickData.day,
            localTickData.month,
            localTickData.year);
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
        jsonObject["transactionDigests"] = txDigestsJson;
        Json::Value contractFeesJson(Json::arrayValue);
        for (unsigned int i = 0; i < MAX_NUMBER_OF_CONTRACTS; i++)
        {
            contractFeesJson.append(Json::UInt64(localTickData.contractFees[i]));
        }
        jsonObject["contractFees"] = contractFeesJson;
        jsonObject["signature"] = base64_encode(localTickData.signature, SIGNATURE_SIZE);

        auto resp = HttpResponse::newHttpJsonResponse(jsonObject);
        cb(resp);
    }

    inline void getTransactionByHash(const HttpRequestPtr &req,
                                     std::function<void(const HttpResponsePtr &)> &&cb)
    {
        Json::Value result;
        auto json = req->getJsonObject();
        if (!json)
        {
            result["code"] = -1;
            result["message"] = "Invalid JSON";
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            cb(res);
            return;
        }

        // check if txHash field exists
        if (!(*json).isMember("hash"))
        {
            result["code"] = -1;
            result["message"] = "Missing hash field";
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            cb(res);
            return;
        }

        std::string txHash = (*json)["hash"].asString();
        if (txHash.length() != 60)
        {
            result["code"] = StatusCode::BadRequest;
            result["message"] = fmt::format("invalid id format: converting id to pubkey: invalid ID length, expected 60, found {}", txHash.length());
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            cb(res);
            return;
        }
        // To upperse txhash
        std::transform(txHash.begin(), txHash.end(), txHash.begin(), ::toupper);
        m256i txDigest = {};
        if (!getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(txHash.c_str()), txDigest.m256i_u8))
        {
            result["code"] = StatusCode::BadRequest;
            result["message"] = fmt::format("invalid id format: invalid hash [{}]", txHash);
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            cb(res);
            return;
        }

        unsigned int foundTick = 0;
        unsigned int foundSlot = 0;
        bool found = false;
        TickData localTickData;
        // Optional tickNumber hint bounds the scan to one tick instead of the whole epoch.
        const bool hasTickHint = (*json).isMember("tickNumber");
        const unsigned int scanLo = hasTickHint ? (*json)["tickNumber"].asUInt() : system.initialTick;
        const unsigned int scanHi = hasTickHint ? scanLo : system.tick;
        for (unsigned int tick = scanLo; tick <= scanHi && !found; tick++)
        {
            TickStorage::tickData.acquireLock();
            auto tickData = TickStorage::tickData.getByTickIfNotEmpty(tick);
            if (tickData)
            {
                copyMem(&localTickData, tickData.get(), sizeof(TickData));
            }
            TickStorage::tickData.releaseLock();
            if (!tickData)
            {
                continue;
            }
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
            result["code"] = StatusCode::NotFound;
            result["message"] = "Transaction not found";
            auto resp = HttpResponse::newHttpJsonResponse(result);
            resp->setStatusCode(k404NotFound);
            cb(resp);
            return;
        }

        ts.tickTransactions.acquireLock();
        unsigned long long txOffset = ts.tickTransactionOffsets(foundTick, foundSlot);
        if (!txOffset)
        {
            ts.tickTransactions.releaseLock();
            result["code"] = StatusCode::NotFound;
            result["message"] = "Transaction not found";
            auto resp = HttpResponse::newHttpJsonResponse(result);
            resp->setStatusCode(k404NotFound);
            cb(resp);
            return;
        }
        Transaction *txPtr = ts.tickTransactions(txOffset);
        const unsigned int txTotalSize = txPtr->totalSize();
        std::vector<unsigned char> txBuf(txTotalSize);
        copyMem(txBuf.data(), txPtr, txTotalSize);
        ts.tickTransactions.releaseLock();

        Json::Value jsonObject = HttpUtils::transactionToJson(reinterpret_cast<Transaction *>(txBuf.data()));
        auto resp = HttpResponse::newHttpJsonResponse(jsonObject);
        cb(resp);
    }

    inline void getTransactionsForIdentity(const HttpRequestPtr &req,
                                           std::function<void(const HttpResponsePtr &)> &&cb)
    {
        try
        {
            Json::Value result;
            auto json = req->getJsonObject();
            if (!json)
            {
                result["code"] = StatusCode::BadRequest;
                result["message"] = "Invalid JSON";
                auto res = HttpResponse::newHttpJsonResponse(result);
                res->setStatusCode(k400BadRequest);
                cb(res);
                return;
            }

            // check if identity field exists
            if (!(*json).isMember("identity"))
            {
                result["code"] = StatusCode::BadRequest;
                result["message"] = "Missing identity field";
                auto res = HttpResponse::newHttpJsonResponse(result);
                res->setStatusCode(k400BadRequest);
                cb(res);
                return;
            }

            // type: map<string,string>
            auto filters = (*json)["filters"];
            // type: map<string, Range>
            auto ranges = (*json)["ranges"];
            // type
            // {
            //     offset: number,
            //     size: number
            // }
            auto pagination = (*json)["pagination"];

            std::string identityStr = (*json)["identity"].asString();
            m256i publicKey{};
            if (!getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(identityStr.c_str()), publicKey.m256i_u8))
            {
                result["code"] = StatusCode::BadRequest;
                result["message"] = fmt::format("invalid id format: invalid identity [{}]", identityStr);
                auto res = HttpResponse::newHttpJsonResponse(result);
                res->setStatusCode(k400BadRequest);
                cb(res);
                return;
            }

            Json::Value transactions(Json::arrayValue);
            std::vector<std::vector<unsigned char>> matchedBufs;
            for (unsigned int tick = system.initialTick; tick <= system.tick; tick++)
            {
                TickData localTickData;
                TickStorage::tickData.acquireLock();
                auto tickData = TickStorage::tickData.getByTickIfNotEmpty(tick);
                if (tickData)
                {
                    copyMem(&localTickData, tickData.get(), sizeof(TickData));
                }
                TickStorage::tickData.releaseLock();
                if (!tickData)
                {
                    continue;
                }

                ts.tickTransactions.acquireLock();
                auto offsets = ts.tickTransactionOffsets.getByTickInCurrentEpoch(tick);
                for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
                {
                    if (isZero(localTickData.transactionDigests[i]) || !offsets[i])
                    {
                        continue;
                    }
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
            {
                transactions.append(HttpUtils::transactionToJson(reinterpret_cast<Transaction *>(buf.data())));
            }

            // filter transactions based on filters
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
                    {
                        filteredTransactions.append(tx);
                    }
                }
                transactions = filteredTransactions;
            }
            // filter transactions based on ranges
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
                                    if (!(stoull(tx[key].asString()) < stoull(range["lt"].asString())))
                                    {
                                        match = false;
                                        break;
                                    }
                                }
                                if (range.isMember("gt"))
                                {
                                    if (!(stoull(tx[key].asString()) > stoull(range["gt"].asString())))
                                    {
                                        match = false;
                                        break;
                                    }
                                }
                                if (range.isMember("lte"))
                                {
                                    if (!(stoull(tx[key].asString()) <= stoull(range["lte"].asString())))
                                    {
                                        match = false;
                                        break;
                                    }
                                }
                                if (range.isMember("gte"))
                                {
                                    if (!(stoull(tx[key].asString()) >= stoull(range["gte"].asString())))
                                    {
                                        match = false;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (match)
                    {
                        rangedTransactions.append(tx);
                    }
                }
                transactions = rangedTransactions;
            }
            // apply pagination
            if (pagination.isObject())
            {
                unsigned int offset = 0;
                unsigned int size = 0;
                if (pagination.isMember("offset"))
                {
                    offset = pagination["offset"].asUInt64();
                }
                offset = std::min(offset, (unsigned int)10000);
                if (pagination.isMember("size"))
                {
                    size = pagination["size"].asUInt64();
                }
                else
                {
                    size = 10;
                }
                size = std::min(size, (unsigned int)1000);
                Json::Value paginatedTransactions(Json::arrayValue);
                for (unsigned int i = offset; i < transactions.size() && i < offset + size; i++)
                {
                    paginatedTransactions.append(transactions[i]);
                }
                transactions = paginatedTransactions;
            }

            result["transactions"] = transactions;
            result["validForTick"] = 0;
            result["hits"]["total"] = transactions.size();
            result["hits"]["from"] = 0;
            result["hits"]["size"] = transactions.size();
            auto resp = HttpResponse::newHttpJsonResponse(result);
            cb(resp);
        }
        catch (const std::exception &e)
        {
            Json::Value result;
            result["code"] = -1;
            result["message"] = std::string("Internal server error: ") + e.what();
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k500InternalServerError);
            cb(res);
        }
    }

    // Returns recent transfers (incoming + outgoing) for an identity.
    // Request:  {identity, direction?:"in"|"out"|"both" (default "both"), limit?:50}
    // Response: {transactions: [{ ...tx, direction: "in"|"out" }]}, newest tick first.
    inline void getTransfersForIdentity(const HttpRequestPtr &req,
                                        std::function<void(const HttpResponsePtr &)> &&cb)
    {
        Json::Value result;
        auto json = req->getJsonObject();
        if (!json || !(*json).isMember("identity"))
        {
            result["code"] = StatusCode::BadRequest;
            result["message"] = "Missing identity field";
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            cb(res);
            return;
        }

        std::string identityStr = (*json)["identity"].asString();
        m256i publicKey{};
        if (!getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(identityStr.c_str()), publicKey.m256i_u8))
        {
            result["code"] = StatusCode::BadRequest;
            result["message"] = fmt::format("invalid identity [{}]", identityStr);
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            cb(res);
            return;
        }

        std::string direction = (*json).get("direction", "both").asString();
        const bool wantIn  = (direction == "in"  || direction == "both");
        const bool wantOut = (direction == "out" || direction == "both");
        const unsigned int limit = (*json).get("limit", 50).asUInt();

        Json::Value transactions(Json::arrayValue);
        // pair of (buf, direction); walk newest tick → oldest, stop at limit.
        struct Hit { std::vector<unsigned char> buf; const char* dir; };
        std::vector<Hit> hits;
        hits.reserve(limit);

        for (unsigned int tick = system.tick; tick + 1 > system.initialTick && hits.size() < limit; tick--)
        {
            TickData localTickData;
            TickStorage::tickData.acquireLock();
            auto tickData = TickStorage::tickData.getByTickIfNotEmpty(tick);
            if (tickData) copyMem(&localTickData, tickData.get(), sizeof(TickData));
            TickStorage::tickData.releaseLock();
            if (!tickData)
            {
                if (tick == 0) break;
                continue;
            }

            ts.tickTransactions.acquireLock();
            auto offsets = ts.tickTransactionOffsets.getByTickInCurrentEpoch(tick);
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
        cb(HttpResponse::newHttpJsonResponse(result));
    }

    // Server-side scan for txs whose destination is a smart-contract pubkey.
    // A contract pubkey is m256i(idx, 0, 0, 0) — upper 3 uint64 chunks are zero.
    // Request:  {fromTick, toTick, contractIndex?, page?:0, pageSize?:50}
    // Response: {fromTick, toTick, total, page, pageSize, transactions:[{...tx, contractIndex}]}
    // toTick capped at system.tick. fromTick clamped to system.initialTick.
    // Scan range capped at 1000 ticks per call to bound work.
    inline void getContractCalls(const HttpRequestPtr &req,
                                 std::function<void(const HttpResponsePtr &)> &&cb)
    {
        Json::Value result;
        auto json = req->getJsonObject();
        if (!json)
        {
            result["code"] = StatusCode::BadRequest;
            result["message"] = "Invalid JSON";
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            cb(res);
            return;
        }

        unsigned int toTick = (*json).get("toTick", system.tick).asUInt();
        unsigned int fromTick = (*json).get("fromTick", 0).asUInt();
        const bool hasFilter = (*json).isMember("contractIndex");
        const unsigned int filterIdx = hasFilter ? (*json)["contractIndex"].asUInt() : 0;
        const unsigned int page = (*json).get("page", 0).asUInt();
        const unsigned int pageSize = std::min((unsigned int)200, (*json).get("pageSize", 50).asUInt());

        // contract indices: 1..contractCount-1. idx 0 is the null/burn sentinel.
        if (hasFilter && (filterIdx < 1 || filterIdx >= contractCount))
        {
            result["code"] = StatusCode::BadRequest;
            result["message"] = "contractIndex out of range";
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            cb(res);
            return;
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
            cb(HttpResponse::newHttpJsonResponse(result));
            return;
        }
        // cap range to 1000 ticks
        if (toTick - fromTick + 1 > 1000) {
            fromTick = toTick - 1000 + 1;
        }

        struct Hit { std::vector<unsigned char> buf; unsigned int idx; };
        std::vector<Hit> hits;
        hits.reserve(256);

        for (unsigned int tick = toTick; tick + 1 > fromTick; tick--)
        {
            TickData localTickData;
            TickStorage::tickData.acquireLock();
            auto tickData = TickStorage::tickData.getByTickIfNotEmpty(tick);
            if (tickData) copyMem(&localTickData, tickData.get(), sizeof(TickData));
            TickStorage::tickData.releaseLock();
            if (!tickData) {
                if (tick == 0) break;
                continue;
            }

            ts.tickTransactions.acquireLock();
            auto offsets = ts.tickTransactionOffsets.getByTickInCurrentEpoch(tick);
            for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
            {
                if (isZero(localTickData.transactionDigests[i]) || !offsets[i]) continue;
                Transaction *txPtr = ts.tickTransactions(offsets[i]);
                const m256i &dest = txPtr->destinationPublicKey;
                // contract pubkey: upper 3 chunks zero AND chunk-0 in [1, contractCount).
                // (idx=0 / chunk-0 zero is the null/burn pubkey, not a contract.)
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
        cb(HttpResponse::newHttpJsonResponse(result));
    }

    // Return per-computor Tick votes for the given tick. Each non-empty Tick struct
    // (one per computor index 0..675) is serialized with its digests and signature.
    // Request:  {tickNumber}
    // Response: {tickNumber, votes:[{computorIndex, epoch, tick, timestamp,
    //   prevSpectrumDigest, saltedSpectrumDigest, prevUniverseDigest, saltedUniverseDigest,
    //   prevComputerDigest, saltedComputerDigest, transactionDigest,
    //   expectedNextTickTransactionDigest,
    //   prevResourceTestingDigest, saltedResourceTestingDigest,
    //   prevTransactionBodyDigest, saltedTransactionBodyDigest, signature}], count}
    inline void getVotesForTick(const HttpRequestPtr &req,
                                std::function<void(const HttpResponsePtr &)> &&cb)
    {
        Json::Value result;
        auto json = req->getJsonObject();
        if (!json || !(*json).isMember("tickNumber"))
        {
            result["code"] = StatusCode::BadRequest;
            result["message"] = "Missing tickNumber";
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k400BadRequest);
            cb(res);
            return;
        }
        unsigned int tickNumber = (*json)["tickNumber"].asUInt();
        if (tickNumber < system.initialTick || tickNumber > system.tick)
        {
            result["code"] = StatusCode::NotFound;
            result["message"] = fmt::format("tick {} out of epoch range [{}, {}]",
                                            tickNumber, system.initialTick, system.tick);
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k404NotFound);
            cb(res);
            return;
        }

        Json::Value votes(Json::arrayValue);

        // copy all 676 per-computor Tick structs under the locks
        Tick localCopy[NUMBER_OF_COMPUTORS];
        for (unsigned int i = 0; i < NUMBER_OF_COMPUTORS; i++)
        {
            ts.ticks.acquireLock(i);
            auto srcBlock = TickStorage::ticks.getByTickInCurrentEpoch(tickNumber);
            copyMem(&localCopy[i], srcBlock + i, sizeof(Tick));
            ts.ticks.releaseLock(i);
        }

        unsigned int count = 0;
        for (unsigned int i = 0; i < NUMBER_OF_COMPUTORS; i++)
        {
            Tick &t = localCopy[i];
            // empty / never-received slot — only current-epoch entries are real votes
            if (t.epoch != system.epoch) continue;
            count++;
            Json::Value v;
            v["computorIndex"] = t.computorIndex;
            v["epoch"] = t.epoch;
            v["tick"] = t.tick;
            v["timestamp"] = HttpUtils::formatTimestamp(t.millisecond, t.second, t.minute,
                                                        t.hour, t.day, t.month, t.year);
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
        cb(HttpResponse::newHttpJsonResponse(result));
    }

    // Read the contractDescriptions[] table from contract_def.h and expose it
    // so the frontend can map contractIndex → name without baking the list in.
    inline void getContracts(const HttpRequestPtr &req,
                             std::function<void(const HttpResponsePtr &)> &&cb)
    {
        Json::Value result;
        Json::Value arr(Json::arrayValue);
        // contractDescriptions[0] is the empty sentinel — skip it.
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
        cb(HttpResponse::newHttpJsonResponse(result));
    }

    inline void getTransactionsForTick(const HttpRequestPtr &req,
                                       std::function<void(const HttpResponsePtr &)> &&cb)
    {
        Json::Value result;
        auto json = req->getJsonObject();
        unsigned int tickNumber = (*json)["tickNumber"].asUInt64();
        TickData localTickData;
        TickStorage::tickData.acquireLock();
        auto tickData = TickStorage::tickData.getByTickIfNotEmpty(tickNumber);
        if (tickData)
        {
            copyMem(&localTickData, tickData.get(), sizeof(TickData));
        }
        TickStorage::tickData.releaseLock();
        if (!tickData)
        {
            result["code"] = StatusCode::NotFound;
            result["message"] = "Tick data not found";
            auto resp = HttpResponse::newHttpJsonResponse(result);
            resp->setStatusCode(k404NotFound);
            cb(resp);
            return;
        }

        Json::Value transactions(Json::arrayValue);
        ts.tickTransactions.acquireLock();
        auto offsets = ts.tickTransactionOffsets.getByTickInCurrentEpoch(tickNumber);
        std::vector<std::vector<unsigned char>> txBufs;
        txBufs.reserve(NUMBER_OF_TRANSACTIONS_PER_TICK);
        for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
        {
            if (isZero(localTickData.transactionDigests[i]) || !offsets[i])
            {
                continue;
            }
            Transaction *txPtr = ts.tickTransactions(offsets[i]);
            const unsigned int txTotalSize = txPtr->totalSize();
            txBufs.emplace_back(txTotalSize);
            copyMem(txBufs.back().data(), txPtr, txTotalSize);
        }
        ts.tickTransactions.releaseLock();

        for (auto &buf : txBufs)
        {
            transactions.append(HttpUtils::transactionToJson(reinterpret_cast<Transaction *>(buf.data())));
        }
        result["transactions"] = transactions;
        auto resp = HttpResponse::newHttpJsonResponse(result);
        cb(resp);
    }

#if ENABLED_LOGGING
    // Encode a 32-byte pubkey as a 60-char identity string (uppercase or lowercase per the spec).
    static std::string identityFromBytes(const unsigned char *bytes, bool lowercase)
    {
        CHAR16 id[61] = {};
        getIdentity(const_cast<unsigned char *>(bytes), id, lowercase);
        return wchar_to_string(id);
    }

    // Read N bytes from an unaligned offset into the payload.
    template <typename T>
    static T readUnaligned(const unsigned char *p, size_t off)
    {
        T v{};
        memcpy(&v, p + off, sizeof(T));
        return v;
    }

    // Return the value of a filterable field, as the string representation the spec uses
    // ("123" for numbers, 60-char identity for pubkeys, 7-char trimmed for asset name).
    // Returns std::nullopt if the field is not present on this payload type.
    static std::optional<std::string> payloadFieldValue(unsigned int logType,
                                                        const unsigned char *p,
                                                        unsigned int payloadSize,
                                                        const std::string &field)
    {
        auto fitId = [&](size_t off, bool lowercase = false) -> std::optional<std::string>
        {
            if (off + 32 > payloadSize) return std::nullopt;
            return identityFromBytes(p + off, lowercase);
        };
        auto fitU64 = [&](size_t off) -> std::optional<std::string>
        {
            if (off + 8 > payloadSize) return std::nullopt;
            return std::to_string(readUnaligned<unsigned long long>(p, off));
        };
        auto fitI64 = [&](size_t off) -> std::optional<std::string>
        {
            if (off + 8 > payloadSize) return std::nullopt;
            return std::to_string(readUnaligned<long long>(p, off));
        };
        auto fitU32 = [&](size_t off) -> std::optional<std::string>
        {
            if (off + 4 > payloadSize) return std::nullopt;
            return std::to_string(readUnaligned<unsigned int>(p, off));
        };
        auto fitI32 = [&](size_t off) -> std::optional<std::string>
        {
            if (off + 4 > payloadSize) return std::nullopt;
            return std::to_string(readUnaligned<int>(p, off));
        };
        auto fitU8 = [&](size_t off) -> std::optional<std::string>
        {
            if (off + 1 > payloadSize) return std::nullopt;
            return std::to_string((unsigned int)p[off]);
        };
        auto fitName = [&](size_t off) -> std::optional<std::string>
        {
            if (off + 7 > payloadSize) return std::nullopt;
            char name[8] = {0};
            memcpy(name, p + off, 7);
            return std::string(name);
        };

        switch (logType)
        {
        case QU_TRANSFER:
            if (field == "source") return fitId(0);
            if (field == "destination") return fitId(32);
            if (field == "amount") return fitI64(64);
            break;
        case ASSET_ISSUANCE:
            if (field == "assetIssuer") return fitId(0);
            if (field == "numberOfShares") return fitI64(32);
            if (field == "managingContractIndex") return fitI64(40);
            if (field == "assetName") return fitName(48);
            break;
        case ASSET_OWNERSHIP_CHANGE:
        case ASSET_POSSESSION_CHANGE:
            if (field == "source") return fitId(0);
            if (field == "destination") return fitId(32);
            if (field == "assetIssuer") return fitId(64);
            if (field == "numberOfShares") return fitI64(96);
            if (field == "managingContractIndex") return fitI64(104);
            if (field == "assetName") return fitName(112);
            break;
        case ASSET_OWNERSHIP_MANAGING_CONTRACT_CHANGE:
            if (field == "assetIssuer") return fitId(32);
            if (field == "numberOfShares") return fitI64(72);
            if (field == "assetName") return fitName(80);
            break;
        case ASSET_POSSESSION_MANAGING_CONTRACT_CHANGE:
            if (field == "assetIssuer") return fitId(64);
            if (field == "numberOfShares") return fitI64(104);
            if (field == "assetName") return fitName(112);
            break;
        case BURNING:
            if (field == "source") return fitId(0);
            if (field == "amount") return fitI64(32);
            if (field == "contractIndex") return fitU32(40);
            break;
        case CONTRACT_ERROR_MESSAGE:
        case CONTRACT_WARNING_MESSAGE:
        case CONTRACT_INFORMATION_MESSAGE:
        case CONTRACT_DEBUG_MESSAGE:
            if (field == "contractIndex") return fitU32(0);
            if (field == "contractMessageType") return fitU32(4);
            break;
        case CONTRACT_RESERVE_DEDUCTION:
            if (field == "deductedAmount") return fitU64(0);
            if (field == "remainingAmount") return fitI64(8);
            if (field == "contractIndex") return fitU32(16);
            break;
        case ORACLE_QUERY_STATUS_CHANGE:
            if (field == "queryingEntity") return fitId(0);
            if (field == "queryId") return fitI64(32);
            if (field == "interfaceIndex") return fitU32(40);
            if (field == "queryType") return fitU8(44);
            if (field == "queryStatus") return fitU8(45);
            break;
        case ORACLE_SUBSCRIBER_MESSAGE:
            if (field == "subscriptionId") return fitI32(0);
            if (field == "interfaceIndex") return fitU32(4);
            if (field == "contractIndex") return fitU32(8);
            break;
        case CUSTOM_MESSAGE:
            if (field == "customMessage") return fitU64(0);
            break;
        default:
            break;
        }
        return std::nullopt;
    }

    // Compare value against a Range (gt/gte/lt/lte). All comparisons are unsigned-decimal.
    // Returns true if value satisfies the range, false otherwise.
    static bool rangeMatches(const std::string &value, const Json::Value &range)
    {
        if (!range.isObject()) return true;
        unsigned long long v;
        try { v = std::stoull(value); } catch (...) { return false; }
        if (range.isMember("gt"))
        {
            try { if (!(v > std::stoull(range["gt"].asString()))) return false; }
            catch (...) { return false; }
        }
        if (range.isMember("gte"))
        {
            try { if (!(v >= std::stoull(range["gte"].asString()))) return false; }
            catch (...) { return false; }
        }
        if (range.isMember("lt"))
        {
            try { if (!(v < std::stoull(range["lt"].asString()))) return false; }
            catch (...) { return false; }
        }
        if (range.isMember("lte"))
        {
            try { if (!(v <= std::stoull(range["lte"].asString()))) return false; }
            catch (...) { return false; }
        }
        return true;
    }

    // Build the typed payload sub-object for an event, based on logType.
    static void buildTypedPayload(unsigned int logType,
                                  const unsigned char *p,
                                  unsigned int payloadSize,
                                  Json::Value &out)
    {
        switch (logType)
        {
        case QU_TRANSFER:
        {
            if (payloadSize < 72) break;
            Json::Value sub;
            sub["source"] = identityFromBytes(p, false);
            sub["destination"] = identityFromBytes(p + 32, false);
            sub["amount"] = std::to_string(readUnaligned<long long>(p, 64));
            out["quTransfer"] = sub;
            break;
        }
        case ASSET_ISSUANCE:
        {
            if (payloadSize < 63) break;
            Json::Value sub;
            sub["assetIssuer"] = identityFromBytes(p, false);
            sub["numberOfShares"] = std::to_string(readUnaligned<long long>(p, 32));
            sub["managingContractIndex"] = std::to_string(readUnaligned<long long>(p, 40));
            char name[8] = {0};
            memcpy(name, p + 48, 7);
            sub["assetName"] = std::string(name);
            sub["numberOfDecimalPlaces"] = (unsigned int)(unsigned char)p[55];
            char uom[8] = {0};
            memcpy(uom, p + 56, 7);
            sub["unitOfMeasurement"] = std::string(uom);
            out["assetIssuance"] = sub;
            break;
        }
        case ASSET_OWNERSHIP_CHANGE:
        case ASSET_POSSESSION_CHANGE:
        {
            if (payloadSize < 127) break;
            Json::Value sub;
            sub["source"] = identityFromBytes(p, false);
            sub["destination"] = identityFromBytes(p + 32, false);
            sub["assetIssuer"] = identityFromBytes(p + 64, false);
            sub["numberOfShares"] = std::to_string(readUnaligned<long long>(p, 96));
            char name[8] = {0};
            memcpy(name, p + 112, 7);
            sub["assetName"] = std::string(name);
            out[logType == ASSET_OWNERSHIP_CHANGE ? "assetOwnershipChange" : "assetPossessionChange"] = sub;
            break;
        }
        case ASSET_OWNERSHIP_MANAGING_CONTRACT_CHANGE:
        {
            if (payloadSize < 87) break;
            Json::Value sub;
            sub["owner"] = identityFromBytes(p, false);
            sub["assetIssuer"] = identityFromBytes(p + 32, false);
            sub["sourceContractIndex"] = std::to_string(readUnaligned<unsigned int>(p, 64));
            sub["destinationContractIndex"] = std::to_string(readUnaligned<unsigned int>(p, 68));
            sub["numberOfShares"] = std::to_string(readUnaligned<long long>(p, 72));
            char name[8] = {0};
            memcpy(name, p + 80, 7);
            sub["assetName"] = std::string(name);
            out["assetOwnershipManagingContractChange"] = sub;
            break;
        }
        case ASSET_POSSESSION_MANAGING_CONTRACT_CHANGE:
        {
            if (payloadSize < 119) break;
            Json::Value sub;
            sub["possessor"] = identityFromBytes(p, false);
            sub["owner"] = identityFromBytes(p + 32, false);
            sub["assetIssuer"] = identityFromBytes(p + 64, false);
            sub["sourceContractIndex"] = std::to_string(readUnaligned<unsigned int>(p, 96));
            sub["destinationContractIndex"] = std::to_string(readUnaligned<unsigned int>(p, 100));
            sub["numberOfShares"] = std::to_string(readUnaligned<long long>(p, 104));
            char name[8] = {0};
            memcpy(name, p + 112, 7);
            sub["assetName"] = std::string(name);
            out["assetPossessionManagingContractChange"] = sub;
            break;
        }
        case BURNING:
        {
            if (payloadSize < 44) break;
            Json::Value sub;
            sub["source"] = identityFromBytes(p, false);
            sub["amount"] = std::to_string(readUnaligned<long long>(p, 32));
            sub["contractIndex"] = std::to_string(readUnaligned<unsigned int>(p, 40));
            out["burning"] = sub;
            break;
        }
        case CONTRACT_ERROR_MESSAGE:
        case CONTRACT_WARNING_MESSAGE:
        case CONTRACT_INFORMATION_MESSAGE:
        case CONTRACT_DEBUG_MESSAGE:
        {
            if (payloadSize < 8) break;
            Json::Value sub;
            sub["contractIndex"] = std::to_string(readUnaligned<unsigned int>(p, 0));
            sub["contractMessageType"] = std::to_string(readUnaligned<unsigned int>(p, 4));
            out["smartContractMessage"] = sub;
            break;
        }
        case CONTRACT_RESERVE_DEDUCTION:
        {
            if (payloadSize < 20) break;
            Json::Value sub;
            sub["deductedAmount"] = std::to_string(readUnaligned<unsigned long long>(p, 0));
            sub["remainingAmount"] = std::to_string(readUnaligned<long long>(p, 8));
            sub["contractIndex"] = std::to_string(readUnaligned<unsigned int>(p, 16));
            out["contractReserveDeduction"] = sub;
            break;
        }
        case ORACLE_QUERY_STATUS_CHANGE:
        {
            if (payloadSize < 46) break;
            Json::Value sub;
            sub["queryingEntity"] = identityFromBytes(p, false);
            sub["queryId"] = std::to_string(readUnaligned<long long>(p, 32));
            sub["interfaceIndex"] = std::to_string(readUnaligned<unsigned int>(p, 40));
            sub["queryType"] = std::to_string((unsigned int)(unsigned char)p[44]);
            sub["queryStatus"] = std::to_string((unsigned int)(unsigned char)p[45]);
            out["oracleQueryStatusChange"] = sub;
            break;
        }
        case ORACLE_SUBSCRIBER_MESSAGE:
        {
            if (payloadSize < 24) break;
            Json::Value sub;
            sub["subscriptionId"] = std::to_string(readUnaligned<int>(p, 0));
            sub["interfaceIndex"] = std::to_string(readUnaligned<unsigned int>(p, 4));
            sub["contractIndex"] = std::to_string(readUnaligned<unsigned int>(p, 8));
            sub["periodMillis"] = std::to_string(readUnaligned<unsigned int>(p, 12));
            sub["firstQueryTimestamp"] = std::to_string(readUnaligned<unsigned long long>(p, 16));
            out["oracleSubscriberLogMessage"] = sub;
            break;
        }
        case CUSTOM_MESSAGE:
        {
            if (payloadSize < 8) break;
            Json::Value sub;
            sub["value"] = std::to_string(readUnaligned<unsigned long long>(p, 0));
            out["customMessage"] = sub;
            break;
        }
        default:
            // DUST_BURNING (9), SPECTRUM_STATS (10), and unknown types fall through.
            // The spec doesn't enumerate them; clients can still consume rawPayload.
            break;
        }
    }

    // Build the full Event JSON object from one raw [header || payload] log blob.
    static Json::Value eventLogToJson(const char *blob,
                                      unsigned int blobLen,
                                      const TickData &tickDataForTimestamp,
                                      const std::string &transactionHashOrEmpty,
                                      const Json::Value &categories)
    {
        Json::Value out;
        if (blobLen < LOG_HEADER_SIZE) return out;
        const unsigned char *p = reinterpret_cast<const unsigned char *>(blob);
        unsigned short epoch = readUnaligned<unsigned short>(p, 0);
        unsigned int tick = readUnaligned<unsigned int>(p, 2);
        unsigned int sizeAndType = readUnaligned<unsigned int>(p, 6);
        unsigned long long logId = readUnaligned<unsigned long long>(p, 10);
        unsigned long long logDigest = readUnaligned<unsigned long long>(p, 18);
        unsigned int payloadSize = sizeAndType & 0xFFFFFF;
        unsigned char logType = (unsigned char)(sizeAndType >> 24);
        if (LOG_HEADER_SIZE + (unsigned long long)payloadSize > blobLen)
        {
            payloadSize = (blobLen >= LOG_HEADER_SIZE) ? (blobLen - LOG_HEADER_SIZE) : 0;
        }
        const unsigned char *payload = p + LOG_HEADER_SIZE;

        out["epoch"] = epoch;
        out["tickNumber"] = tick;
        out["timestamp"] = HttpUtils::formatTimestamp(
            tickDataForTimestamp.millisecond,
            tickDataForTimestamp.second,
            tickDataForTimestamp.minute,
            tickDataForTimestamp.hour,
            tickDataForTimestamp.day,
            tickDataForTimestamp.month,
            tickDataForTimestamp.year);
        out["transactionHash"] = transactionHashOrEmpty;
        out["logType"] = (unsigned int)logType;
        out["logId"] = std::to_string(logId);
        out["logDigest"] = std::to_string(logDigest);
        out["categories"] = categories;
        out["rawPayload"] = base64_encode(const_cast<unsigned char *>(payload), payloadSize);

        buildTypedPayload(logType, payload, payloadSize, out);
        return out;
    }

    // Evaluate one matched event against the request's filters.
    // - logType, transactionHash, epoch, categories are pre-resolved and passed in.
    // - filters/exclude/should/ranges/numericMap come from the request body.
    static bool eventMatchesFilters(unsigned int logType,
                                    unsigned int epoch,
                                    unsigned int tickNumber,
                                    unsigned long long logId,
                                    const std::string &transactionHash,
                                    const Json::Value &categories,
                                    const unsigned char *payload,
                                    unsigned int payloadSize,
                                    const TickData &tickDataForTimestamp,
                                    const Json::Value &filters,
                                    const Json::Value &exclude,
                                    const Json::Value &should,
                                    const Json::Value &ranges)
    {
        // Resolve a field name to its string value for this event (or std::nullopt if absent).
        auto valueOf = [&](const std::string &field) -> std::optional<std::string>
        {
            if (field == "logType") return std::to_string(logType);
            if (field == "epoch") return std::to_string(epoch);
            if (field == "tickNumber") return std::to_string(tickNumber);
            if (field == "logId") return std::to_string(logId);
            if (field == "transactionHash") return transactionHash;
            if (field == "timestamp")
            {
                return HttpUtils::formatTimestamp(
                    tickDataForTimestamp.millisecond,
                    tickDataForTimestamp.second,
                    tickDataForTimestamp.minute,
                    tickDataForTimestamp.hour,
                    tickDataForTimestamp.day,
                    tickDataForTimestamp.month,
                    tickDataForTimestamp.year);
            }
            if (field == "categories")
            {
                if (categories.isArray() && categories.size() > 0)
                    return std::to_string(categories[0].asInt());
                return std::optional<std::string>();
            }
            return payloadFieldValue(logType, payload, payloadSize, field);
        };

        // Include filters: every key must match (value == filters[key]).
        if (filters.isObject())
        {
            for (const auto &key : filters.getMemberNames())
            {
                auto v = valueOf(key);
                if (!v.has_value()) return false;
                if (*v != filters[key].asString()) return false;
            }
        }

        // Exclude filters: any matching key drops the event.
        if (exclude.isObject())
        {
            for (const auto &key : exclude.getMemberNames())
            {
                auto v = valueOf(key);
                if (v.has_value() && *v == exclude[key].asString()) return false;
            }
        }

        // Top-level ranges.
        if (ranges.isObject())
        {
            for (const auto &key : ranges.getMemberNames())
            {
                auto v = valueOf(key);
                if (!v.has_value()) return false;
                if (!rangeMatches(*v, ranges[key])) return false;
            }
        }

        // Should: at least one clause must match (each clause is AND of terms+ranges).
        if (should.isArray() && should.size() > 0)
        {
            bool anyClauseMatches = false;
            for (Json::ArrayIndex i = 0; i < should.size(); i++)
            {
                const Json::Value &clause = should[i];
                bool clauseOk = true;
                const Json::Value &cTerms = clause["terms"];
                if (cTerms.isObject())
                {
                    for (const auto &k : cTerms.getMemberNames())
                    {
                        auto v = valueOf(k);
                        if (!v.has_value() || *v != cTerms[k].asString())
                        {
                            clauseOk = false;
                            break;
                        }
                    }
                }
                if (clauseOk)
                {
                    const Json::Value &cRanges = clause["ranges"];
                    if (cRanges.isObject())
                    {
                        for (const auto &k : cRanges.getMemberNames())
                        {
                            auto v = valueOf(k);
                            if (!v.has_value() || !rangeMatches(*v, cRanges[k]))
                            {
                                clauseOk = false;
                                break;
                            }
                        }
                    }
                }
                if (clauseOk)
                {
                    anyClauseMatches = true;
                    break;
                }
            }
            if (!anyClauseMatches) return false;
        }

        return true;
    }
#endif // ENABLED_LOGGING

    inline void getEventLogs(const HttpRequestPtr &req,
                             std::function<void(const HttpResponsePtr &)> &&cb)
    {
#if !ENABLED_LOGGING
        Json::Value result;
        result["code"] = -1;
        result["message"] = "event logs disabled in this build";
        auto res = HttpResponse::newHttpJsonResponse(result);
        res->setStatusCode(k503ServiceUnavailable);
        cb(res);
        return;
#else
        try
        {
            Json::Value result;
            auto json = req->getJsonObject();
            // drogon returns nullopt for both empty body and parse failure; distinguish
            // via req->body() so empty body is accepted and bad JSON returns 400.
            if (!json && !req->body().empty())
            {
                result["code"] = StatusCode::BadRequest;
                result["message"] = "Invalid JSON";
                auto res = HttpResponse::newHttpJsonResponse(result);
                res->setStatusCode(k400BadRequest);
                cb(res);
                return;
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
                    if (s != 0) size = s; // 0 means default per spec
                }
            }
            if (offset > 10000)
            {
                result["code"] = StatusCode::BadRequest;
                result["message"] = "pagination.offset must be <= 10000";
                auto res = HttpResponse::newHttpJsonResponse(result);
                res->setStatusCode(k400BadRequest);
                cb(res);
                return;
            }
            if (size > 1000) size = 1000;

            // order: "asc" (default, oldest-first per RPC spec) | "desc" (newest-first).
            bool descOrder = false;
            if (json && (*json).isMember("order"))
            {
                std::string ord = (*json)["order"].asString();
                descOrder = (ord == "desc" || ord == "DESC");
            }

            const unsigned int validForTick = system.tick;

            // Epoch filter / range: if it doesn't intersect system.epoch, short-circuit.
            auto epochInScope = [&]() -> bool
            {
                if (filters.isObject() && filters.isMember("epoch"))
                {
                    try
                    {
                        if (std::stoul(filters["epoch"].asString()) != system.epoch) return false;
                    }
                    catch (...) { return false; }
                }
                if (ranges.isObject() && ranges.isMember("epoch"))
                {
                    if (!rangeMatches(std::to_string(system.epoch), ranges["epoch"])) return false;
                }
                return true;
            };

            // Helper to build the empty-result response.
            auto emitEmpty = [&]()
            {
                Json::Value hits;
                hits["total"] = 0u;
                hits["from"] = offset;
                hits["size"] = 0u;
                result["hits"] = hits;
                result["eventLogs"] = Json::Value(Json::arrayValue);
                result["validForTick"] = validForTick;
                cb(HttpResponse::newHttpJsonResponse(result));
            };

            if (!epochInScope())
            {
                emitEmpty();
                return;
            }

            // Compute the tick scan window.
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
                catch (...) { emitEmpty(); return; }
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
                catch (...) { emitEmpty(); return; }
            }

            // logId window from filter/range.
            unsigned long long logIdLo = 0;
            unsigned long long logIdHi = qLogger::logId;
            if (filters.isObject() && filters.isMember("logId"))
            {
                try
                {
                    unsigned long long l = std::stoull(filters["logId"].asString());
                    logIdLo = logIdHi = l;
                }
                catch (...) { emitEmpty(); return; }
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
                catch (...) { emitEmpty(); return; }
            }

            // logId span [min fromLogId, max fromLogId+length-1] of a tick, or false if no events.
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
                if (any) { lo = mn; hi = mx; }
                return any;
            };

            // Fold the tick window into the logId window where the boundary ticks have events.
            {
                unsigned long long slo = 0, shi = 0;
                if (tickSpan((unsigned int)tickLo, slo, shi)) logIdLo = std::max(logIdLo, slo);
                if (tickSpan((unsigned int)tickHi, slo, shi)) logIdHi = std::min(logIdHi, shi);
            }

            // Optional transactionHash anchor: resolve to a single (tick, txId) so we can skip
            // every other slot. Mirror the scan-resolve pattern from getTransactionByHash.
            bool useHashAnchor = false;
            unsigned int hashAnchorTick = 0;
            unsigned int hashAnchorTxId = 0;
            if (filters.isObject() && filters.isMember("transactionHash"))
            {
                // Require tickNumber: no digest index on mainnet, so scan only that one tick.
                if (!filters.isMember("tickNumber"))
                {
                    result["code"] = StatusCode::BadRequest;
                    result["message"] = "tickNumber filter is required when filtering by transactionHash";
                    auto res = HttpResponse::newHttpJsonResponse(result);
                    res->setStatusCode(k400BadRequest);
                    cb(res);
                    return;
                }
                std::string txHash = filters["transactionHash"].asString();
                if (txHash.length() != 60)
                {
                    result["code"] = StatusCode::BadRequest;
                    result["message"] = fmt::format("invalid id format: converting hash to digest: invalid hash length, expected 60, found {}", txHash.length());
                    auto res = HttpResponse::newHttpJsonResponse(result);
                    res->setStatusCode(k400BadRequest);
                    cb(res);
                    return;
                }
                std::transform(txHash.begin(), txHash.end(), txHash.begin(), ::toupper);
                m256i txDigest = {};
                if (!getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(txHash.c_str()), txDigest.m256i_u8))
                {
                    result["code"] = StatusCode::BadRequest;
                    result["message"] = fmt::format("invalid id format: invalid hash [{}]", txHash);
                    auto res = HttpResponse::newHttpJsonResponse(result);
                    res->setStatusCode(k400BadRequest);
                    cb(res);
                    return;
                }
                bool found = false;
                TickData scanLocal;
                for (unsigned long long t = tickLo; t <= tickHi && !found; t++)
                {
                    TickStorage::tickData.acquireLock();
                    auto td = TickStorage::tickData.getByTickIfNotEmpty((unsigned int)t);
                    if (td) copyMem(&scanLocal, td.get(), sizeof(TickData));
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
                {
                    emitEmpty();
                    return;
                }
                useHashAnchor = true;
                tickLo = tickHi = hashAnchorTick;
                // Narrow the logId window to exactly this tx's logs.
                qLogger::TickBlobInfo tbi;
                qLogger::tx.getTickLogIdInfo(&tbi, hashAnchorTick);
                if (hashAnchorTxId < LOG_TX_PER_TICK &&
                    tbi.fromLogId[hashAnchorTxId] >= 0 && tbi.length[hashAnchorTxId] > 0)
                {
                    unsigned long long f = (unsigned long long)tbi.fromLogId[hashAnchorTxId];
                    logIdLo = std::max(logIdLo, f);
                    logIdHi = std::min(logIdHi, f + (unsigned long long)tbi.length[hashAnchorTxId] - 1);
                }
            }

            if (tickLo > tickHi)
            {
                emitEmpty();
                return;
            }

            // Clamp the logId window to what is committed; then we can seek straight to it.
            if (qLogger::logId == 0) { emitEmpty(); return; }
            logIdHi = std::min(logIdHi, qLogger::logId - 1);
            if (logIdLo > logIdHi) { emitEmpty(); return; }

            // logType filter (cheap pre-check at the inner loop; nothing to do up-front).
            bool haveLogTypeFilter = filters.isObject() && filters.isMember("logType");
            unsigned int wantedLogType = 0;
            if (haveLogTypeFilter)
            {
                try { wantedLogType = (unsigned int)std::stoul(filters["logType"].asString()); }
                catch (...) { emitEmpty(); return; }
            }

            // Iterate by logId, not by raw byte offset: mapLogIdToBufferIndex can
            // contain slots that don't pass verifyLog. getBlobInfo returns {-1,-1}
            // for those so they are skipped cleanly.
            unsigned long long totalMatched = 0;
            const unsigned long long pageEndExclusive = (unsigned long long)offset + (unsigned long long)size;
            std::vector<Json::Value> page;
            page.reserve(size);
            // desc: keep a rolling tail of the newest pageEndExclusive matches (asc order).
            std::deque<Json::Value> dtail;

            // Per-tick caches so we only fetch each tick's data once.
            unsigned int cachedTick = (unsigned int)-1;
            TickData cachedTickData;
            qLogger::TickBlobInfo cachedTbi;
            std::vector<std::string> cachedTxHashes;

            bool stopAll = false;
            for (unsigned long long lid = logIdLo; lid <= logIdHi && !stopAll; lid++)
            {
                qLogger::BlobInfo bi = qLogger::logBuf.getBlobInfo(lid);
                if (bi.startIndex < 0 || bi.length <= 0) continue; // gap from compaction
                unsigned long long entryLen = (unsigned long long)bi.length;
                if (entryLen < LOG_HEADER_SIZE) continue; // sanity
                // Hard cap to header + max 24-bit payload. A corrupt BlobInfo.length would
                // otherwise blow the std::vector allocation below and crash the request handler.
                static constexpr unsigned long long kMaxEntryLen = LOG_HEADER_SIZE + (1ULL << 24);
                if (entryLen > kMaxEntryLen) continue;

                std::vector<unsigned char> blob(entryLen);
                qLogger::logBuffer.getMany(reinterpret_cast<char *>(blob.data()),
                                           bi.startIndex, entryLen);
                const unsigned char *hp = blob.data();
                unsigned int headerEpoch = readUnaligned<unsigned short>(hp, 0);
                unsigned int headerTick = readUnaligned<unsigned int>(hp, 2);
                unsigned int sizeAndType = readUnaligned<unsigned int>(hp, 6);
                unsigned long long headerLogId = readUnaligned<unsigned long long>(hp, 10);
                unsigned int payloadSize = sizeAndType & 0xFFFFFF;
                unsigned char logType = (unsigned char)(sizeAndType >> 24);
                if ((unsigned long long)LOG_HEADER_SIZE + payloadSize > entryLen) continue;

                // Cheap pre-filters.
                if (headerTick < tickLo || headerTick > tickHi) continue;
                if (haveLogTypeFilter && logType != wantedLogType) continue;

                // Refresh per-tick caches when tick changes.
                if (headerTick != cachedTick)
                {
                    cachedTick = headerTick;
                    setMem(&cachedTickData, sizeof(TickData), 0);
                    TickStorage::tickData.acquireLock();
                    auto td = TickStorage::tickData.getByTickIfNotEmpty(headerTick);
                    if (td) copyMem(&cachedTickData, td.get(), sizeof(TickData));
                    TickStorage::tickData.releaseLock();

                    qLogger::tx.getTickLogIdInfo(&cachedTbi, headerTick);

                    cachedTxHashes.assign(NUMBER_OF_TRANSACTIONS_PER_TICK, std::string());
                    if (td)
                    {
                        ts.tickTransactions.acquireLock();
                        auto offsetsArr = ts.tickTransactionOffsets.getByTickInCurrentEpoch(headerTick);
                        for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
                        {
                            if (isZero(cachedTickData.transactionDigests[i]) || !offsetsArr[i])
                                continue;
                            Transaction *txPtr = ts.tickTransactions(offsetsArr[i]);
                            unsigned int txTotalSize = txPtr->totalSize();
                            unsigned char digest[32];
                            KangarooTwelve(txPtr, txTotalSize, digest, 32);
                            cachedTxHashes[i] = identityFromBytes(digest, true);
                        }
                        ts.tickTransactions.releaseLock();
                    }
                }

                // Resolve which txId this event belongs to by searching the tick's
                // fromLogId/length ranges for headerLogId.
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
                {
                    continue;
                }

                const unsigned char *payload = blob.data() + LOG_HEADER_SIZE;
                if (!eventMatchesFilters(logType, headerEpoch, headerTick, headerLogId,
                                         txHashForEvent, categories,
                                         payload, payloadSize,
                                         cachedTickData,
                                         filters, exclude, should, ranges))
                {
                    continue;
                }

                bool wantBuild = descOrder
                    || (totalMatched >= (unsigned long long)offset && totalMatched < pageEndExclusive);
                if (wantBuild)
                {
                    Json::Value je = eventLogToJson(reinterpret_cast<const char *>(blob.data()),
                                                    (unsigned int)entryLen,
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
                // dtail holds the newest min(total, pageEndExclusive) matches in asc order.
                // The desc page [offset, offset+size) maps to asc local [K-offset-size, K-offset).
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
            cb(HttpResponse::newHttpJsonResponse(result));
        }
        catch (const std::exception &e)
        {
            Json::Value result;
            result["code"] = -1;
            result["message"] = std::string("Internal server error: ") + e.what();
            auto res = HttpResponse::newHttpJsonResponse(result);
            res->setStatusCode(k500InternalServerError);
            cb(res);
        }
#endif
    }
};
} // namespace RpcQueryV2