#pragma once

#include "platform/concurrency.h"
#include <drogon/drogon.h>   // fetch() below takes drogon types; do not rely on include order
#include <trantor/net/EventLoopThread.h>

class HttpUtils
{
public:
    struct AssetIssuanceType
    {
        m256i publicKey{};
        unsigned char type{};
        char name[7]{}; // Capital letters + digits
        char numberOfDecimalPlaces{};
        char unitOfMeasurement[7]{}; // Powers of the corresponding SI base units going in alphabetical order
    };

    struct AssetOwnershipType
    {
        m256i publicKey{};
        unsigned char type{};
        char padding[1]{};
        unsigned short managingContractIndex{};
        unsigned int issuanceIndex{};
        long long numberOfShares{};
    };

    struct AssetPossessionType
    {
        m256i publicKey{};
        unsigned char type{};
        char padding[1]{};
        unsigned short managingContractIndex{};
        unsigned int ownershipIndex{};
        long long numberOfShares{};
    };

    static unsigned long long assetNameFromString(const char *assetName)
    {
        size_t n = strlen(assetName);
        unsigned long long integer = 0;
        copyMem(&integer, assetName, n);
        return integer;
    }

    static std::string assetNameFromInt64(unsigned long long assetName)
    {
        char buffer[8];
        copyMem(&buffer, &assetName, sizeof(assetName));
        buffer[7] = 0;
        return buffer;
    }

    static Json::Value issuanceToJson(const AssetIssuanceType *asset)
    {
        Json::Value assetJson;
        Json::Value unitOfMeasurementArray(Json::arrayValue);
        CHAR16 identity[61] = {};
        getIdentity((unsigned char *)&asset->publicKey, identity, false);
        assetJson["issuerIdentity"] = wchar_to_string(identity);
        assetJson["name"] = std::string(asset->name);
        assetJson["type"] = ASSET_ISSUANCE;
        assetJson["numberOfDecimalPlaces"] = asset->numberOfDecimalPlaces;
        // char[7]: the old i < 8 ran past the struct.
        for (size_t i = 0; i < sizeof(asset->unitOfMeasurement); i++)
        {
            unitOfMeasurementArray.append(asset->unitOfMeasurement[i]);
        }
        assetJson["unitOfMeasurement"] = unitOfMeasurementArray;
        return assetJson;
    }

    static Json::Value ownershipToJson(const AssetOwnershipType *asset)
    {
        Json::Value assetJson;
        CHAR16 identity[61] = {};
        getIdentity((unsigned char *)&asset->publicKey, identity, false);
        assetJson["ownerIdentity"] = wchar_to_string(identity);
        assetJson["type"] = OWNERSHIP;
        assetJson["managingContractIndex"] = asset->managingContractIndex;
        assetJson["issuanceIndex"] = asset->issuanceIndex;
        assetJson["numberOfUnits"] = std::to_string(asset->numberOfShares);
        return assetJson;
    }

    static Json::Value possessionToJson(const AssetPossessionType *asset)
    {
        Json::Value assetJson;
        CHAR16 identity[61] = {};
        getIdentity((unsigned char *)&asset->publicKey, identity, false);
        assetJson["possessorIdentity"] = wchar_to_string(identity);
        assetJson["type"] = POSSESSION;
        assetJson["managingContractIndex"] = asset->managingContractIndex;
        assetJson["ownershipIndex"] = asset->ownershipIndex;
        assetJson["numberOfUnits"] = std::to_string(asset->numberOfShares);
        return assetJson;
    }

    static Json::Value transactionToJson(Transaction *tx, bool forceToBeProcessed = true)
    {
        Json::Value jsonObject;

        TickData *tickData = new TickData();
        if (forceToBeProcessed)
        {
            PinScope _pinScope; // callers loop over many ticks; hold the tickData page pin only while copying it out
            TickStorage::tickData.acquireLock();
            TickData *tmptickData = TickStorage::tickData.getByTickIfNotEmpty(tx->tick);
            if (!tmptickData)
            {
                TickStorage::tickData.releaseLock();
                delete tickData;
                return jsonObject;
            }
            copyMem(tickData, tmptickData, sizeof(TickData));
            TickStorage::tickData.releaseLock();
        }

        CHAR16 txHashStr[61] = {0};
        uint8_t digest[32];
        KangarooTwelve(tx,
                       tx->totalSize(),
                       digest,
                       32); // recompute digest for txhash
        getIdentity(digest, txHashStr, true);
        CHAR16 humanId[61] = {0};
        jsonObject["hash"] = wchar_to_string(txHashStr);
        // int64 as string, per protobuf JSON: a number loses precision past 2^53.
        jsonObject["amount"] = std::to_string(tx->amount);
        getIdentity((const unsigned char *)&tx->sourcePublicKey, humanId, false);
        jsonObject["source"] = wchar_to_string(humanId);
        getIdentity((const unsigned char *)&tx->destinationPublicKey, humanId, false);
        jsonObject["destination"] = wchar_to_string(humanId);
        jsonObject["tickNumber"] = tx->tick;
        jsonObject["timestamp"] = forceToBeProcessed ? HttpUtils::formatTimestamp(
            tickData->millisecond,
            tickData->second,
            tickData->minute,
            tickData->hour,
            tickData->day,
            tickData->month,
            tickData->year) : "0";
        jsonObject["inputType"] = tx->inputType;
        jsonObject["inputSize"] = tx->inputSize;
        jsonObject["inputData"] = base64_encode(tx->inputPtr(), tx->inputSize);
        jsonObject["signature"] = base64_encode(tx->signaturePtr(), SIGNATURE_SIZE);
        jsonObject["moneyFlew"] = tx->amount > 0;
        delete tickData;
        return jsonObject;
    }

    static std::string formatTimestamp(
       unsigned short millisecond,
       unsigned char second,
       unsigned char minute,
       unsigned char hour,
       unsigned char day,
       unsigned char month,
       unsigned char year // 2000 + year
   ) {
        std::tm tm{};
        tm.tm_year = (2000 + year) - 1900; // years since 1900
        tm.tm_mon  = month - 1;            // months since January [0–11]
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min  = minute;
        tm.tm_sec  = second;
        tm.tm_isdst = 0;                   // no daylight saving

        // Convert to Unix time (UTC)
#if defined(_WIN32)
        time_t unixTime = _mkgmtime(&tm);  // Windows UTC
#else
        time_t unixTime = timegm(&tm);     // POSIX UTC
#endif

        return std::to_string(unixTime);
    }

    static TickData *findTickDataFromTxHash(m256i &hash)
    {
        TickData localTickData;
        for (unsigned int tick = system.initialTick; tick <= system.tick; tick++)
        {
            PinScope _pinScope; // release this tick's swap-page pin each iteration (full-epoch scan)
            TickStorage::tickData.acquireLock();
            TickData *tickData = TickStorage::tickData.getByTickIfNotEmpty(tick);
            if (tickData)
            {
                copyMem(&localTickData, tickData, sizeof(TickData));
            }
            TickStorage::tickData.releaseLock();
            if (!tickData)
            {
                continue;
            }

            for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
            {
                if (localTickData.transactionDigests[i] == hash)
                {
                    // found
                    TickData *result = new TickData();
                    copyMem(result, &localTickData, sizeof(TickData));
                    return result;
                }
            }
        }

        return nullptr;
    }

    static void fetch(const std::string &url, const std::string &path, const drogon::HttpMethod method, const Json::Value &body, const std::map<std::string, std::string> &headers = {}, std::function<void(drogon::ReqResult &result, const drogon::HttpResponsePtr &resp)> callback = nullptr)
    {
        struct ForkCensusScope
        {
            ForkCensusScope() { forkCensusEnter("HttpUtils::fetch"); }
            ~ForkCensusScope() { forkCensusLeave(); }
        } forkCensusScope;

        trantor::EventLoopThread loopThread("qubic-http-client");
        loopThread.run();
        auto client = drogon::HttpClient::newHttpClient(url, loopThread.getLoop());

        // Serialize per-call: newHttpJsonRequest() shares a process-wide static StreamWriterBuilder that this background thread races against drogon's loop threads (SIGSEGV in newStreamWriter()).
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(method);
        req->setPath(path);
        if (!body.isNull())
        {
            Json::StreamWriterBuilder jsonWriter;
            jsonWriter["commentStyle"] = "None";
            jsonWriter["indentation"] = "";
            req->setBody(Json::writeString(jsonWriter, body));
            req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        }

        for (const auto& header : headers) {
            req->addHeader(header.first, header.second);
        }

        auto [result, resp] = client->sendRequest(req, 30.0);
        if (callback != nullptr)
        {
            callback(result, resp);
        }
    }
};
