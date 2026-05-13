#pragma once

// =====================================================================
// Lite-node check-in over P2P (Qubic TCP).
//
// Mirrors the HTTP /tick-info?challenge=... contract that getCheckInData()
// in overload.h serves, but as a fixed-size binary request/respond pair on
// the existing peer port (21841 mainnet, 31841 testnet). Lets the HTTP
// interface stay disabled while the upstream lite-node validator can still
// verify liveness via the standard Qubic P2P protocol.
//
// Wiring is intentionally minimal so upstream-core merges stay clean:
//
//   1. One #include of this file in qubic.cpp (after extensions/overload.h
//      so myOperatorId/mySubseed/myPublicKey/nodeAlias are visible).
//   2. One case block in the request-dispatch switch:
//
//          case LiteCheckin::RequestLiteCheckin::type():
//              LiteCheckin::processRequest(peer, header);
//              break;
//
// Type numbers are pinned to the high end of the currently-unallocated
// range. Upstream NetworkMessageType uses 0..202 and 255; 250/251 leaves
// plenty of headroom on either side for future upstream allocations.
// =====================================================================

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>

namespace LiteCheckin
{

constexpr unsigned char REQUEST_TYPE = 230;
constexpr unsigned char RESPOND_TYPE = 231;

#pragma pack(push, 1)

struct RequestLiteCheckin
{
    static constexpr unsigned char type() { return REQUEST_TYPE; }

    unsigned char challenge[16];   // 16 random bytes from the checker
};
static_assert(sizeof(RequestLiteCheckin) == 16,
              "RequestLiteCheckin layout drifted");

struct RespondLiteCheckin
{
    static constexpr unsigned char type() { return RESPOND_TYPE; }

    unsigned char      publicKey[32];  // 32-byte operator public key
    unsigned char      alias[32];      // null-padded ASCII alias
    unsigned short     versionA;
    unsigned short     versionB;
    unsigned short     versionC;
    unsigned short     _pad;           // align uptime/timestamp to 8-byte boundary
    unsigned long long uptime;         // seconds since process start
    long long          timestamp;      // unix seconds, UTC
    unsigned char      challenge[16];  // echoed verbatim from request

    // SchnorrQ signature over K12(everything above this field).
    unsigned char      signature[64];
};
static_assert(sizeof(RespondLiteCheckin) == 168,
              "RespondLiteCheckin layout drifted");

#pragma pack(pop)

inline void processRequest(Peer* peer, RequestResponseHeader* header)
{
    if (header->size() != sizeof(RequestResponseHeader) + sizeof(RequestLiteCheckin))
        return;

    const auto* req = header->getPayload<RequestLiteCheckin>();

    const auto now = std::chrono::system_clock::now();

    RespondLiteCheckin resp{};

    std::memcpy(resp.publicKey, myPublicKey.m256i_u8, sizeof(resp.publicKey));
    std::memcpy(resp.alias, nodeAlias.data(),
                std::min<size_t>(nodeAlias.size(), sizeof(resp.alias)));

    resp.versionA = (unsigned short)VERSION_A;
    resp.versionB = (unsigned short)VERSION_B;
    resp.versionC = (unsigned short)VERSION_C;

    resp.uptime = (unsigned long long)
        std::chrono::duration_cast<std::chrono::seconds>(now - liteNodeStartTime).count();
    resp.timestamp = (long long)
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    std::memcpy(resp.challenge, req->challenge, sizeof(resp.challenge));

    // Sign K12(struct bytes up to but not including the signature field).
    constexpr size_t signedLen = offsetof(RespondLiteCheckin, signature);
    unsigned char digest[32];
    KangarooTwelve(reinterpret_cast<unsigned char*>(&resp),
                   (unsigned int)signedLen, digest, 32);
    sign(mySubseed.m256i_u8, myPublicKey.m256i_u8, digest, resp.signature);

    enqueueResponse(peer, sizeof(resp), RESPOND_TYPE,
                    header->dejavu(), &resp);
}

} // namespace LiteCheckin
