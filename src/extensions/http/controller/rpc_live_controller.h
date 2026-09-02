#pragma once

// Live endpoints as RpcReq->RpcResp router handlers.

#if defined(__linux__) || defined(LITE_WASM_SC)

#include <string>
#include <vector>
#include <memory>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <iterator>
#include "extensions/rpc/rpc_core.h"

// ============================ live (/live/v1/...) ============================
// Mainnet returns errors as HTTP 4xx/5xx, never 200: clients branch on res.ok.
static RpcResp rpcErr(int code, const std::string& message, int status = 400)
{
    Json::Value e;
    e["code"] = code;
    e["message"] = message;
    e["details"] = Json::Value(Json::arrayValue);
    return jsonResp(e, status);
}

RPC_ROUTE("GET", "/live/v1/assets/issuances")
{
    std::string issuerIdentity = req.getParameter("issuerIdentity");
    std::string assetName = req.getParameter("assetName");
    Json::Value result;
    Json::Value assetsArray(Json::arrayValue);
    unsigned long long targetUniverseIndex = -1;
    for (unsigned long long i = 0; i < ASSETS_CAPACITY; i++)
    {
        if (assets[i].varStruct.issuance.type == ISSUANCE)
        {
            auto &asset = assets[i].varStruct.issuance;
            CHAR16 identity[61] = {};
            getIdentity((unsigned char *)&asset.publicKey, identity, false);
            std::string identityStr = wchar_to_string(identity);
            std::string assetNameStr = std::string(asset.name);

            if ((!issuerIdentity.empty() && identityStr != issuerIdentity) || (!assetName.empty() && assetNameStr != assetName))
                continue;

            Json::Value root;
            Json::Value assetJson = HttpUtils::issuanceToJson((HttpUtils::AssetIssuanceType *)&asset);
            root["data"] = assetJson;
            root["tick"] = system.tick;
            root["universeIndex"] = Json::UInt64(i);
            assetsArray.append(root);
            targetUniverseIndex = i;
            break;
        }
    }
    (void)targetUniverseIndex;
    result["assets"] = assetsArray;
    return jsonResp(result);
}

RPC_ROUTE("GET", "/live/v1/assets/issuances/:index")
{
    Json::Value result;
    unsigned long long index = std::stoull(req.getParameter("index"));
    if (index >= ASSETS_CAPACITY)
    {
        return rpcErr(3, "Index out of range", 400);
    }
    if (assets[index].varStruct.issuance.type != ISSUANCE)
    {
        return rpcErr(3, "No asset issuance at the given index", 400);
    }
    auto &asset = assets[index].varStruct.issuance;
    Json::Value assetJson = HttpUtils::issuanceToJson((HttpUtils::AssetIssuanceType *)&asset);
    result["data"] = assetJson;
    result["tick"] = system.tick;
    result["universeIndex"] = Json::UInt64(index);
    return jsonResp(result);
}

RPC_ROUTE("GET", "/live/v1/assets/ownerships")
{
    std::string issuerIdentity = req.getParameter("issuerIdentity");
    std::string assetName = req.getParameter("assetName");
    std::string ownerIdentity = req.getParameter("ownerIdentity");
    int64_t ownershipManagingContract = -1;
    if (req.getParameter("ownershipManagingContract") != "")
        ownershipManagingContract = stoll(req.getParameter("ownershipManagingContract"));
    Json::Value result;
    Json::Value assetsArray(Json::arrayValue);

    m256i issuerPublicKey;
    getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(issuerIdentity.c_str()), issuerPublicKey.m256i_u8);
    auto targetIssuanceIndex = issuanceIndex(issuerPublicKey, HttpUtils::assetNameFromString(assetName.c_str()));
    for (unsigned long long i = 0; i < ASSETS_CAPACITY; i++)
    {
        if (assets[i].varStruct.ownership.type == OWNERSHIP)
        {
            auto &asset = assets[i].varStruct.ownership;
            CHAR16 identity[61] = {};
            getIdentity((unsigned char *)&asset.publicKey, identity, false);
            std::string identityStr = wchar_to_string(identity);

            if ((!ownerIdentity.empty() && identityStr != ownerIdentity) ||
                (ownershipManagingContract >= 0 && asset.managingContractIndex != ownershipManagingContract) ||
                (targetIssuanceIndex >= 0 && asset.issuanceIndex != (unsigned int)targetIssuanceIndex))
                continue;

            Json::Value root;
            Json::Value assetJson = HttpUtils::ownershipToJson((HttpUtils::AssetOwnershipType *)&asset);
            root["tick"] = system.tick;
            root["universeIndex"] = Json::UInt64(i);
            root["data"] = assetJson;
            assetsArray.append(root);
        }
    }
    result["assets"] = assetsArray;
    return jsonResp(result);
}

RPC_ROUTE("GET", "/live/v1/assets/ownerships/:index")
{
    Json::Value result;
    unsigned long long index = std::stoull(req.getParameter("index"));
    if (index >= ASSETS_CAPACITY)
    {
        return rpcErr(3, "Index out of range", 400);
    }
    if (assets[index].varStruct.ownership.type != OWNERSHIP)
    {
        return rpcErr(3, "No asset ownership at the given index", 400);
    }
    auto &asset = assets[index].varStruct.ownership;
    Json::Value assetJson = HttpUtils::ownershipToJson((HttpUtils::AssetOwnershipType *)&asset);
    result["data"] = assetJson;
    result["tick"] = system.tick;
    result["universeIndex"] = Json::UInt64(index);
    return jsonResp(result);
}

RPC_ROUTE("GET", "/live/v1/assets/possessions")
{
    std::string issuerIdentity = req.getParameter("issuerIdentity");
    std::string assetName = req.getParameter("assetName");
    std::string ownerIdentity = req.getParameter("ownerIdentity");
    std::string possessorIdentity = req.getParameter("possessorIdentity");
    int64_t ownershipManagingContract = -1;
    int64_t possessionManagingContract = -1;
    if (req.getParameter("ownershipManagingContract") != "")
        ownershipManagingContract = stoll(req.getParameter("ownershipManagingContract"));
    if (req.getParameter("possessionManagingContract") != "")
        possessionManagingContract = stoll(req.getParameter("possessionManagingContract"));
    Json::Value result;
    Json::Value assetsArray(Json::arrayValue);

    m256i issuerPublicKey;
    getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(issuerIdentity.c_str()), issuerPublicKey.m256i_u8);
    auto targetIssuanceIndex = issuanceIndex(issuerPublicKey, HttpUtils::assetNameFromString(assetName.c_str()));

    for (unsigned long long i = 0; i < ASSETS_CAPACITY; i++)
    {
        if (assets[i].varStruct.possession.type == POSSESSION)
        {
            auto &asset = assets[i].varStruct.possession;
            CHAR16 identity[61] = {};
            getIdentity((unsigned char *)&asset.publicKey, identity, false);
            std::string identityStr = wchar_to_string(identity);
            unsigned short currentOwnershipManagingContractIndex = assets[asset.ownershipIndex].varStruct.ownership.managingContractIndex;
            unsigned int currentIssuanceIndex = assets[asset.ownershipIndex].varStruct.ownership.issuanceIndex;
            if ((!possessorIdentity.empty() && identityStr != possessorIdentity) || (!ownerIdentity.empty() && identityStr != ownerIdentity) ||
                (ownershipManagingContract >= 0 && currentOwnershipManagingContractIndex != ownershipManagingContract) ||
                (possessionManagingContract >= 0 && asset.managingContractIndex != possessionManagingContract) ||
                (targetIssuanceIndex >= 0 && currentIssuanceIndex != targetIssuanceIndex))
                continue;

            Json::Value root;
            Json::Value assetJson = HttpUtils::possessionToJson((HttpUtils::AssetPossessionType *)&asset);
            root["data"] = assetJson;
            root["tick"] = system.tick;
            root["universeIndex"] = Json::UInt64(i);
            assetsArray.append(root);
        }
    }
    result["assets"] = assetsArray;
    return jsonResp(result);
}

RPC_ROUTE("GET", "/live/v1/assets/possessions/:index")
{
    Json::Value result;
    unsigned long long index = std::stoull(req.getParameter("index"));
    if (index >= ASSETS_CAPACITY)
    {
        return rpcErr(3, "Index out of range", 400);
    }
    if (assets[index].varStruct.possession.type != POSSESSION)
    {
        return rpcErr(3, "No asset possession at the given index", 400);
    }
    auto &asset = assets[index].varStruct.possession;
    Json::Value assetJson = HttpUtils::possessionToJson((HttpUtils::AssetPossessionType *)&asset);
    result["data"] = assetJson;
    result["tick"] = system.tick;
    result["universeIndex"] = Json::UInt64(index);
    return jsonResp(result);
}

RPC_ROUTE("GET", "/live/v1/assets/:identity/issued")
{
    std::string identityStr = req.getParameter("identity");
    Json::Value result;
    Json::Value assetsArray(Json::arrayValue);

    m256i identityPublicKey;
    getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(identityStr.c_str()), identityPublicKey.m256i_u8);

    for (unsigned long long i = 0; i < ASSETS_CAPACITY; i++)
    {
        if (assets[i].varStruct.issuance.type == ISSUANCE)
        {
            auto &asset = assets[i].varStruct.issuance;
            if (asset.publicKey != identityPublicKey)
                continue;

            Json::Value root;
            Json::Value assetJson = HttpUtils::issuanceToJson((HttpUtils::AssetIssuanceType *)&asset);
            root["data"] = assetJson;
            Json::Value info(Json::objectValue);
            info["tick"] = system.tick;
            info["universeIndex"] = Json::UInt64(i);
            root["info"] = info;
            assetsArray.append(root);
        }
    }
    result["issuedAssets"] = assetsArray;
    return jsonResp(result);
}

RPC_ROUTE("GET", "/live/v1/assets/:identity/owned")
{
    std::string identityStr = req.getParameter("identity");
    Json::Value result;
    Json::Value assetsArray(Json::arrayValue);

    m256i identityPublicKey;
    getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(identityStr.c_str()), identityPublicKey.m256i_u8);

    for (unsigned long long i = 0; i < ASSETS_CAPACITY; i++)
    {
        if (assets[i].varStruct.ownership.type == OWNERSHIP)
        {
            auto &asset = assets[i].varStruct.ownership;
            auto issuanceAsset = &assets[asset.issuanceIndex].varStruct.issuance;
            if (asset.publicKey != identityPublicKey)
                continue;

            Json::Value root;
            Json::Value assetJson = HttpUtils::ownershipToJson((HttpUtils::AssetOwnershipType *)&asset);
            // Only the per-identity routes carry padding; the universe-index ones do not.
            assetJson["padding"] = (int)asset.padding[0];
            assetJson["issuedAsset"] = HttpUtils::issuanceToJson((HttpUtils::AssetIssuanceType *)issuanceAsset);
            root["data"] = assetJson;
            Json::Value info(Json::objectValue);
            info["tick"] = system.tick;
            info["universeIndex"] = Json::UInt64(i);
            root["info"] = info;
            assetsArray.append(root);
        }
    }
    result["ownedAssets"] = assetsArray;
    return jsonResp(result);
}

RPC_ROUTE("GET", "/live/v1/assets/:identity/possessed")
{
    std::string identityStr = req.getParameter("identity");
    Json::Value result;
    Json::Value assetsArray(Json::arrayValue);

    m256i identityPublicKey;
    getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(identityStr.c_str()), identityPublicKey.m256i_u8);

    for (unsigned long long i = 0; i < ASSETS_CAPACITY; i++)
    {
        if (assets[i].varStruct.possession.type == POSSESSION)
        {
            auto &asset = assets[i].varStruct.possession;
            auto ownershipAsset = &assets[asset.ownershipIndex].varStruct.ownership;
            auto issuanceAsset = &assets[ownershipAsset->issuanceIndex].varStruct.issuance;
            if (asset.publicKey != identityPublicKey)
                continue;

            Json::Value root;
            Json::Value assetJson = HttpUtils::possessionToJson((HttpUtils::AssetPossessionType *)&asset);
            // Per-identity possessions report the owned asset's issuanceIndex, not ownershipIndex.
            assetJson["padding"] = (int)asset.padding[0];
            assetJson.removeMember("ownershipIndex");
            assetJson["issuanceIndex"] = ownershipAsset->issuanceIndex;
            assetJson["ownedAsset"] = HttpUtils::ownershipToJson((HttpUtils::AssetOwnershipType *)ownershipAsset);
            assetJson["ownedAsset"]["padding"] = (int)ownershipAsset->padding[0];
            assetJson["ownedAsset"]["issuedAsset"] = HttpUtils::issuanceToJson((HttpUtils::AssetIssuanceType *)issuanceAsset);
            root["data"] = assetJson;
            Json::Value info(Json::objectValue);
            info["tick"] = system.tick;
            info["universeIndex"] = Json::UInt64(i);
            root["info"] = info;
            assetsArray.append(root);
        }
    }
    result["possessedAssets"] = assetsArray;
    return jsonResp(result);
}

RPC_ROUTE("GET", "/live/v1/balances/:id")
{
    std::string idStr = req.getParameter("id");
    Json::Value result;
    Json::Value balance;
    m256i identityPublicKey;
    getPublicKeyFromIdentity(reinterpret_cast<const unsigned char *>(idStr.c_str()), identityPublicKey.m256i_u8);
    auto spectrumInfo = spectrum[spectrumIndex(identityPublicKey)];
    balance["id"] = idStr;
    balance["balance"] = std::to_string(spectrumInfo.incomingAmount - spectrumInfo.outgoingAmount);
    balance["validForTick"] = system.tick;
    balance["latestIncomingTransferTick"] = spectrumInfo.latestIncomingTransferTick;
    balance["latestOutgoingTransferTick"] = spectrumInfo.latestOutgoingTransferTick;
    balance["incomingAmount"] = std::to_string(spectrumInfo.incomingAmount);
    balance["outgoingAmount"] = std::to_string(spectrumInfo.outgoingAmount);
    balance["numberOfIncomingTransfers"] = spectrumInfo.numberOfIncomingTransfers;
    balance["numberOfOutgoingTransfers"] = spectrumInfo.numberOfOutgoingTransfers;
    result["balance"] = balance;
    return jsonResp(result);
}

static RpcResp rpcLiveTickInfo(const RpcReq& req, const char* wrapperKey)
{
    (void)req;
    Json::Value tickInfo;
    tickInfo["tick"] = system.tick;
    tickInfo["duration"] = 0;
    tickInfo["epoch"] = system.epoch;
    tickInfo["initialTick"] = system.initialTick;

    Json::Value json;
    json[wrapperKey] = tickInfo;
    json["alignedVotes"] = gTickNumberOfComputors;
    json["misalignedVotes"] = gTickTotalNumberOfComputors - gTickNumberOfComputors;
    json["mainAuxStatus"] = mainAuxStatus;
    return jsonResp(json);
}
RPC_ROUTE("GET", "/live/v1/block-height") { return rpcLiveTickInfo(req, "blockHeight"); }
RPC_ROUTE("GET", "/live/v1/tick-info")    { return rpcLiveTickInfo(req, "tickInfo"); }

RPC_ROUTE("POST", "/live/v1/broadcast-transaction")
{
    Json::Value result;
    try
    {
        auto json = rpcJsonBody(req.body);
        if (!json)
        {
            return rpcErr(3, "Invalid JSON", 400);
        }

        std::string txBase64 = (*json)["encodedTransaction"].asString();
        auto txData = base64_decode(txBase64);
        std::cout << "tx data size: " << txData.size() << std::endl;
        Transaction *tx = (Transaction*)txData.data();
        if (!tx->checkValidity())
        {
            return rpcErr(3, "Invalid validity", 400);
        }
        std::cout << "tx json" << HttpUtils::transactionToJson(tx, false) << std::endl;
        {
            unsigned char digest[32];
            KangarooTwelve(txData.data(), tx->totalSize() - SIGNATURE_SIZE, digest, sizeof(digest));
            if (!verify(tx->sourcePublicKey.m256i_u8, digest, tx->signaturePtr()))
            {
                return rpcErr(3, "Invalid signature", 400);
            }
        }

        std::vector<uint8_t> packet(sizeof(RequestResponseHeader) + tx->totalSize());
        RequestResponseHeader *header = (RequestResponseHeader *)packet.data();
        header->setSize2(packet.size());
        header->setDejavu(0);
        header->setType(BROADCAST_TRANSACTION);
        copyMem(packet.data() + sizeof(RequestResponseHeader), txData.data(), packet.size() - sizeof(RequestResponseHeader));
        enqueueResponse(NULL, header);

        uint8_t digest[32];
        KangarooTwelve(packet.data() + sizeof(RequestResponseHeader), tx->totalSize(), digest, 32);
        CHAR16 txHash[61] = {};
        getIdentity(digest, txHash, true);

        result["peersBroadcasted"] = 1;
        result["encodedTransaction"] = txBase64;
        result["transactionId"] = wchar_to_string(txHash);
        return jsonResp(result);
    }
    catch (const std::exception &e)
    {
        return rpcErr(-1, "Exception: " + std::string(e.what()), 500);
    }
}

RPC_ROUTE("GET", "/live/v1/ipos/active")
{
    (void)req;
    Json::Value result;
    Json::Value iposArray(Json::arrayValue);
    for (unsigned int contractIndex = 1; contractIndex < contractCount; ++contractIndex)
    {
        if (system.epoch == contractDescriptions[contractIndex].constructionEpoch - 1)
        {
            Json::Value ipoJson;
            ipoJson["contractIndex"] = contractIndex;
            ipoJson["assetName"] = std::string(contractDescriptions[contractIndex].assetName);
            iposArray.append(ipoJson);
        }
    }
    result["ipos"] = iposArray;
    return jsonResp(result);
}

RPC_ROUTE("POST", "/live/v1/querySmartContract")
{
    Json::Value result;
    try
    {
        auto json = rpcJsonBody(req.body);
        if (!json)
        {
            return rpcErr(3, "Invalid JSON", 400);
        }

        unsigned int contractIndex = (*json)["contractIndex"].asUInt();
        if (contractIndex < 1 || contractIndex >= contractCount)
        {
            return rpcErr(3, "contractIndex out of range", 400);
        }
        unsigned short inputType = (*json)["inputType"].asUInt();
        unsigned short inputSize = (*json)["inputSize"].asUInt();
        std::string requestData = (*json)["requestData"].asString();
        std::vector<uint8_t> inputData = base64_decode(requestData);
        if (inputData.size() != inputSize)
        {
            return rpcErr(3, "Input size mismatch", 400);
        }
        QpiContextUserFunctionCall qpiContext(contractIndex);
        auto errorCode = qpiContext.call(inputType, inputData.data(), inputSize);
        if (errorCode == NoContractError)
        {
            std::vector<uint8_t> responseData(qpiContext.outputSize);
            copyMem(responseData.data(), qpiContext.outputBuffer, qpiContext.outputSize);
            result["responseData"] = base64_encode(responseData);
            return jsonResp(result);
        }
        else
        {
            return rpcErr(-1, "Error calling smart contract function: " + std::to_string(errorCode), 500);
        }
    }
    catch (const std::exception &e)
    {
        return rpcErr(-1, "Exception: " + std::string(e.what()), 500);
    }
}

RPC_ROUTE("GET", "/live/v1/whoami")
{
    (void)req;
    Json::Value result;
    result["backend"] = "core";
    return jsonResp(result);
}

#ifdef LITE_WASM_SC

static std::string rpcHex32(const unsigned char* bytes)
{
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", bytes[i]);
    return std::string(hex, 64);
}

// ============================ wasm contracts (/live/v1/...) ============================

// Reserved slots and their registered entry points.
RPC_ROUTE("GET", "/live/v1/dyn-registry")
{
    (void)req;
    Json::Value json;
    Json::Value contractsJson(Json::arrayValue);
    for (unsigned int i = 0; i < WASM_RESERVED_SLOT_COUNT; i++)
    {
        const Wasm::Runtime::ContractSlot& slot = Wasm::Runtime::contractSlots[i];
        const unsigned int slotIndex = WASM_RESERVED_SLOT_BASE + i;
        Json::Value contractJson;
        contractJson["index"] = slotIndex;
        contractJson["armed"] = slot.armed;
        contractJson["constructed"] = slot.constructed;
        contractJson["version"] = slot.version;
        contractJson["name"] = std::string(slot.name);
        contractJson["codeHash"] = rpcHex32(slot.codeHash);

        Json::Value functionsJson(Json::arrayValue);
        Json::Value proceduresJson(Json::arrayValue);
        if (slot.armed)
        {
            for (unsigned int inputType = 1; inputType <= 65535; inputType++)
            {
                if (contractUserFunctions[slotIndex][inputType])
                {
                    Json::Value entry;
                    entry["inputType"] = inputType;
                    entry["inputSize"] = contractUserFunctionInputSizes[slotIndex][inputType];
                    entry["outputSize"] = contractUserFunctionOutputSizes[slotIndex][inputType];
                    functionsJson.append(entry);
                }
                if (contractUserProcedures[slotIndex][inputType])
                {
                    Json::Value entry;
                    entry["inputType"] = inputType;
                    entry["inputSize"] = contractUserProcedureInputSizes[slotIndex][inputType];
                    entry["outputSize"] = contractUserProcedureOutputSizes[slotIndex][inputType];
                    proceduresJson.append(entry);
                }
            }
        }
        contractJson["functions"] = functionsJson;
        contractJson["procedures"] = proceduresJson;
        contractJson["source"] = slot.sourceH;
        contractJson["lastError"] = Wasm::Runtime::lastTrap(slotIndex);
        contractsJson.append(contractJson);
    }
    json["slotBase"] = (unsigned int)WASM_RESERVED_SLOT_BASE;
    json["slotCount"] = (unsigned int)WASM_RESERVED_SLOT_COUNT;
    json["contracts"] = contractsJson;
    return jsonResp(json);
}

// Progress for the active dynamic-contract upload.
RPC_ROUTE("GET", "/live/v1/dyn-upload")
{
    (void)req;
    Json::Value json;
    const Wasm::Runtime::ModuleUpload& upload = Wasm::Runtime::moduleUpload;
    char sessionId[32];
    snprintf(sessionId, sizeof(sessionId), "%llu", (unsigned long long)upload.sessionId);
    json["active"] = upload.active;
    // JSON cannot represent every 64-bit session ID exactly.
    json["sessionId"] = std::string(sessionId);
    json["totalSize"] = upload.totalSize;
    json["chunkSize"] = 1008u;
    json["chunkCount"] = upload.chunkCount;
    json["receivedCount"] = upload.receivedCount;
    json["complete"] = upload.active && upload.receivedCount == upload.chunkCount;
    json["finalHash"] = rpcHex32(upload.finalHash);

    // Cap the missing-sequence response for large uploads.
    Json::Value missing(Json::arrayValue);
    unsigned int missingCount = 0;
    const unsigned int CAP = 4096;
    if (upload.active)
    {
        for (unsigned int sequence = 0; sequence < upload.chunkCount; sequence++)
        {
            const unsigned int byteIndex = sequence >> 3;
            const unsigned int bit = 1u << (sequence & 7);
            if (!(Wasm::Runtime::receivedChunkBits[byteIndex] & bit))
            {
                if (missingCount < CAP)
                    missing.append(sequence);
                missingCount++;
            }
        }
    }
    json["missing"] = missing;
    json["missingCount"] = missingCount;
    return jsonResp(json);
}

// Current log ID and a small recent sample.
RPC_ROUTE("GET", "/live/v1/log-stats")
{
    (void)req;
    Json::Value json;
    const unsigned long long currentLogId = qLogger::logId;
    json["logId"] = (Json::UInt64)currentLogId;
    Json::Value recentEntries(Json::arrayValue);
#if ENABLED_LOGGING
    const unsigned long long firstLogId = currentLogId > 16 ? currentLogId - 16 : 0;
    for (unsigned long long logId = firstLogId; logId < currentLogId; logId++)
    {
        const auto blobInfo = qLogger::logBuf.getBlobInfo(logId);
        if (blobInfo.startIndex < 0 || blobInfo.length < LOG_HEADER_SIZE)
            continue;

        // Header + up to 32 payload bytes are enough for the sample.
        unsigned char bytes[LOG_HEADER_SIZE + 32];
        const unsigned long long readSize = blobInfo.length < (long long)sizeof(bytes) ? (unsigned long long)blobInfo.length : sizeof(bytes);
        qLogger::logBuf.getMany((char*)bytes, blobInfo.startIndex, readSize);

        const unsigned int sizeAndType = *((unsigned int*)(bytes + 6));
        const unsigned int messageSize = sizeAndType & 0xFFFFFF;
        Json::Value entry;
        entry["logId"] = (Json::UInt64)logId;
        entry["type"] = (unsigned int)(sizeAndType >> 24);
        if (messageSize >= 4 && readSize >= LOG_HEADER_SIZE + 4)
            entry["contractIndex"] = *((unsigned int*)(bytes + 26));

        char payloadHex[65];
        unsigned int capturedSize = messageSize < 32 ? messageSize : 32;
        if (capturedSize > readSize - LOG_HEADER_SIZE)
            capturedSize = (unsigned int)(readSize - LOG_HEADER_SIZE);
        for (unsigned int i = 0; i < capturedSize; i++)
            snprintf(payloadHex + i * 2, 3, "%02x", bytes[26 + i]);
        entry["payloadHex"] = std::string(payloadHex, capturedSize * 2);
        recentEntries.append(entry);
    }
#endif
    json["recent"] = recentEntries;
    return jsonResp(json);
}

// Recent Wasm call traces after the requested sequence.
RPC_ROUTE("GET", "/live/v1/debug-trace")
{
    unsigned long long since = 0;
    unsigned int limit = 64;
    try
    {
        const auto value = req.getParameter("since");
        if (!value.empty())
            since = std::stoull(value);
    }
    catch (...) {}
    try
    {
        const auto value = req.getParameter("limit");
        if (!value.empty())
            limit = (unsigned int)std::stoul(value);
    }
    catch (...) {}
    if (limit == 0 || limit > WASM_TRACE_RING_CAPACITY)
        limit = WASM_TRACE_RING_CAPACITY;

    Json::Value json;
    json["enabled"] = Wasm::Runtime::traceEnabled();
    Json::Value entries(Json::arrayValue);
    for (const auto& trace : Wasm::Runtime::traceSnapshot(since, limit))
    {
        Json::Value entry;
        entry["seq"] = (Json::UInt64)trace.sequence;
        entry["tick"] = trace.tick;
        entry["index"] = trace.contractIndex;
        entry["entry"] = (unsigned int)trace.inputType;
        entry["kind"] = (unsigned int)trace.kind;
        entry["ok"] = trace.ok;
        entry["execNs"] = (Json::UInt64)trace.executionNanoseconds;
        entry["inSize"] = trace.inputSize;
        entry["outSize"] = trace.outputSize;
        entry["stateSize"] = trace.stateSize;
        entry["stateTruncated"] = trace.stateTruncated;
        entry["invocator"] = Wasm::Runtime::hex(&trace.invocator, 32);
        entry["invocationReward"] = (Json::Int64)trace.invocationReward;

        entry["inHex"] = trace.input.empty() ? "" : Wasm::Runtime::hex(trace.input.data(), (unsigned int)trace.input.size());
        entry["outHex"] = trace.output.empty() ? "" : Wasm::Runtime::hex(trace.output.data(), (unsigned int)trace.output.size());

        Json::Value stateDiff(Json::arrayValue);
        for (const auto& run : trace.stateDiff)
        {
            Json::Value diff;
            diff["off"] = run.offset;
            diff["before"] = run.before;
            diff["after"] = run.after;
            stateDiff.append(diff);
        }
        entry["stateDiff"] = stateDiff;
        if (!trace.trap.empty())
            entry["trap"] = trace.trap;

        Json::Value hostCalls(Json::arrayValue);
        for (const auto& hostCall : trace.hostCalls)
        {
            Json::Value call;
            call["name"] = hostCall.name;
            call["detail"] = hostCall.detail;
            hostCalls.append(call);
        }
        entry["hostCalls"] = hostCalls;

        Json::Value logs(Json::arrayValue);
        for (const auto& log : trace.logs)
        {
            Json::Value logEntry;
            logEntry["type"] = (unsigned int)log.type;
            logEntry["size"] = log.size;
            logEntry["hex"] = log.hex;
            logs.append(logEntry);
        }
        entry["logs"] = logs;

        Json::Value cheats(Json::arrayValue);
        for (const auto& cheat : trace.cheats)
        {
            Json::Value cheatEntry;
            cheatEntry["id"] = cheat.id;
            cheatEntry["part"] = (unsigned int)cheat.part;
            cheatEntry["size"] = cheat.size;
            cheatEntry["value"] = (Json::UInt64)cheat.value;
            cheatEntry["hex"] = cheat.hex;
            cheats.append(cheatEntry);
        }
        entry["cheats"] = cheats;
        entries.append(entry);
    }
    json["entries"] = entries;
    return jsonResp(json);
}

// Toggle Wasm trace capture.
RPC_ROUTE("GET", "/live/v1/dev/debug")
{
    auto on = req.getParameter("on");
    if (!on.empty())
        Wasm::Runtime::setTraceEnabled(on == "1" || on == "true");
    Json::Value json;
    json["enabled"] = Wasm::Runtime::traceEnabled();
    return jsonResp(json);
}

// Drop all captured traces.
RPC_ROUTE("GET", "/live/v1/dev/debug-clear")
{
    (void)req;
    Wasm::Runtime::clearTrace();
    Json::Value json;
    json["cleared"] = true;
    return jsonResp(json);
}

// Resolve a state-read/digest slot: wasm slots use the engine's effective size, native ones the
// declared stateSize.
static bool rpcResolveContractSlot(int slotIndex, unsigned long long& stateSize)
{
    const int localIndex = slotIndex - (int)WASM_RESERVED_SLOT_BASE;
    bool validSlot;
    if (slotIndex >= (int)WASM_RESERVED_SLOT_BASE)
    {
        validSlot = localIndex >= 0 && localIndex < (int)WASM_RESERVED_SLOT_COUNT && Wasm::Runtime::isContractLoaded(slotIndex) && contractStates[slotIndex];
        stateSize = validSlot ? Wasm::Runtime::effectiveStateSize(slotIndex, contractDescriptions[slotIndex].stateSize) : 0;
    }
    else
    {
        validSlot = slotIndex >= 1 && slotIndex < (int)contractCount && contractStates[slotIndex];
        stateSize = validSlot ? contractDescriptions[slotIndex].stateSize : 0;
    }
    return validSlot;
}

// Bounded best-effort snapshot of contract state bytes.
RPC_ROUTE("GET", "/live/v1/dev/state-read")
{
    Json::Value json;
    const int slotIndex = std::atoi(req.getParameter("slot").c_str());
    unsigned long long offset = strtoull(req.getParameter("off").c_str(), nullptr, 10);
    unsigned long long length = strtoull(req.getParameter("len").c_str(), nullptr, 10);
    unsigned long long stateSize = 0;
    if (!rpcResolveContractSlot(slotIndex, stateSize))
    {
        json["error"] = "bad slot";
        return jsonResp(json);
    }

    if (offset > stateSize)
        offset = stateSize;
    if (length > 262144ull)
        length = 262144ull;
    if (offset + length > stateSize)
        length = stateSize - offset;

    const unsigned char* state = contractStates[slotIndex];
    static const char* hexDigits = "0123456789abcdef";
    std::string hex;
    hex.reserve((size_t)length * 2);
    for (unsigned long long i = 0; i < length; i++)
    {
        hex += hexDigits[state[offset + i] >> 4];
        hex += hexDigits[state[offset + i] & 15];
    }
    json["off"] = (Json::UInt64)offset;
    json["len"] = (Json::UInt64)length;
    json["stateSize"] = (Json::UInt64)stateSize;
    json["hex"] = hex;
    return jsonResp(json);
}

// Canonical K12 digest of a contract's effective state.
RPC_ROUTE("GET", "/live/v1/dev/contract-digest")
{
    Json::Value json;
    const int slotIndex = std::atoi(req.getParameter("slot").c_str());
    unsigned long long stateSize = 0;
    if (!rpcResolveContractSlot(slotIndex, stateSize))
    {
        json["error"] = "bad slot";
        return jsonResp(json);
    }

    unsigned char digest[32];
    KangarooTwelve(contractStates[slotIndex], (unsigned int)stateSize, digest, 32);
    json["slot"] = slotIndex;
    json["stateSize"] = (Json::UInt64)stateSize;
    json["digest"] = rpcHex32(digest);
    return jsonResp(json);
}

#if ADDON_TX_STATUS_REQUEST
// Exact inclusion and processing status for one transaction.
RPC_ROUTE("GET", "/live/v1/tx-status/:tick/:tx")
{
    Json::Value result;
    const std::string transactionId = req.getParameter("tx");
    const unsigned int tick = (unsigned int)strtoul(req.getParameter("tick").c_str(), nullptr, 10);
    result["tick"] = tick;
    result["currentTick"] = system.tick;
    result["txId"] = transactionId;

    std::string uppercaseId = transactionId;
    for (auto& character : uppercaseId)
    {
        if (character >= 'a' && character <= 'z')
            character -= 32;
    }
    m256i targetDigest;
    getPublicKeyFromIdentity(reinterpret_cast<const unsigned char*>(uppercaseId.c_str()), targetDigest.m256i_u8);

    // Search the current or retained previous epoch.
    bool inRange = false;
    int tickIndex = 0;
    if (tick >= txStatusData.confirmedTxCurrentEpochBeginTick && tick < txStatusData.confirmedTxCurrentEpochBeginTick + MAX_NUMBER_OF_TICKS_PER_EPOCH)
    {
        tickIndex = tick - txStatusData.confirmedTxCurrentEpochBeginTick;
        inRange = true;
    }
    else if (txStatusData.confirmedTxPreviousEpochBeginTick != 0 && tick >= txStatusData.confirmedTxPreviousEpochBeginTick &&
             tick < txStatusData.confirmedTxCurrentEpochBeginTick)
    {
        tickIndex = tick - txStatusData.confirmedTxPreviousEpochBeginTick + MAX_NUMBER_OF_TICKS_PER_EPOCH;
        inRange = true;
    }

    bool found = false;
    bool moneyFlew = false;
    if (inRange)
    {
        ACQUIRE(confirmedTxLock);
        const unsigned int firstIndex = txStatusData.tickTxIndexStart[tickIndex];
        const unsigned int transactionCount = txStatusData.tickTxCounter[tickIndex];
        for (unsigned int i = 0; i < transactionCount; i++)
        {
            const ConfirmedTx& transaction = confirmedTx[firstIndex + i];
            if (transaction.digest == targetDigest)
            {
                found = true;
                moneyFlew = transaction.moneyFlew != 0;
                break;
            }
        }
        RELEASE(confirmedTxLock);
    }
    result["found"] = found;
    result["moneyFlew"] = moneyFlew;
    result["processed"] = system.tick > tick;
    return jsonResp(result);
}
#endif // ADDON_TX_STATUS_REQUEST

#if defined(TESTNET)

// ============================ testnet dev (/live/v1/dev/...) ============================

// Pre-funded testnet seed.
RPC_ROUTE("GET", "/live/v1/dev/funded-seed")
{
    (void)req;
    Json::Value json;
    if (std::size(broadcastedComputorSeeds) > 0)
        json["seed"] = std::string((const char*)broadcastedComputorSeeds[0]);
    return jsonResp(json);
}

// Requested number of pre-funded testnet seeds.
RPC_ROUTE("GET", "/live/v1/dev/funded-seeds")
{
    const unsigned int total = (unsigned int)std::size(broadcastedComputorSeeds);
    unsigned int limit = 32;
    try
    {
        const auto value = req.getParameter("limit");
        if (!value.empty())
            limit = (unsigned int)std::stoul(value);
    }
    catch (...) {}
    if (limit == 0 || limit > total)
        limit = total;

    Json::Value json;
    Json::Value seeds(Json::arrayValue);
    for (unsigned int i = 0; i < limit; i++)
        seeds.append(std::string((const char*)broadcastedComputorSeeds[i]));
    json["seeds"] = seeds;
    json["count"] = total;
    return jsonResp(json);
}

// Store node-local source used for inter-contract type resolution.
RPC_ROUTE("POST", "/live/v1/dev/contract-source")
{
    Json::Value json;
    const int slotIndex = std::atoi(req.getParameter("slot").c_str());
    const int localIndex = slotIndex - (int)WASM_RESERVED_SLOT_BASE;
    if (localIndex < 0 || localIndex >= (int)WASM_RESERVED_SLOT_COUNT)
    {
        json["ok"] = false;
        json["error"] = "bad slot";
        return jsonResp(json);
    }
    Wasm::Runtime::contractSlots[localIndex].sourceH = req.body;
    json["ok"] = true;
    json["slot"] = slotIndex;
    json["len"] = (Json::UInt)Wasm::Runtime::contractSlots[localIndex].sourceH.size();
    return jsonResp(json);
}

static unsigned int liteDevEpochLastTick()
{
    return system.initialTick + (unsigned int)TESTNET_EPOCH_DURATION - 1;
}

// Fast-forward with a timeout, then restore the configured tick delay.
static unsigned int liteDevFastForwardTo(unsigned int target, unsigned int timeoutMs)
{
    if (system.tick >= target)
        return system.tick;

    const unsigned long long savedTickDelay = tickDelay;
    tickDelay = 0;
    const auto startTime = std::chrono::steady_clock::now();
    while (system.tick < target)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime);
        if (elapsed.count() > (long long)timeoutMs)
            break;
    }
    tickDelay = savedTickDelay;
    return system.tick;
}

// Current testnet epoch window.
RPC_ROUTE("GET", "/live/v1/dev/epoch-info")
{
    (void)req;
    const unsigned int lastTick = liteDevEpochLastTick();
    Json::Value json;
    json["epoch"] = (unsigned int)system.epoch;
    json["tick"] = system.tick;
    json["initialTick"] = system.initialTick;
    json["epochLastTick"] = lastTick;
    json["ticksLeft"] = system.tick <= lastTick ? lastTick - system.tick : 0u;
    json["duration"] = (unsigned int)TESTNET_EPOCH_DURATION;
    return jsonResp(json);
}

// Advance without crossing the current epoch boundary.
RPC_ROUTE("GET", "/live/v1/dev/advance-tick")
{
    unsigned int requestedTicks = 1;
    try
    {
        const auto value = req.getParameter("n");
        if (!value.empty())
            requestedTicks = (unsigned int)std::stoul(value);
    }
    catch (...) {}
    if (requestedTicks == 0)
        requestedTicks = 1;

    const unsigned int startTick = system.tick;
    const unsigned int lastTick = liteDevEpochLastTick();
    unsigned int targetTick = startTick + requestedTicks;
    const bool capped = targetTick > lastTick;
    if (capped)
        targetTick = lastTick;
    const unsigned int reachedTick = liteDevFastForwardTo(targetTick, 12000);

    Json::Value json;
    json["from"] = startTick;
    json["requested"] = requestedTicks;
    json["target"] = targetTick;
    json["reached"] = reachedTick;
    json["epochLastTick"] = lastTick;
    json["cappedAtEpochEnd"] = capped;
    return jsonResp(json);
}

// Advance to a safe gap before the current epoch boundary.
RPC_ROUTE("GET", "/live/v1/dev/advance-to-last")
{
    unsigned int gap = 3;
    try
    {
        const auto value = req.getParameter("gap");
        if (!value.empty())
            gap = (unsigned int)std::stoul(value);
    }
    catch (...) {}

    const unsigned int startTick = system.tick;
    const unsigned int lastTick = liteDevEpochLastTick();
    const unsigned int targetTick = lastTick > gap ? lastTick - gap : lastTick;
    const unsigned int reachedTick = liteDevFastForwardTo(targetTick, 12000);

    Json::Value json;
    json["from"] = startTick;
    json["target"] = targetTick;
    json["reached"] = reachedTick;
    json["epochLastTick"] = lastTick;
    json["epoch"] = (unsigned int)system.epoch;
    return jsonResp(json);
}

// Advance through the node's normal epoch transition.
RPC_ROUTE("GET", "/live/v1/dev/advance-epoch")
{
    (void)req;
    const unsigned int startEpoch = (unsigned int)system.epoch;
    const unsigned int startTick = system.tick;
    const unsigned long long savedTickDelay = tickDelay;
    tickDelay = 0;
    forceSwitchEpoch = true;
    const auto startTime = std::chrono::steady_clock::now();
    while ((unsigned int)system.epoch == startEpoch)
    {
        // Keep the transition moving through its clean-memory wait.
        epochTransitionCleanMemoryFlag = 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime);
        if (elapsed.count() > 25000)
            break;
    }
    tickDelay = savedTickDelay;
    if ((unsigned int)system.epoch == startEpoch)
        forceSwitchEpoch = false;

    Json::Value json;
    json["fromEpoch"] = startEpoch;
    json["toEpoch"] = (unsigned int)system.epoch;
    json["fromTick"] = startTick;
    json["tick"] = system.tick;
    json["initialTick"] = system.initialTick;
    json["switched"] = (unsigned int)system.epoch != startEpoch;
    return jsonResp(json);
}

#endif // TESTNET
#endif // LITE_WASM_SC

#endif // __linux__ || LITE_WASM_SC
