#pragma once

// Generic/admin endpoints as RpcReq->RpcResp router handlers.
// Included late in qubic.cpp so node globals are visible.

#if defined(__linux__) || defined(LITE_WASM_SC)

#include <string>
#include <filesystem>
#include <memory>
#include <system_error>
#include "extensions/rpc/rpc_core.h"
#include "extensions/http/utils.h"
#include "extensions/tick_bench.h"
#include "extensions/tx_stats.h"

// Parse a JSON request body; null on failure (mirrors drogon req->getJsonObject()).
static std::shared_ptr<Json::Value> rpcJsonBody(const std::string& body)
{
    auto j = std::make_shared<Json::Value>();
    Json::CharReaderBuilder rb;
    std::string err;
    const std::unique_ptr<Json::CharReader> rd(rb.newCharReader());
    if (!rd->parse(body.data(), body.data() + body.size(), j.get(), &err))
    {
        return nullptr;
    }
    return j;
}

// Was in http.h; the RPC routes reference it for passcode-gated endpoints.
extern unsigned long long httpPasscodes[4];
void logColorToScreen(std::string type, std::string msg);

// ---- auth: passcode = p0-p1-p2-p3; unset (all zero) disables the gated routes ----
static bool rpcPasscodeOk(const RpcReq& req)
{
    if (!httpPasscodes[0] && !httpPasscodes[1] && !httpPasscodes[2] && !httpPasscodes[3])
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            logColorToScreen("WARN", "Admin HTTP routes disabled: start with --http-passcode to enable them");
        }
        return false;
    }
    std::string correct = std::to_string(httpPasscodes[0]) + "-" +
                          std::to_string(httpPasscodes[1]) + "-" +
                          std::to_string(httpPasscodes[2]) + "-" +
                          std::to_string(httpPasscodes[3]);
    return req.getParameter("passcode") == correct;
}
static RpcResp rpcUnauthorized()
{
    return { 401, "text/plain", "Unauthorized: Invalid passcode", "", "" };
}

// Build a downloadable file response, zipping into .qubic-tmp/ when zip=true.
// kind is "spectrum" or "universe"; mirrors the two http.h file handlers.
static RpcResp rpcEpochFile(const RpcReq& req, const std::string& kind)
{
    static const std::string hiddenFolder = ".qubic-tmp";
    bool isZip = req.getParameter("zip") == "true";
    std::string base = kind + "." + std::to_string(system.epoch);
    std::string path = isZip ? (hiddenFolder + "/" + kind + ".zip") : base;

    if (isZip && !std::filesystem::exists(path))
    {
        if (!std::filesystem::exists(hiddenFolder + "/"))
            std::filesystem::create_directory(hiddenFolder);
        std::string command = "zip -j " + path + " " + base;
        if (exec(command.c_str()) != 0)
        {
            Json::Value e;
            e["error"] = "Failed to create zip file";
            return jsonResp(e, 500);
        }
    }

    std::error_code ec;
    std::string abs = std::filesystem::absolute(path, ec).string();
    std::string fileName = isZip ? (kind + ".zip") : base;
    return fileResp(ec ? path : abs, fileName);
}

RPC_ROUTE("GET", "/")
{
    (void)req;
    return { 200, "text/plain", "Hello, World!2", "", "" };
}

RPC_ROUTE("GET", "/tick-info")
{
    Json::Value json;
    json["epoch"] = system.epoch;
    json["tick"] = system.tick;
    json["initialTick"] = system.initialTick;
    json["alignedVotes"] = gTickNumberOfComputors;
    json["misalignedVotes"] = gTickTotalNumberOfComputors - gTickNumberOfComputors;
    json["mainAuxStatus"] = mainAuxStatus;
    json["duration"] = 0;
    json["isSavingSnapshot"] = (bool)persistingNodeStateTickProcWaiting;
    json["extraInfo"] = getCheckInData(req.getParameter("challenge"));
    return jsonResp(json);
}

RPC_ROUTE("GET", "/running-ids")
{
    (void)req;
    Json::Value json;
    Json::Value idsJson(Json::arrayValue);
    for (int i = 0; i < computorSeedsCount; i++)
    {
        CHAR16 id[61] = {};
        m256i publicKey = {};
        m256i privateKey = {};
        m256i subseed = {};
        bool isOk = getSubseed(reinterpret_cast<const unsigned char *>(computorSeeds[i]), subseed.m256i_u8);
        if (!isOk)
            continue;
        getPrivateKey(subseed.m256i_u8, privateKey.m256i_u8);
        getPublicKey(privateKey.m256i_u8, publicKey.m256i_u8);
        getIdentity(publicKey.m256i_u8, id, false);
        if (publicKey != computorPublicKeys[i])
            continue;
        idsJson.append(wchar_to_string(id));
    }
    json["runningIds"] = idsJson;
    return jsonResp(json);
}

RPC_ROUTE("GET", "/latest-created-tick-info")
{
    (void)req;
    Json::Value json;
    CHAR16 id[61] = {};
    getIdentity((const unsigned char*)&latestCreatedTickInfo.id, id, false);
    json["tick"] = latestCreatedTickInfo.tick;
    json["epoch"] = latestCreatedTickInfo.epoch;
    json["numberOfTxs"] = latestCreatedTickInfo.numberOfTxs;
    json["id"] = wchar_to_string(id);
    return jsonResp(json);
}

RPC_ROUTE("GET", "/solutions")
{
    (void)req;
    Json::Value json(Json::arrayValue);
    for (unsigned int i = 0; i < system.numberOfSolutions; i++)
    {
        Json::Value solutionJson;
        solutionJson["computorPublicKey"] = byteToHex((unsigned char *)&system.solutions[i].computorPublicKey, sizeof(m256i));
        solutionJson["miningSeed"] = byteToHex((unsigned char *)&system.solutions[i].miningSeed, sizeof(m256i));
        solutionJson["nonce"] = byteToHex((unsigned char *)&system.solutions[i].nonce, sizeof(m256i));
        json.append(solutionJson);
    }
    return jsonResp(json);
}

RPC_ROUTE("GET", "/solution-publish-ticks")
{
    (void)req;
    Json::Value json(Json::arrayValue);
    for (unsigned int i = 0; i < system.numberOfSolutions; i++)
    {
        Json::Value jsonObject;
        jsonObject["solutionIndex"] = i;
        jsonObject["publishTick"] = solutionPublicationTicks[i];
        json.append(jsonObject);
    }
    return jsonResp(json);
}

RPC_ROUTE("GET", "/request-save-snapshot")
{
    if (!rpcPasscodeOk(req)) return rpcUnauthorized();
    requestPersistingNodeState = 1;
    Json::Value json;
    json["status"] = "ok";
    return jsonResp(json);
}

RPC_ROUTE("GET", "/spectrum")
{
    if (!rpcPasscodeOk(req)) return rpcUnauthorized();
    return rpcEpochFile(req, "spectrum");
}

RPC_ROUTE("GET", "/universe")
{
    if (!rpcPasscodeOk(req)) return rpcUnauthorized();
    return rpcEpochFile(req, "universe");
}

RPC_ROUTE("GET", "/shutdown")
{
    if (!rpcPasscodeOk(req)) return rpcUnauthorized();
    requestGracefulShutdown();
    Json::Value json;
    json["status"] = "ok";
    return jsonResp(json);
}

RPC_ROUTE("GET", "/set-max-inbound")
{
    if (!rpcPasscodeOk(req)) return rpcUnauthorized();
    int n = std::stoi(req.getParameter("n"));
    if (n < 0) n = 0;
    if (n > NUMBER_OF_INCOMING_CONNECTIONS) n = NUMBER_OF_INCOMING_CONNECTIONS;
    maxInboundAccepts = n;
    Json::Value json;
    json["status"] = "ok";
    json["maxInboundAccepts"] = maxInboundAccepts;
    return jsonResp(json);
}

RPC_ROUTE("GET", "/spam")
{
    if (!rpcPasscodeOk(req)) return rpcUnauthorized();
    char enable = 0;
    std::string enableStr = req.getParameter("enable");
    bool withRpc = req.getParameter("withRpc") == "true" || req.getParameter("withRpc") == "1";
    enable = static_cast<char>(std::stoi(enableStr));
    if (enable > 2) enable = 2;
    enableBadBoySpammer = enable;
    spammerWithRpc = withRpc;
    Json::Value json;
    json["status"] = "ok";
    json["spamEnabled"] = enableBadBoySpammer;
    json["withRpc"] = spammerWithRpc;
    return jsonResp(json);
}

RPC_ROUTE("GET", "/set-operator-seed")
{
    if (!rpcPasscodeOk(req)) return rpcUnauthorized();
    std::string seed = req.getParameter("seed");
    if (seed.length() != 55)
    {
        RpcResp r{ 400, "text/plain", "Invalid seed length", "", "" };
        return r;
    }
    mySeed = seed;
    CHAR16 id[61] = {};
    m256i publicKey = {};
    m256i privateKey = {};
    m256i subseed = {};
    bool isOk = getSubseed(reinterpret_cast<const unsigned char *>(mySeed.c_str()), subseed.m256i_u8);
    if (!isOk)
    {
        RpcResp r{ 400, "text/plain", "Invalid seed format", "", "" };
        return r;
    }
    getPrivateKey(subseed.m256i_u8, privateKey.m256i_u8);
    getPublicKey(privateKey.m256i_u8, publicKey.m256i_u8);
    getIdentity(publicKey.m256i_u8, id, false);
    myOperatorId = wchar_to_string(id);
    mySubseed = subseed;
    myPublicKey = publicKey;
    Json::Value json;
    json["status"] = "ok";
    json["newId"] = myOperatorId;
    return jsonResp(json);
}

#include "extensions/http/controller/explorer_controller.h"
#include "extensions/http/controller/rpc_stats_controller.h"
#include "extensions/http/controller/rpc_live_controller.h"
#include "extensions/http/controller/rpc_queryv2_controller.h"

#endif // __linux__ || LITE_WASM_SC
