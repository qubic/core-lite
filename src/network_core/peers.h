// handling of peers, connections, receiving, and transmitting
// (general and low level, independent of specific network message types)

#pragma once

#include <vector>
#include <atomic>

#include <lib/platform_common/processor.h>
#include <lib/platform_efi/uefi.h>
#include "platform/random.h"
#include "platform/concurrency.h"
#include "platform/profiling.h"

#include "network_messages/common_def.h"
#include "network_messages/header.h"
#include "network_messages/common_response.h"

#include "tcp4.h"
#include "kangaroo_twelve.h"

#include "text_output.h"
#include "private_settings.h"

#ifdef TESTNET
  #ifdef TESTNET_LITE_RAM
  #define NETWORK_QUEUEUE_REDUCED_TIME 16 // 64 MB request/response queues — LITE testnet
  #else
  #define NETWORK_QUEUEUE_REDUCED_TIME 4
  #endif
#else
#define NETWORK_QUEUEUE_REDUCED_TIME 1
#endif

#define DEJAVU_SWAP_LIMIT 1000000
#define DISSEMINATION_MULTIPLIER 6
#ifdef TESTNET
#define NUMBER_OF_REGULAR_OUTGOING_CONNECTIONS 4
#define NUMBER_OF_INCOMING_CONNECTIONS 4
#else
#define NUMBER_OF_REGULAR_OUTGOING_CONNECTIONS 8
#define NUMBER_OF_INCOMING_CONNECTIONS 176
#endif
#define NUMBER_OF_OM_NODE_CONNECTIONS (sizeof(oracleMachineIPs) / sizeof(oracleMachineIPs[0]))
#define NUMBER_OF_OC_MACHINE_CONNECTIONS (sizeof(ocMachineIPs) / sizeof(ocMachineIPs[0]))
#define NUMBER_OF_OUTGOING_CONNECTIONS (NUMBER_OF_REGULAR_OUTGOING_CONNECTIONS + NUMBER_OF_OM_NODE_CONNECTIONS + NUMBER_OF_OC_MACHINE_CONNECTIONS)
#define FIRST_OM_NODE_CONNECTION_INDEX NUMBER_OF_REGULAR_OUTGOING_CONNECTIONS
#define FIRST_OC_MACHINE_CONNECTION_INDEX (NUMBER_OF_REGULAR_OUTGOING_CONNECTIONS + NUMBER_OF_OM_NODE_CONNECTIONS)
#define MAX_NUMBER_OF_PUBLIC_PEERS 1024
#define REQUEST_QUEUE_BUFFER_SIZE (1073741824 / NETWORK_QUEUEUE_REDUCED_TIME)
#define REQUEST_QUEUE_LENGTH 65536 // Must be 65536
#define RESPONSE_QUEUE_BUFFER_SIZE (1073741824 / NETWORK_QUEUEUE_REDUCED_TIME)
#define RESPONSE_QUEUE_LENGTH 65536 // Must be 65536
#define NUMBER_OF_PUBLIC_PEERS_TO_KEEP 10
#define NUMBER_OF_WHITE_LIST_PEERS sizeof(whiteListPeers) / sizeof(whiteListPeers[0])
#define NUMBER_OF_INCOMING_CONNECTIONS_RESERVED_FOR_WHITELIST_IPS 16

// OM related setting
static constexpr unsigned int ORACLE_MACHINE_CONNECTION_TIMEOUT_SECS = 15; // Config timout for connecting attemp to OM
static constexpr unsigned int ORACLE_MACHINE_TRANSMITING_TIMEOUT_SECS = 30; // Transmitting timeout to OM
static constexpr unsigned int ORACLE_MACHINE_GRACEFUL_CLOSE_RETRIES = 3; // Graceful close retries for connecting attempt to OM
static constexpr unsigned long long OM_RECONNECT_COOLDOWN_SECS = 0;

// OC machine setting (mirrors the OM settings above)
static constexpr unsigned int OC_MACHINE_CONNECTION_TIMEOUT_SECS = 15; // Config timeout for connecting attempt to OC machine
static constexpr unsigned int OC_MACHINE_TRANSMITING_TIMEOUT_SECS = 30; // Transmitting timeout to OC machine
static constexpr unsigned int OC_MACHINE_GRACEFUL_CLOSE_RETRIES = 3; // Graceful close retries for connecting attempt to OC machine
static constexpr unsigned long long OC_RECONNECT_COOLDOWN_SECS = 0;

#ifndef TESTNET
static_assert((NUMBER_OF_INCOMING_CONNECTIONS / NUMBER_OF_REGULAR_OUTGOING_CONNECTIONS) >= 11, "Number of incoming connections must be x11+ number of outgoing connections to keep healthy network");
#endif

static volatile bool listOfPeersIsStatic = false;

#define OM_RETRY_COUNT(dejavu) ((dejavu) >> 24)
#define OM_SET_RETRY_COUNT(dejavu, count) (((dejavu) & 0x00FFFFFF) | ((count) << 24))
#define OM_MAX_RETRIES 3

#define OC_RETRY_COUNT(dejavu) ((dejavu) >> 24)
#define OC_SET_RETRY_COUNT(dejavu, count) (((dejavu) & 0x00FFFFFF) | ((count) << 24))
#define OC_MAX_RETRIES 3

struct Peer
{
    EFI_TCP4_PROTOCOL* tcp4Protocol;
    EFI_TCP4_LISTEN_TOKEN connectAcceptToken;
    IPv4Address address;
    void* receiveBuffer;
    EFI_TCP4_RECEIVE_DATA receiveData;
    EFI_TCP4_IO_TOKEN receiveToken;
    EFI_TCP4_TRANSMIT_DATA transmitData;
    EFI_TCP4_IO_TOKEN transmitToken;
    char* dataToTransmit;
    unsigned int dataToTransmitSize;
    BOOLEAN isConnectingAccepting;
    BOOLEAN isConnectedAccepted;
    BOOLEAN isReceiving, isTransmitting;
    BOOLEAN exchangedPublicPeers;
    BOOLEAN isClosing;
    // Indicate the peer is incomming connection type
    BOOLEAN isIncommingConnection;

    // Indicate the peer is OM connection type which is a subtype of outgoing connection
    BOOLEAN isOMNode;
    unsigned long long connectionStartTime;
    unsigned long long lastOMActivityTime;
    unsigned long long omTransmitStartTime;
    unsigned long long lastOMCloseTime;

    // Indicate the peer is an OC machine connection type which is a subtype of outgoing connection.
    // Mirrors the OM fields above; connectionStartTime is shared (set on connect for either kind).
    BOOLEAN isOcMachine;
    unsigned long long lastOcActivityTime;
    unsigned long long ocTransmitStartTime;
    unsigned long long lastOcCloseTime;

    // Extra data to determine if this peer is a fullnode
    // Note: an **active fullnode** is a peer that is able to reply valid tick data, tick vote to this node after getting requested
    // If a peer is an active fullnode, it will receive more requests from this node than others, as well as longer alive connection time.
    // Here we also consider relayer as a fullnode (as long as it transmits new&valid tick data)
    static constexpr unsigned int dejavuListSize = 32;
    unsigned int trackRequestedDejavu[dejavuListSize];
    unsigned int trackRequestedTick[dejavuListSize];
    long trackRequestedCounter; // "long" to discard warning from intrin.h
    unsigned int lastActiveTick; // indicate the tick number that this peer transfer valid tick/vote data
    unsigned int peerReportedTick; // highest tick number received from this peer (votes/tickData), used for network-tip / catch-up detection

    bool isFullNode() const
    {
        return (lastActiveTick >= system.tick - 100);
    }

    bool isOracleMachineNode() const
    {
        return isOMNode;
    }

    bool isOcMachineNode() const
    {
        return isOcMachine;
    }

    // store a dejavu number into local list
    void trackDejavu(unsigned int dejavu)
    {
        if (!dejavu) return;
        long index = _InterlockedIncrement(&trackRequestedCounter);
        index %= dejavuListSize;
        trackRequestedDejavu[index] = dejavu;
        trackRequestedTick[index] = system.tick;
    }

    // check if dejavu is inside local list
    // if found, return the tick when the request (with dejavu) was sent
    unsigned int getDejavuTick(unsigned int dejavu)
    {
        if (!dejavu) return 0; // no check for 0 dejavu
        for (int i = 0; i < dejavuListSize; i++)
        {
            if (dejavu == trackRequestedDejavu[i])
            {
                return trackRequestedTick[i];
            }
        }
        return 0;
    }

    // set handler to null and all params to false/zeroes
    void reset()
    {
        tcp4Protocol = NULL;
        isConnectingAccepting = FALSE;
        isConnectedAccepted = FALSE;
        isReceiving = FALSE;
        isTransmitting = FALSE;
        exchangedPublicPeers = FALSE;
        isClosing = FALSE;
        isIncommingConnection = FALSE;

        isOMNode = FALSE;
        lastOMActivityTime = 0;
        connectionStartTime = 0;
        omTransmitStartTime = 0;
        lastOMCloseTime = 0;

        isOcMachine = FALSE;
        lastOcActivityTime = 0;
        ocTransmitStartTime = 0;
        lastOcCloseTime = 0;

        dataToTransmitSize = 0;
        lastActiveTick = 0;
        peerReportedTick = 0;
        trackRequestedCounter = 0;
        setMem(trackRequestedTick, sizeof(trackRequestedTick), 0);
        setMem(trackRequestedDejavu, sizeof(trackRequestedDejavu), 0);
    }
};

typedef struct
{
    bool isHandshaked;
    bool isFullnode;
    IPv4Address address;
} PublicPeer;

static Peer peers[NUMBER_OF_OUTGOING_CONNECTIONS + NUMBER_OF_INCOMING_CONNECTIONS];
static volatile long long numberOfReceivedBytes = 0, prevNumberOfReceivedBytes = 0;
static volatile long long numberOfTransmittedBytes = 0, prevNumberOfTransmittedBytes = 0;
static int numberOfAcceptedIncommingConnection = 0;
// Max incoming slots allowed to arm Accept() (--max-inbound / /set-max-inbound); default = all.
static int maxInboundAccepts = NUMBER_OF_INCOMING_CONNECTIONS;
// Max incoming slots per single IP (--max-inbound-per-ip); 0 = unlimited (default).
static int maxIncomingConnectionsPerIp = 0;
// Extra inbound slots armed beyond --max-inbound; admit loopback only (local RPC/tooling always connects).
static constexpr unsigned int LOOPBACK_INBOUND_RESERVE = 4;

static volatile char publicPeersLock = 0;
static unsigned int numberOfPublicPeers = 0;
static PublicPeer publicPeers[MAX_NUMBER_OF_PUBLIC_PEERS];

static volatile char omPeersLock = 0;
static unsigned int numberOfOMPeers = 0;
IPv4Address omIPv4Address[NUMBER_OF_OM_NODE_CONNECTIONS];

static unsigned int numberOfOcPeers = 0;
IPv4Address ocIPv4Address[NUMBER_OF_OC_MACHINE_CONNECTIONS];

// Delivery counters for OcMachineInvocation messages (reported by logInfo).
// A message is "sent" once it was pushed to at least one connected OC peer,
// "dropped" when all OC_MAX_RETRIES re-enqueues found no ready OC peer.
static volatile long long numberOfOcInvocationsSent = 0;
static volatile long long numberOfOcInvocationsDropped = 0;

static unsigned long long* dejavu0 = NULL;
static unsigned long long* dejavu1 = NULL;
static unsigned int dejavuSwapCounter = DEJAVU_SWAP_LIMIT;

static volatile long long numberOfProcessedRequests = 0, prevNumberOfProcessedRequests = 0;
static volatile long long numberOfDiscardedRequests = 0, prevNumberOfDiscardedRequests = 0;
static volatile long long numberOfDuplicateRequests = 0, prevNumberOfDuplicateRequests = 0;
static volatile long long numberOfDisseminatedRequests = 0, prevNumberOfDisseminatedRequests = 0;
static volatile long long numberOfDroppedTransmits = 0, prevNumberOfDroppedTransmits = 0;   // messages dropped on a full send buffer (peer kept alive)

static unsigned char* requestQueueBuffer = NULL;
static unsigned char* responseQueueBuffer = NULL;

static struct Request
{
    Peer* peer;
    unsigned int offset;
} requestQueueElements[REQUEST_QUEUE_LENGTH];

static struct Response
{
    Peer* peer;
    unsigned int offset;
} responseQueueElements[RESPONSE_QUEUE_LENGTH];

static volatile unsigned int requestQueueBufferHead = 0, requestQueueBufferTail = 0;
static volatile unsigned int responseQueueBufferHead = 0, responseQueueBufferTail = 0;
static volatile unsigned short requestQueueElementHead = 0, requestQueueElementTail = 0;
static volatile unsigned short responseQueueElementHead = 0, responseQueueElementTail = 0;
static volatile char requestQueueTailLock = 0;
static volatile char responseQueueHeadLock = 0;
static volatile unsigned long long queueProcessingNumerator = 0, queueProcessingDenominator = 0;
static volatile unsigned long long tickerLoopNumerator = 0, tickerLoopDenominator = 0;

/*
static bool isWhiteListPeer(unsigned char address[4])
{
    for (unsigned int i = 0; i < NUMBER_OF_WHITE_LIST_PEERS; i++)
    {
        const auto& whiteListIp = whiteListPeers[i];
        if (address[0] == whiteListIp[0]
            && address[1] == whiteListIp[1]
            && address[2] == whiteListIp[2]
            && address[3] == whiteListIp[3])
        {
            return true;
        }
    }
    return false;
}
*/

static bool isPrivateIp(const unsigned char address[4])
{
    int total = min(int(sizeof(knownPublicPeers)/sizeof(knownPublicPeers[0])), NUMBER_OF_PRIVATE_IP);
    for (int i = 0; i < total; i++)
    {
        const auto& privateIp = knownPublicPeers[i];
        if (address[0] == privateIp[0]
            && address[1] == privateIp[1]
            && address[2] == privateIp[2]
            && address[3] == privateIp[3])
        {
            return true;
        }
    }
    return false;
}

#include "extensions/peer_disc_stats.h"

static void closePeer(Peer* peer, int closeGracefullyRetries = 0, unsigned int discReason = PeerDisc::OTHER)
{
    PROFILE_SCOPE();
    ASSERT(isMainProcessor());
    if (((unsigned long long)peer->tcp4Protocol) > 1)
    {
        if (!peer->isClosing)
        {
            peer->isClosing = TRUE;
            PeerDisc::note(discReason, (unsigned int)(peer - peers));
            if (peer->isOracleMachineNode())
            {
                // Track close time for OM nodes to enable reconnection cooldown
                peer->lastOMCloseTime = __rdtsc();
            }
            else if (peer->isOcMachineNode())
            {
                // Track close time for OC machine nodes to enable reconnection cooldown
                peer->lastOcCloseTime = __rdtsc();
            }

            EFI_STATUS_TYPE status = EFI_SUCCESS;
            // Decide to close gracefully with Close()
            if (closeGracefullyRetries > 0)
            {
                EFI_TCP4_CLOSE_TOKEN closeToken;
                bs->SetMem(&closeToken, sizeof(EFI_TCP4_CLOSE_TOKEN), 0);
                status = bs->CreateEvent(0, TPL_CALLBACK, NULL, NULL, &closeToken.CompletionToken.Event);
                closeToken.AbortOnClose = TRUE; // quickly close by send RST then don't care anymore
                status = peer->tcp4Protocol->Close(peer->tcp4Protocol, &closeToken);
                if (status == EFI_SUCCESS)
                {
                    // Poll a few times (non-blocking) - hopefully RST will arrive the
                    for (int i = 0; i < closeGracefullyRetries; i++)
                    {
                        peer->tcp4Protocol->Poll(peer->tcp4Protocol);
                        if (bs->CheckEvent(closeToken.CompletionToken.Event) != EFI_NOT_READY)
                        {
                            break;
                        }
                    }
                    bs->CloseEvent(closeToken.CompletionToken.Event);
                }
                else
                {
                    logStatusToConsole(L"EFI_TCP4_PROTOCOL.Close() fails", status, __LINE__);
                }
            }

            if (status = peer->tcp4Protocol->Configure(peer->tcp4Protocol, NULL))
            {
                logStatusToConsole(L"EFI_TCP4_PROTOCOL.Configure() fails", status, __LINE__);
            }
        }

        if (!peer->isConnectingAccepting && !peer->isReceiving && !peer->isTransmitting)
        {
            bs->CloseProtocol(peer->connectAcceptToken.NewChildHandle, &tcp4ProtocolGuid, ih, NULL);
            EFI_STATUS status;
            if (status = tcp4ServiceBindingProtocol->DestroyChild(tcp4ServiceBindingProtocol, peer->connectAcceptToken.NewChildHandle))
            {
                logStatusToConsole(L"EFI_TCP4_SERVICE_BINDING_PROTOCOL.DestroyChild() fails", status, __LINE__);
            }

            // Decrease the accepted counter
            if (peer->isConnectedAccepted && peer->isIncommingConnection)
            {
                numberOfAcceptedIncommingConnection--;
                ASSERT(numberOfAcceptedIncommingConnection >= 0);
            }

            // Save OM/OC close time before reset
            unsigned long long savedOMCloseTime = peer->lastOMCloseTime;
            unsigned long long savedOcCloseTime = peer->lastOcCloseTime;
            bool wasOMNode = peer->isOracleMachineNode();
            bool wasOcMachine = peer->isOcMachineNode();
            unsigned int peerIndex = (unsigned int)(peer - peers);

            peer->reset();

            if (wasOMNode)
            {
                peer->lastOMCloseTime = savedOMCloseTime;
                peer->isOMNode = TRUE;
                peer->address = omIPv4Address[peerIndex - FIRST_OM_NODE_CONNECTION_INDEX];
            }
            else if (wasOcMachine)
            {
                peer->lastOcCloseTime = savedOcCloseTime;
                peer->isOcMachine = TRUE;
                peer->address = ocIPv4Address[peerIndex - FIRST_OC_MACHINE_CONNECTION_INDEX];
            }
        }
    }
}

// Closes all peer connections; optionally includes the persistent machine-node connections
// (Oracle Machine and OC machine), which are otherwise kept open for low latency.
static void closeAllPeers(bool closeMachineNodes = false)
{
    for (unsigned int i = 0; i < NUMBER_OF_OUTGOING_CONNECTIONS + NUMBER_OF_INCOMING_CONNECTIONS; i++)
    {
        if (closeMachineNodes || (!peers[i].isOracleMachineNode() && !peers[i].isOcMachineNode()))
        {
            closePeer(&peers[i]);
        }
    }
}

// Add message to sending buffer of specific peer, can only called from main thread (not thread-safe).
static void push(Peer* peer, RequestResponseHeader* requestResponseHeader)
{
    PROFILE_SCOPE();

    // The sending buffer may queue multiple messages, each of which may need to transmitted in many small packets.
    if (peer->tcp4Protocol && peer->isConnectedAccepted && !peer->isClosing)
    {
        // Read the size exactly once: re-reading requestResponseHeader->size() while the
        // source is concurrently mutated would make the copy length, the cursor advance and
        // the copied header disagree, producing a malformed frame in the outgoing buffer.
        const unsigned int msgSize = requestResponseHeader->size();
        if (peer->dataToTransmitSize + msgSize > BUFFER_SIZE)
        {
            // Send buffer full: drop this message (peer re-requests on timeout) instead of culling.
            // A slow consumer (log reader, save-frozen peer) must survive, not get disconnected.
#ifndef NDEBUG
            {
                CHAR16 debugMessage[256];
                setText(debugMessage, L"Warning: Peer transmit buffer overflow. IP: ");
                appendIPv4Address(debugMessage, peer->address);
                appendText(debugMessage, L" | dataToTransmitSize: ");
                appendNumber(debugMessage, peer->dataToTransmitSize, true);
                appendText(debugMessage, L" | requestResponseHeader->size(): ");
                appendNumber(debugMessage, msgSize, true);
                addDebugMessage(debugMessage);
            }
#endif
            _InterlockedIncrement64(&numberOfDroppedTransmits);
        }
        else
        {
            // Add message to buffer
            copyMem(&peer->dataToTransmit[peer->dataToTransmitSize], requestResponseHeader, msgSize);
            peer->dataToTransmitSize += msgSize;
            peer->trackDejavu(requestResponseHeader->dejavu());
            _InterlockedIncrement64(&numberOfDisseminatedRequests);
        }
    }
}

// Add message to sending buffer of custom filtered (and random) peer, can only called from main thread (not thread-safe).
static void pushCustom(RequestResponseHeader* requestResponseHeader, int numberOfReceivers, bool filterFullNode)
{
    unsigned short suitablePeerIndices[NUMBER_OF_OUTGOING_CONNECTIONS + NUMBER_OF_INCOMING_CONNECTIONS];
    unsigned short numberOfSuitablePeers = 0;
    for (unsigned int i = 0; i < NUMBER_OF_OUTGOING_CONNECTIONS + NUMBER_OF_INCOMING_CONNECTIONS; i++)
    {
        // Outbound (dialed) peers are request-eligible once connected, before ExchangePublicPeers arrives.
        if (peers[i].tcp4Protocol && peers[i].isConnectedAccepted
            && (i < NUMBER_OF_REGULAR_OUTGOING_CONNECTIONS || peers[i].exchangedPublicPeers) && !peers[i].isClosing)
        {
            if ((filterFullNode && peers[i].isFullNode()) || (!filterFullNode))
            {
                suitablePeerIndices[numberOfSuitablePeers++] = i;
            }
        }
    }
    unsigned short numberOfRemainingSuitablePeers = numberOfReceivers;
    while (numberOfRemainingSuitablePeers-- && numberOfSuitablePeers)
    {
        const unsigned short index = random(numberOfSuitablePeers);
        push(&peers[suitablePeerIndices[index]], requestResponseHeader);
        suitablePeerIndices[index] = suitablePeerIndices[--numberOfSuitablePeers];
    }
}

// Add message to sending buffer of random peer, can only called from main thread (not thread-safe).
static void pushToAny(RequestResponseHeader* requestResponseHeader)
{
    PROFILE_SCOPE();
    const bool filterFullNode = false;
    pushCustom(requestResponseHeader, 1, filterFullNode);
}

// Add message to sending buffer of some(DISSEMINATION_MULTIPLIER) random peers, can only called from main thread (not thread-safe).
static void pushToSeveral(RequestResponseHeader* requestResponseHeader)
{
    PROFILE_SCOPE();
    const bool filterFullNode = false;
    pushCustom(requestResponseHeader, DISSEMINATION_MULTIPLIER, filterFullNode);
}

// Add message to sending buffer of any full node peer, can only called from main thread (not thread-safe).
static void pushToAnyFullNode(RequestResponseHeader* requestResponseHeader)
{
    PROFILE_SCOPE();
    const bool filterFullNode = true;
    pushCustom(requestResponseHeader, 1, filterFullNode);
}

// Add message to sending buffer of some full node peers, can only called from main thread (not thread-safe).
static void pushToSeveralFullNode(RequestResponseHeader* requestResponseHeader)
{
    PROFILE_SCOPE();
    const bool filterFullNode = true;
    pushCustom(requestResponseHeader, DISSEMINATION_MULTIPLIER, filterFullNode);
}

// Add message to sending buffer of some (limit by DISSEMINATION_MULTIPLIER) full node peer, can only called from main thread (not thread-safe).
static void pushToFullNodes(RequestResponseHeader* requestResponseHeader, int numberOfReceivers)
{
    PROFILE_SCOPE();
    if (numberOfReceivers > DISSEMINATION_MULTIPLIER)
    {
        pushToSeveralFullNode(requestResponseHeader);
    }
    else
    {
        const bool filterFullNode = true;
        pushCustom(requestResponseHeader, numberOfReceivers, filterFullNode);
    }
}

// Fan-out widths for catch-up "fetch missing data" requests.  Each upstream
// pushToAny + pushToAnyFullNode pair sends to only 2 peers; if either is slow
// the whole 500 ms tick-request poll is wasted.  Sending to a few more peers
// in parallel shortens the average wait at negligible bandwidth cost.
static constexpr int CATCHUP_FANOUT_ANY      = 3;
static constexpr int CATCHUP_FANOUT_FULLNODE = 2;
static constexpr int CATCHUP_FANOUT_TOTAL    = CATCHUP_FANOUT_ANY + CATCHUP_FANOUT_FULLNODE;

// Fan out a "fetch missing data" request to several random peers plus several
// fullnode peers.  Replaces the upstream pushToAny + pushToAnyFullNode pair
// for requests whose completion gates tick advancement.
static void pushCatchupFanOut(RequestResponseHeader* requestResponseHeader)
{
    PROFILE_SCOPE();
    pushCustom(requestResponseHeader, CATCHUP_FANOUT_ANY,      /*filterFullNode=*/false);
    pushCustom(requestResponseHeader, CATCHUP_FANOUT_FULLNODE, /*filterFullNode=*/true);
}

// Send a "fetch data for tick T" request preferring peers whose peerReportedTick
// is >= T (i.e. peers that have provably processed up to or past that tick, so
// they must have its tick-data, votes, and transactions).  Falls back to peers
// below the target when not enough qualified peers exist — so we always send
// to up to `numberOfReceivers` peers total, just with a quality preference.
//
// Used in catch-up mode for tick-data, quorum-tick, and tick-transaction
// requests where the target tick is known.  Peers below the target can't help,
// so prefer the ones that can.
static void pushPreferringAtOrAbove(
    RequestResponseHeader* requestResponseHeader,
    unsigned int targetTick,
    int numberOfReceivers = CATCHUP_FANOUT_TOTAL)
{
    PROFILE_SCOPE();
    unsigned short qualified[NUMBER_OF_OUTGOING_CONNECTIONS + NUMBER_OF_INCOMING_CONNECTIONS];
    unsigned short unqualified[NUMBER_OF_OUTGOING_CONNECTIONS + NUMBER_OF_INCOMING_CONNECTIONS];
    int numQualified = 0;
    int numUnqualified = 0;

    for (unsigned int i = 0; i < NUMBER_OF_OUTGOING_CONNECTIONS + NUMBER_OF_INCOMING_CONNECTIONS; i++)
    {
        if (peers[i].tcp4Protocol && peers[i].isConnectedAccepted
            && (i < NUMBER_OF_REGULAR_OUTGOING_CONNECTIONS || peers[i].exchangedPublicPeers) && !peers[i].isClosing)
        {
            if (peers[i].peerReportedTick >= targetTick)
                qualified[numQualified++] = i;
            else
                unqualified[numUnqualified++] = i;
        }
    }

    int remaining = numberOfReceivers;
    // Prefer qualified peers (known to have data for this tick).
    while (remaining > 0 && numQualified > 0)
    {
        const unsigned short idx = random(numQualified);
        push(&peers[qualified[idx]], requestResponseHeader);
        qualified[idx] = qualified[--numQualified];
        remaining--;
    }
    // Fall back to the rest of the pool so we still hit the fan-out width
    // when qualified candidates are sparse (e.g. fresh node, peerReportedTick
    // still 0 for most peers because nobody has replied to us yet).
    while (remaining > 0 && numUnqualified > 0)
    {
        const unsigned short idx = random(numUnqualified);
        push(&peers[unqualified[idx]], requestResponseHeader);
        unqualified[idx] = unqualified[--numUnqualified];
        remaining--;
    }
}

// Add message to response queue of specific peer. If peer is NULL, it will be sent to random peers. Can be called from any thread.
static void enqueueResponse(Peer* peer, RequestResponseHeader* responseHeader)
{
    PROFILE_SCOPE();

    ACQUIRE(responseQueueHeadLock);

    if ((responseQueueBufferHead >= responseQueueBufferTail || responseQueueBufferHead + responseHeader->size() < responseQueueBufferTail)
        && (unsigned short)(responseQueueElementHead + 1) != responseQueueElementTail)
    {
        ASSERT(responseQueueElementHead < RESPONSE_QUEUE_LENGTH);
        ASSERT(responseQueueBufferHead < RESPONSE_QUEUE_BUFFER_SIZE);
        ASSERT(responseQueueBufferHead + responseHeader->size() < RESPONSE_QUEUE_BUFFER_SIZE);

        responseQueueElements[responseQueueElementHead].offset = responseQueueBufferHead;
        copyMem(&responseQueueBuffer[responseQueueBufferHead], responseHeader, responseHeader->size());
        responseQueueBufferHead += responseHeader->size();
        responseQueueElements[responseQueueElementHead].peer = peer;
        if (responseQueueBufferHead > RESPONSE_QUEUE_BUFFER_SIZE - BUFFER_SIZE)
        {
            responseQueueBufferHead = 0;
        }
        responseQueueElementHead++;
    }

    RELEASE(responseQueueHeadLock);
}


static void pushToOracleMachineNodes(RequestResponseHeader* requestResponseHeader)
{
#if !defined(NDEBUG)
    // TODO: cleanup this debug code when OM connection is fully stable
    setText(::message, L"pushToOracleMachineNodes(): ");
#endif
    if (NUMBER_OF_OM_NODE_CONNECTIONS > 0)
    {
        bool pushedToAny = false;
        unsigned short numberOfSuitablePeers = 0;

        // Heuristic pick the first byte of dejavu for tracking the retries
        // Clear the dejavu retry if first bytes is greater than OM_MAX_RETRIES
        // Incase of first bytes is 1 .. OM_MAX_RETRIES, we retrie fewer
        // Currently is 0 when creating a querry, so it is not a problem
        unsigned int currentDejavu = requestResponseHeader->dejavu();
        if (OM_RETRY_COUNT(currentDejavu) > OM_MAX_RETRIES)
        {
            requestResponseHeader->setDejavu(currentDejavu & 0x00FFFFFF);
        }

        for (unsigned int i = 0; i < NUMBER_OF_OUTGOING_CONNECTIONS + NUMBER_OF_INCOMING_CONNECTIONS && numberOfSuitablePeers < NUMBER_OF_OM_NODE_CONNECTIONS; i++)
        {
            if (peers[i].isOracleMachineNode())
            {
                if (peers[i].tcp4Protocol
                    && peers[i].isConnectedAccepted
                    && !peers[i].isClosing)
                {
#if !defined(NDEBUG)
                    appendIPv4Address(::message, peers[i].address);
#endif
                    push(&peers[i], requestResponseHeader);
                    numberOfSuitablePeers++;
                    pushedToAny = true;
                }
            }
        }

        // Re-enqueue if no OM peer was ready.
        if (!pushedToAny)
        {
            unsigned int retryCount = OM_RETRY_COUNT(requestResponseHeader->dejavu());
            if (retryCount < OM_MAX_RETRIES)
            {
                requestResponseHeader->setDejavu(
                    OM_SET_RETRY_COUNT(requestResponseHeader->dejavu(), retryCount + 1));
                enqueueResponse((Peer*)1, requestResponseHeader);
            }
        }
    }
#if !defined(NDEBUG)
    addDebugMessage(::message);
#endif

}


// Push a message (an OcMachineInvocation) to all connected OC machine peers.
// Mirrors pushToOracleMachineNodes. Re-enqueues for retry if no OC peer is ready yet.
static void pushToOcMachineNodes(RequestResponseHeader* requestResponseHeader)
{
    if (NUMBER_OF_OC_MACHINE_CONNECTIONS > 0)
    {
        bool pushedToAny = false;
        unsigned short numberOfSuitablePeers = 0;

        unsigned int currentDejavu = requestResponseHeader->dejavu();
        if (OC_RETRY_COUNT(currentDejavu) > OC_MAX_RETRIES)
        {
            requestResponseHeader->setDejavu(currentDejavu & 0x00FFFFFF);
        }

        for (unsigned int i = 0; i < NUMBER_OF_OUTGOING_CONNECTIONS + NUMBER_OF_INCOMING_CONNECTIONS && numberOfSuitablePeers < NUMBER_OF_OC_MACHINE_CONNECTIONS; i++)
        {
            if (peers[i].isOcMachineNode())
            {
                if (peers[i].tcp4Protocol
                    && peers[i].isConnectedAccepted
                    && !peers[i].isClosing)
                {
                    push(&peers[i], requestResponseHeader);
                    numberOfSuitablePeers++;
                    pushedToAny = true;
                }
            }
        }

        // Re-enqueue if no OC peer was ready.
        if (!pushedToAny)
        {
            unsigned int retryCount = OC_RETRY_COUNT(requestResponseHeader->dejavu());
            if (retryCount < OC_MAX_RETRIES)
            {
                requestResponseHeader->setDejavu(
                    OC_SET_RETRY_COUNT(requestResponseHeader->dejavu(), retryCount + 1));
                enqueueResponse((Peer*)2, requestResponseHeader);
            }
            else
            {
                _InterlockedIncrement64(&numberOfOcInvocationsDropped);
            }
        }
        else
        {
            _InterlockedIncrement64(&numberOfOcInvocationsSent);
        }
    }
}


// Add message to response queue of specific peer. If peer is NULL, it will be sent to random peers. Can be called from any thread.
static void enqueueResponse(Peer* peer, unsigned int dataSize, unsigned char type, unsigned int dejavu, const void* data)
{
    PROFILE_SCOPE();

    ACQUIRE(responseQueueHeadLock);

    if ((responseQueueBufferHead >= responseQueueBufferTail || responseQueueBufferHead + sizeof(RequestResponseHeader) + dataSize < responseQueueBufferTail)
        && (unsigned short)(responseQueueElementHead + 1) != responseQueueElementTail)
    {
        ASSERT(responseQueueElementHead < RESPONSE_QUEUE_LENGTH);
        ASSERT(responseQueueBufferHead < RESPONSE_QUEUE_BUFFER_SIZE);
        ASSERT(responseQueueBufferHead + sizeof(RequestResponseHeader) + dataSize < RESPONSE_QUEUE_BUFFER_SIZE);

        responseQueueElements[responseQueueElementHead].offset = responseQueueBufferHead;
        RequestResponseHeader* responseHeader = (RequestResponseHeader*)&responseQueueBuffer[responseQueueBufferHead];
        if (!responseHeader->checkAndSetSize(sizeof(RequestResponseHeader) + dataSize))
        {
#ifndef NDEBUG
            addDebugMessage(L"Error: Message size exceeds maximum message size!");
#endif
        }
        responseHeader->setType(type);
        responseHeader->setDejavu(dejavu);
        if (data)
        {
            copyMem(&responseQueueBuffer[responseQueueBufferHead + sizeof(RequestResponseHeader)], data, dataSize);
        }
        responseQueueBufferHead += responseHeader->size();
        responseQueueElements[responseQueueElementHead].peer = peer;
        if (responseQueueBufferHead > RESPONSE_QUEUE_BUFFER_SIZE - BUFFER_SIZE)
        {
            responseQueueBufferHead = 0;
        }
        responseQueueElementHead++;
    }

    RELEASE(responseQueueHeadLock);
}

/**
* checks if a given address is a bogon address
* a bogon address is an ip address which should not be used publicly (e.g. private networks)
*
* @param address the ip address to be checked
* @return true if address is bogon or false if not
*/
static bool isBogonAddress(const IPv4Address& address)
{
#ifdef  TESTNET
	return false; // in testnet mode, we allow all ip addresses
#else
    return (!address.u8[0])
        || (address.u8[0] == 127)
        || (address.u8[0] == 10)
        || (address.u8[0] == 172 && address.u8[1] >= 16 && address.u8[1] <= 31)
        || (address.u8[0] == 192 && address.u8[1] == 168)
        || (address.u8[0] == 255);
#endif
}

/**
* checks if a given address was manually set in the initial list of known public peers
*
* @param address the ip address to be checked
* @return true if the ip address is in the Known Public Peers or false if not
*/
static bool isAddressInKnownPublicPeers(const IPv4Address& address)
{
    // keep this exception to avoid bogon addresses kept for outgoing connections
    if (isBogonAddress(address))
        return false;

    for (unsigned int i = 0; i < sizeof(knownPublicPeers) / sizeof(knownPublicPeers[0]); i++)
    {
        const IPv4Address& peer_ip = *reinterpret_cast<const IPv4Address*>(knownPublicPeers[i]);
        if (peer_ip == address)
            return true;
    }
    return false;
}


// Inactive pool: forgotten (bad/unresponsive) peers are parked here instead of dropped, then
// recycled into candidates when active peers fall to the keep-floor.
static IPv4Address inactivePublicPeers[MAX_NUMBER_OF_PUBLIC_PEERS];
static unsigned int numberOfInactivePublicPeers = 0;

// Park an address in the inactive pool (dedup, bounded). Caller holds publicPeersLock.
static void pushInactivePublicPeer(const IPv4Address& address)
{
    for (unsigned int i = 0; i < numberOfInactivePublicPeers; i++)
        if (inactivePublicPeers[i] == address)
            return;
    if (numberOfInactivePublicPeers < MAX_NUMBER_OF_PUBLIC_PEERS)
        inactivePublicPeers[numberOfInactivePublicPeers++] = address;
}

// Forget a peer: park it in the inactive pool and drop it from active candidates.
static void forgetPublicPeer(const IPv4Address& address)
{
    // if address is one of our initial peers we don't forget it
    if (isAddressInKnownPublicPeers(address))
    {
        return;
    }

    if (listOfPeersIsStatic)
    {
        return;
    }

    ACQUIRE(publicPeersLock);

    for (unsigned int i = 0; i < numberOfPublicPeers; i++)
    {
        if (publicPeers[i].address == address)
        {
            pushInactivePublicPeer(address); // park for recycling, don't drop
            if (i != --numberOfPublicPeers)
            {
                copyMem(&publicPeers[i], &publicPeers[numberOfPublicPeers], sizeof(PublicPeer));
            }

            break;
        }
    }

    RELEASE(publicPeersLock);
}

// Penalize rejected connection by setting verified peer to non-verified or forgetting a non-verified peer
static void penalizePublicPeerRejectedConnection(const IPv4Address& address)
{
    // Never demote baked-in knownPublicPeers (seed list). Transient connect
    // rejects (peer's incoming slots full, momentary network blip) shouldn't
    // strip the seed-quality flag from peers we explicitly chose to trust.
    if (isAddressInKnownPublicPeers(address))
        return;

    bool forgetPeer = false;

    ACQUIRE(publicPeersLock);

    for (unsigned int i = 0; i < numberOfPublicPeers; i++)
    {
        if (publicPeers[i].address == address)
        {
            if (publicPeers[i].isHandshaked || publicPeers[i].isFullnode)
            {
                publicPeers[i].isHandshaked = false;
                publicPeers[i].isFullnode = false;
            }
            else
            {
                forgetPeer = true;
            }
            break;
        }
    }

    RELEASE(publicPeersLock);

    if (forgetPeer)
    {
        forgetPublicPeer(address);
    }
}


static void addPublicPeer(const IPv4Address& address)
{
    if (isBogonAddress(address)) // not add bogon ip
    {
        return;
    }
    if (address.u32 == 0) // not add zero address
    {
        return;
    }
    for (unsigned int i = 0; i < numberOfPublicPeers; i++)
    {
        if (address == publicPeers[i].address)
        {
            return;
        }
    }

    ACQUIRE(publicPeersLock);

    if (numberOfPublicPeers < MAX_NUMBER_OF_PUBLIC_PEERS)
    {
        publicPeers[numberOfPublicPeers].isHandshaked = false;
        publicPeers[numberOfPublicPeers].isFullnode = false;
        publicPeers[numberOfPublicPeers++].address = address;
    }

    RELEASE(publicPeersLock);
}

// Recycle the inactive pool back into candidates when active peers hit the keep-floor.
static void refillPublicPeersFromInactive()
{
    if (listOfPeersIsStatic || numberOfPublicPeers > NUMBER_OF_PUBLIC_PEERS_TO_KEEP || numberOfInactivePublicPeers == 0)
        return;
    IPv4Address recycled[MAX_NUMBER_OF_PUBLIC_PEERS];
    ACQUIRE(publicPeersLock);
    unsigned int n = numberOfInactivePublicPeers;
    for (unsigned int i = 0; i < n; i++)
        recycled[i] = inactivePublicPeers[i];
    numberOfInactivePublicPeers = 0;
    RELEASE(publicPeersLock);
    for (unsigned int i = 0; i < n; i++)
        addPublicPeer(recycled[i]); // re-add as fresh candidates (dedups internally)
}

static bool peerConnectionNewlyEstablished(unsigned int i)
{
    PROFILE_SCOPE();

    // handle new connections (called in main loop)
    if (((unsigned long long)peers[i].tcp4Protocol)
        && peers[i].connectAcceptToken.CompletionToken.Status != -1)
    {
        peers[i].isConnectingAccepting = FALSE;

        if (i < NUMBER_OF_OUTGOING_CONNECTIONS)
        {
            // outgoing connection
            peers[i].isIncommingConnection = FALSE;

            if (peers[i].connectAcceptToken.CompletionToken.Status)
            {
                // connection rejected
                peers[i].connectAcceptToken.CompletionToken.Status = -1;
                penalizePublicPeerRejectedConnection(peers[i].address);

                closePeer(&peers[i], 0, PeerDisc::CONNECT_REJECT);

            }
            else
            {
                peers[i].connectAcceptToken.CompletionToken.Status = -1;
                if (peers[i].isClosing)
                {
                    closePeer(&peers[i]);
                }
                else
                {
                    peers[i].isConnectedAccepted = TRUE;
                    if (peers[i].isOracleMachineNode())
                    {
                        peers[i].lastOMActivityTime = __rdtsc();
                    }
                    else if (peers[i].isOcMachineNode())
                    {
                        peers[i].lastOcActivityTime = __rdtsc();
                    }
                }
            }
        }
        else
        {
            // incoming connection
            peers[i].isIncommingConnection = TRUE;
            if (peers[i].connectAcceptToken.CompletionToken.Status)
            {
                // connection error
                peers[i].connectAcceptToken.CompletionToken.Status = -1;
                peers[i].tcp4Protocol = NULL;
            }
            else
            {
                peers[i].connectAcceptToken.CompletionToken.Status = -1;
                if (peers[i].isClosing)
                {
                    closePeer(&peers[i]);
                }
                else
                {
                    EFI_STATUS status = bs->OpenProtocol(peers[i].connectAcceptToken.NewChildHandle, &tcp4ProtocolGuid, (void**)&peers[i].tcp4Protocol, ih, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
                    if (status)
                    {
                        logStatusToConsole(L"EFI_BOOT_SERVICES.OpenProtocol() fails", status, __LINE__);

                        tcp4ServiceBindingProtocol->DestroyChild(tcp4ServiceBindingProtocol, peers[i].connectAcceptToken.NewChildHandle);
                        peers[i].tcp4Protocol = NULL;
                    }
                    else
                    {
                        /* GetModeData() freezes the node occasionally: the quick-fix is to disable whitelisting
                        // If number of unused incoming connection slots is low, only accept white list IPs
                        if (NUMBER_OF_INCOMING_CONNECTIONS - numberOfAcceptedIncommingConnection < NUMBER_OF_INCOMING_CONNECTIONS_RESERVED_FOR_WHITELIST_IPS)
                        {
                            EFI_TCP4_CONFIG_DATA tcp4ConfigData;
                            if (peers[i].tcp4Protocol
                                && !peers[i].tcp4Protocol->GetModeData(peers[i].tcp4Protocol, NULL, &tcp4ConfigData, NULL, NULL, NULL))
                            {
                                if (!isWhiteListPeer(tcp4ConfigData.AccessPoint.RemoteAddress.Addr))
                                {
                                    closePeer(&peers[i]);
                                    return false;
                                }
                                else
                                {
                                    peers[i].isConnectedAccepted = TRUE;
                                    peers[i].address.u32 = *(unsigned int*)tcp4ConfigData.AccessPoint.RemoteAddress.Addr;
                                }
                            }
                            else
                            {
                                closePeer(&peers[i]);
                                return false;
                            }
                        }
                        else
                        */
                        {
                            peers[i].isConnectedAccepted = TRUE;
                        }
                    }
                }
            }
        }
        // new connection has been established
        if (peers[i].isConnectedAccepted)
        {
            if (peers[i].isIncommingConnection)
            {
                // Loopback (local RPC/tooling) is never capped: unlimited slots, unlimited per-IP.
                const bool isLoopback = (peers[i].address.u8[0] == 127);

                // Count before the cap rejects so each reject's closePeer() decrement stays balanced.
                numberOfAcceptedIncommingConnection++;
                ASSERT(numberOfAcceptedIncommingConnection <= NUMBER_OF_INCOMING_CONNECTIONS);

                // Total inbound cap (--max-inbound): reserve-band slots admit only loopback, so local
                // connections get in even when remotes saturate the cap.
                if (!isLoopback && (i - NUMBER_OF_OUTGOING_CONNECTIONS) >= (unsigned int)maxInboundAccepts)
                {
                    closePeer(&peers[i]);
                    return false;
                }

                // Per-IP cap (--max-inbound-per-ip; 0 = unlimited; loopback exempt): one peer can't flood
                // many slots (wastes slots + redundant sends -> churn).
                if (maxIncomingConnectionsPerIp > 0 && !isLoopback && peers[i].address.u32 != 0)
                {
                    unsigned int sameIp = 0;
                    for (unsigned int k = 0; k < NUMBER_OF_OUTGOING_CONNECTIONS + NUMBER_OF_INCOMING_CONNECTIONS; k++)
                    {
                        if (k != i && (unsigned long long)peers[k].tcp4Protocol > 1 && peers[k].isIncommingConnection
                            && peers[k].isConnectedAccepted && !peers[k].isClosing && peers[k].address == peers[i].address)
                        {
                            if (++sameIp >= (unsigned int)maxIncomingConnectionsPerIp)
                            {
                                closePeer(&peers[i]);
                                return false;
                            }
                        }
                    }
                }
            }
            return true;
        }
    }
    return false;
}

// This function process all data that arrive in FragmentBuffer.
// based on RequestResponseHeader to determine whether the received packet is completed or not
// if it receives a completed packet, it will copy the packet to requestQueueElements to process later in requestProcessors
static void processReceivedData(unsigned int i, unsigned int salt)
{
    PROFILE_SCOPE();

    if (((unsigned long long)peers[i].tcp4Protocol) > 1)
    {
        if (peers[i].receiveToken.CompletionToken.Status != -1)
        {
            // Pair with the release fence in receiveProcessor: we read Status above and
            // DataLength below, so acquire here to guarantee the receive thread's DataLength
            // store is visible (otherwise a stale, buffer-sized DataLength over-advances the
            // cursor into zero-filled memory and the parser reads size()==0).
            std::atomic_thread_fence(std::memory_order_acquire);
            peers[i].isReceiving = FALSE;
            if (peers[i].receiveToken.CompletionToken.Status)
            {
                peers[i].receiveToken.CompletionToken.Status = -1;
                closePeer(&peers[i], 0, PeerDisc::RECV_ERR);
            }
            else
            {
                peers[i].receiveToken.CompletionToken.Status = -1;
                if (peers[i].isClosing)
                {
                    closePeer(&peers[i]);
                }
                else
                {
                    // Defensive: never advance the receive cursor past the end of the buffer,
                    // even if a bogus DataLength were ever observed. recv is already capped to
                    // this same free space, so this clamp is a no-op in normal operation.
                    const unsigned long long usedBefore = ((unsigned long long)peers[i].receiveData.FragmentTable[0].FragmentBuffer) - ((unsigned long long)peers[i].receiveBuffer);
                    unsigned int receivedLen = peers[i].receiveData.DataLength;
                    if (receivedLen > BUFFER_SIZE - usedBefore)
                    {
                        receivedLen = (unsigned int)(BUFFER_SIZE - usedBefore);
                    }
                    numberOfReceivedBytes += receivedLen;
                    PeerDisc::noteRx(i, receivedLen);
                    *((unsigned long long*) & peers[i].receiveData.FragmentTable[0].FragmentBuffer) += receivedLen;

                    // Parse every complete packet in place, advancing a read cursor per packet.
                    const unsigned int totalReceivedSize = (unsigned int)(((unsigned long long)peers[i].receiveData.FragmentTable[0].FragmentBuffer) - ((unsigned long long)peers[i].receiveBuffer));
                    unsigned int readOffset = 0;
                    bool peerForgotten = false;

                    while (totalReceivedSize - readOffset >= sizeof(RequestResponseHeader))
                    {
                        RequestResponseHeader* requestResponseHeader = (RequestResponseHeader*)((char*)peers[i].receiveBuffer + readOffset);
                        if (requestResponseHeader->size() < sizeof(RequestResponseHeader))
                        {
                            // TODO: remove this tolerate on epoch 218
                            // TOLERATE TRAILING ZERO PADDING: other nodes run old buggy code that
                            // can append zero bytes past their real messages. A valid frame never
                            // starts with a zero size field (min message size is
                            // sizeof(RequestResponseHeader)), so a zero here is padding, not a frame.
                            // Skip the zero run and resync instead of forgetting the (un-updatable) peer.
                            if (requestResponseHeader->size() == 0)
                            {
                                const unsigned char* p = (const unsigned char*)peers[i].receiveBuffer;
                                unsigned int z = readOffset;
                                while (z < totalReceivedSize && p[z] == 0) z++;
                                _InterlockedIncrement64(&numberOfDiscardedRequests);
                                readOffset = z;
                                continue; // re-check loop condition; resync at next non-zero byte
                            }
                            // protocol violation -> forget peer
                            setText(message, L"Forgetting ");
                            appendIPv4Address(message, peers[i].address);
                            appendText(message, L"...");
                            logToConsole(message);
                            forgetPublicPeer(peers[i].address);
                            closePeer(&peers[i], 0, PeerDisc::PROTO_VIOLATION);
                            peerForgotten = true;
                            break;
                        }

                        // Packet not fully received yet; stop and wait for more bytes.
                        if (totalReceivedSize - readOffset < requestResponseHeader->size())
                        {
                            break;
                        }

                        // Compute saltId of packet with K12 of payload and header (size + type temporarily
                        // overwritten with salt). This is used recognized and skip packet duplicates with
                        // dejavu0 (checking/setting flag for received package). After receiving a certain
                        // number of packages (DEJAVU_SWAP_LIMIT), dejavu0 is moved to dejavu1 for checking
                        // and dejavu0 is initialized with an empty buffer for checking/setting.
                        unsigned int saltedId;
                        const unsigned int header = *((unsigned int*)requestResponseHeader);
                        *((unsigned int*)requestResponseHeader) = salt;
                        KangarooTwelve(requestResponseHeader, header & 0xFFFFFF, &saltedId, sizeof(saltedId));
                        *((unsigned int*)requestResponseHeader) = header;
                        // mask hash into allocated dejavu bit-range (no-op at 512 MB; required when LITE shrinks pool)
                        saltedId &= (unsigned int)(DEJAVU_POOL_SIZE * 8ULL - 1);

                        // Initiate transfer of already received packet to processing thread
                        // (or drop it without processing if Dejavu filter tells to ignore it)
                        if (!((dejavu0[saltedId >> 6] | dejavu1[saltedId >> 6]) & (1ULL << (saltedId & 63))))
                        {
                            if ((requestQueueBufferHead >= requestQueueBufferTail || requestQueueBufferHead + requestResponseHeader->size() < requestQueueBufferTail)
                                && (unsigned short)(requestQueueElementHead + 1) != requestQueueElementTail)
                            {
                                dejavu0[saltedId >> 6] |= (1ULL << (saltedId & 63));

                                ASSERT(requestQueueElementHead < REQUEST_QUEUE_LENGTH);
                                ASSERT(requestQueueBufferHead < REQUEST_QUEUE_BUFFER_SIZE);
                                ASSERT(requestQueueBufferHead + requestResponseHeader->size() < REQUEST_QUEUE_BUFFER_SIZE);

                                requestQueueElements[requestQueueElementHead].offset = requestQueueBufferHead;
                                copyMem(&requestQueueBuffer[requestQueueBufferHead], (char*)peers[i].receiveBuffer + readOffset, requestResponseHeader->size());
                                requestQueueBufferHead += requestResponseHeader->size();
                                requestQueueElements[requestQueueElementHead].peer = &peers[i];
                                if (requestQueueBufferHead > REQUEST_QUEUE_BUFFER_SIZE - BUFFER_SIZE)
                                {
                                    requestQueueBufferHead = 0;
                                }
                                // TODO: Place a fence
                                requestQueueElementHead++;

                                if (!(--dejavuSwapCounter))
                                {
                                    unsigned long long* tmp = dejavu1;
                                    dejavu1 = dejavu0;
                                    setMem(dejavu0 = tmp, DEJAVU_POOL_SIZE, 0);
                                    dejavuSwapCounter = DEJAVU_SWAP_LIMIT;
                                }
                            }
                            else
                            {
                                _InterlockedIncrement64(&numberOfDiscardedRequests);

                                enqueueResponse(&peers[i], 0, TryAgain::type(), requestResponseHeader->dejavu(), NULL);
                            }
                        }
                        else
                        {
                            _InterlockedIncrement64(&numberOfDuplicateRequests);
                        }

                        readOffset += requestResponseHeader->size();
                    }

                    // Compact the unparsed remainder to the front once, then reposition the write cursor.
                    if (!peerForgotten)
                    {
                        const unsigned int remainingSize = totalReceivedSize - readOffset;
                        if (readOffset && remainingSize)
                        {
                            copyMem(peers[i].receiveBuffer, (char*)peers[i].receiveBuffer + readOffset, remainingSize);
                        }
                        peers[i].receiveData.FragmentTable[0].FragmentBuffer = (char*)peers[i].receiveBuffer + remainingSize;
                    }
                }
            }
        }
    }
}

// Signaling the system to receive data from this connection
static void receiveData(unsigned int i, unsigned int salt)
{
    PROFILE_SCOPE();

    EFI_STATUS status;
    if (((unsigned long long)peers[i].tcp4Protocol) > 1)
    {
        if (!peers[i].isReceiving && peers[i].isConnectedAccepted && !peers[i].isClosing)
        {
            // check that receive buffer has enough space (less than BUFFER_SIZE is used)
            if ((((unsigned long long)peers[i].receiveData.FragmentTable[0].FragmentBuffer) - ((unsigned long long)peers[i].receiveBuffer)) < BUFFER_SIZE)
            {
                unsigned int receiveSize = BUFFER_SIZE - (unsigned int)(((unsigned long long)peers[i].receiveData.FragmentTable[0].FragmentBuffer) - ((unsigned long long)peers[i].receiveBuffer));
                peers[i].receiveData.DataLength = receiveSize;
                peers[i].receiveData.FragmentTable[0].FragmentLength = receiveSize;
                if (peers[i].receiveData.DataLength)
                {
                    EFI_TCP4_CONNECTION_STATE state;
                    if ((status = peers[i].tcp4Protocol->GetModeData(peers[i].tcp4Protocol, &state, NULL, NULL, NULL, NULL))
                        || state == Tcp4StateClosed)
                    {
                        closePeer(&peers[i], 0, PeerDisc::RECV_FIN_POLL);
                    }
                    else
                    {
                        // Initiate receiving data. At the same moment buffer may already contain a message of
                        // up to 16MB size, we don't wait for this message to be passed on to processing thread.
                        if (status = peers[i].tcp4Protocol->Receive(peers[i].tcp4Protocol, &peers[i].receiveToken))
                        {
                            if (status != EFI_CONNECTION_FIN)
                            {
                                logStatusToConsole(L"EFI_TCP4_PROTOCOL.Receive() fails", status, __LINE__);
                            }

                            closePeer(&peers[i], 0, PeerDisc::RECV_INIT_FAIL);
                        }
                        else
                        {
                            peers[i].isReceiving = TRUE;
                        }
                    }
                }
            }
        }
    }
}

// Checking the status last queued data for transmitting
static void processTransmittedData(unsigned int i, unsigned int salt)
{
    PROFILE_SCOPE();

    if (((unsigned long long)peers[i].tcp4Protocol) > 1)
    {
        // Special treatment for OM, make sure we close peer when the transmitting is stucked
        if (peers[i].isOracleMachineNode() && peers[i].isTransmitting && peers[i].omTransmitStartTime > 0)
        {
            unsigned long long elapsedSecs = (__rdtsc() - peers[i].omTransmitStartTime) / frequency;
            if (elapsedSecs > ORACLE_MACHINE_TRANSMITING_TIMEOUT_SECS)
            {
                peers[i].isTransmitting = FALSE;
                peers[i].omTransmitStartTime = 0; // mark as invalid
                closePeer(&peers[i]);
                return;  // Exit early
            }
        }

        // Same treatment for OC machine peers: close when transmitting is stuck
        if (peers[i].isOcMachineNode() && peers[i].isTransmitting && peers[i].ocTransmitStartTime > 0)
        {
            unsigned long long elapsedSecs = (__rdtsc() - peers[i].ocTransmitStartTime) / frequency;
            if (elapsedSecs > OC_MACHINE_TRANSMITING_TIMEOUT_SECS)
            {
                peers[i].isTransmitting = FALSE;
                peers[i].ocTransmitStartTime = 0; // mark as invalid
                closePeer(&peers[i]);
                return;  // Exit early
            }
        }

        // check if transmission is completed
        if (peers[i].transmitToken.CompletionToken.Status != -1)
        {
            peers[i].isTransmitting = FALSE;
            if (peers[i].transmitToken.CompletionToken.Status)
            {
                // transmission error
                peers[i].transmitToken.CompletionToken.Status = -1;
                closePeer(&peers[i], 0, PeerDisc::XMIT_ERR);
            }
            else
            {
                peers[i].transmitToken.CompletionToken.Status = -1;
                if (peers[i].isClosing)
                {
                    closePeer(&peers[i]);
                }
                else
                {
                    // success
                    numberOfTransmittedBytes += peers[i].transmitData.DataLength;
                    PeerDisc::noteTx(i, peers[i].transmitData.DataLength);

                    // Update OM activity time on successful transmit so the inactivity
                    // timer doesn't kill connections that are actively sending queries
                    if (peers[i].isOracleMachineNode())
                    {
                        peers[i].lastOMActivityTime = __rdtsc();
                    }
                    else if (peers[i].isOcMachineNode())
                    {
                        peers[i].lastOcActivityTime = __rdtsc();
                    }
                }
            }
        }
    }
}

// Enqueue data for transmitting
static void transmitData(unsigned int i, unsigned int salt)
{
    PROFILE_SCOPE();

    EFI_STATUS status;
    if (((unsigned long long)peers[i].tcp4Protocol) > 1)
    {
        if (peers[i].dataToTransmitSize && !peers[i].isTransmitting && peers[i].isConnectedAccepted && !peers[i].isClosing)
        {
            EFI_TCP4_CONNECTION_STATE state;
            if ((status = peers[i].tcp4Protocol->GetModeData(peers[i].tcp4Protocol, &state, NULL, NULL, NULL, NULL))
                || state == Tcp4StateClosed)
            {
                closePeer(&peers[i], 0, PeerDisc::XMIT_GETMODE);
            }
            else
            {
                // initiate transmission
                copyMem(peers[i].transmitData.FragmentTable[0].FragmentBuffer, peers[i].dataToTransmit, peers[i].transmitData.DataLength = peers[i].transmitData.FragmentTable[0].FragmentLength = peers[i].dataToTransmitSize);
                peers[i].dataToTransmitSize = 0;
                if (status = peers[i].tcp4Protocol->Transmit(peers[i].tcp4Protocol, &peers[i].transmitToken))
                {
                    logStatusToConsole(L"EFI_TCP4_PROTOCOL.Transmit() fails", status, __LINE__);
                    closePeer(&peers[i], 0, PeerDisc::XMIT_INIT_FAIL);
                }
                else
                {
                    peers[i].isTransmitting = TRUE;
                    if (peers[i].isOracleMachineNode())
                    {
                        peers[i].omTransmitStartTime = __rdtsc();
                    }
                    else if (peers[i].isOcMachineNode())
                    {
                        peers[i].ocTransmitStartTime = __rdtsc();
                    }
                }
            }
        }

    }
}

static void peerReceiveAndTransmit(unsigned int i, unsigned int salt)
{
    // poll to receive incoming data and transmit outgoing segments
    if (((unsigned long long)peers[i].tcp4Protocol) > 1)
    {
        PROFILE_SCOPE();
        //peers[i].tcp4Protocol->Poll(peers[i].tcp4Protocol);
        processReceivedData(i, salt);
        receiveData(i, salt);
        processTransmittedData(i, salt);
        transmitData(i, salt);
    }
}

static void peerReconnectIfInactive(unsigned int i, unsigned short port)
{
    PROFILE_SCOPE();

    EFI_STATUS status;
    if (!peers[i].tcp4Protocol)
    {
        // Save OM/OC close time before reset (for cooldown to work)
        unsigned long long savedOMCloseTime = peers[i].lastOMCloseTime;
        unsigned long long savedOcCloseTime = peers[i].lastOcCloseTime;

        peers[i].reset();
        // peer slot without active connection
        if (i < NUMBER_OF_OUTGOING_CONNECTIONS)
        {
            unsigned int connectionTimeout = 0;
            bool experimentSetting = false;
            // outgoing connection:
            peers[i].isIncommingConnection = FALSE;
            // Slot layout: [0,REGULAR) regular, [REGULAR,REGULAR+OM) OM, [REGULAR+OM,...) OC
            if (i >= FIRST_OC_MACHINE_CONNECTION_INDEX)
            {
                // OC machine slot
                peers[i].isOcMachine = TRUE;
                peers[i].address = ocIPv4Address[i - FIRST_OC_MACHINE_CONNECTION_INDEX];

                // Restore OC close time for cooldown check
                peers[i].lastOcCloseTime = savedOcCloseTime;

                // Mark it as failure
                peers[i].connectAcceptToken.CompletionToken.Status = -1;

                // Reconnection cooldown: prevent rapid reconnect cycles of the OC machine
                if (peers[i].lastOcCloseTime > 0)
                {
                    unsigned long long elapsedSecs = (__rdtsc() - peers[i].lastOcCloseTime) / frequency;
                    if (elapsedSecs < OC_RECONNECT_COOLDOWN_SECS)
                    {
                        return;  // Still in cooldown, don't attempt reconnection yet
                    }
                }
            }
            else if (i >= FIRST_OM_NODE_CONNECTION_INDEX)
            {
                peers[i].isOMNode = TRUE;
                peers[i].address = omIPv4Address[i - FIRST_OM_NODE_CONNECTION_INDEX];

                // Restore OM close time for cooldown check
                peers[i].lastOMCloseTime = savedOMCloseTime;

                // Mark it as failure
                peers[i].connectAcceptToken.CompletionToken.Status = -1;

                // Reconnection cooldown: prevent rapid reconnect cycles of the OM
                if (peers[i].lastOMCloseTime > 0)
                {
                    unsigned long long elapsedSecs = (__rdtsc() - peers[i].lastOMCloseTime) / frequency;
                    if (elapsedSecs < OM_RECONNECT_COOLDOWN_SECS)
                    {
                        return;  // Still in cooldown, don't attempt reconnection yet
                    }
                }

            }
            else
            {
                peers[i].isOMNode = FALSE;
                peers[i].isOcMachine = FALSE;
                // randomly select public peer and try to connect if we do not
                // yet have an outgoing connection to it. Empty list (e.g. only a
                // self/loopback --peers entry): random(0) divides by zero, and a
                // stale address would be redialled below, so clear it.
                refillPublicPeersFromInactive();
                const unsigned int publicPeerCount = numberOfPublicPeers;
                peers[i].address.u32 = publicPeerCount ? publicPeers[random(publicPeerCount)].address.u32 : 0;
            }

            if (peers[i].address.u32 != 0)
            {
                unsigned int j;
                for (j = 0; j < NUMBER_OF_OUTGOING_CONNECTIONS; j++)
                {
                    if (peers[j].tcp4Protocol && peers[j].address == peers[i].address)
                    {
                        break;
                    }
                }
                if (j == NUMBER_OF_OUTGOING_CONNECTIONS)
                {
                    if (peers[i].connectAcceptToken.NewChildHandle = getTcp4Protocol(peers[i].address.u8, port, &peers[i].tcp4Protocol))
                    {
                        peers[i].receiveData.FragmentTable[0].FragmentBuffer = peers[i].receiveBuffer;

                        if (status = peers[i].tcp4Protocol->Connect(peers[i].tcp4Protocol, (EFI_TCP4_CONNECTION_TOKEN*)&peers[i].connectAcceptToken))
                        {
                            logStatusToConsole(L"EFI_TCP4_PROTOCOL.Connect() fails", status, __LINE__);

                            bs->CloseProtocol(peers[i].connectAcceptToken.NewChildHandle, &tcp4ProtocolGuid, ih, NULL);
                            tcp4ServiceBindingProtocol->DestroyChild(tcp4ServiceBindingProtocol, peers[i].connectAcceptToken.NewChildHandle);
                            peers[i].tcp4Protocol = NULL;
                        }
                        else
                        {
                            peers[i].isConnectingAccepting = TRUE;
                            if (peers[i].isOracleMachineNode() || peers[i].isOcMachineNode())
                            {
                                peers[i].connectionStartTime = __rdtsc();
                            }
                        }
                    }
                    else
                    {
                        peers[i].tcp4Protocol = NULL;
                    }
                }
            }
        }
        else
        {
            // incoming connection:
            // Arm Accept on the cap plus a small reserve band; the reserve admits only loopback (checked at
            // accept time) so local RPC/tooling connects even when remotes saturate --max-inbound.
            if (!listOfPeersIsStatic && (i - NUMBER_OF_OUTGOING_CONNECTIONS) < (unsigned int)maxInboundAccepts + LOOPBACK_INBOUND_RESERVE)
            {
                peers[i].isIncommingConnection = TRUE;
                peers[i].receiveData.FragmentTable[0].FragmentBuffer = peers[i].receiveBuffer;

                if (status = peerTcp4Protocol->Accept(peerTcp4Protocol, &peers[i].connectAcceptToken, &peers[i]))
                {
                    logStatusToConsole(L"EFI_TCP4_PROTOCOL.Accept() fails", status, __LINE__);
                    if (peerTcp4Protocol == NULL)
                    {
                        logToConsole(L"[CRITICAL-Report to dev] peerTcp4Protocol is NULL. Please report!");
                    }
                    else
                    {
                        logStatusToConsole(L"[CRITICAL-Report to dev] peers[i].connectAcceptToken.CompletionToken.Status", peers[i].connectAcceptToken.CompletionToken.Status, __LINE__);
                    }
                    unsigned long long ptr = (unsigned long long)(&peers[i].connectAcceptToken);
                    setText(message, L"[CRITICAL-Report to dev] &peers[i].connectAcceptToken: ");
                    appendNumber(message, ptr, false);
                    logToConsole(message);
                }
                else
                {
                    peers[i].isConnectingAccepting = TRUE;
                    peers[i].tcp4Protocol = (EFI_TCP4_PROTOCOL*)1;
                }
            }
        }
    }
}
