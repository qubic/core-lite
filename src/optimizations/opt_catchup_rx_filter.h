#pragma once

// Catch-up receive filter (AUX only).
//
// A lite far behind the network still receives the live network's votes (BroadcastTick) for
// ticks hours ahead, and checks each signature in a request processor. On top of the replies to
// its own requests, this keeps the request queue full: the replies the node waits for (tickData,
// txs, votes of the next ticks) are discarded with the rest, every discard sends a TryAgain, and
// request processors spend their time contending instead of catching up.
// While far behind, votes for ticks more than KEEP_AHEAD ticks above our tick are dropped on
// receive, before the dejavu hash and the queue (no TryAgain). Votes within the window are kept;
// farther ones are requested by the future-tick prefetch when needed.
// Future tick data is never dropped on receive: it keeps Peer::peerReportedTick (our estimate of the
// network tip, which arms this filter) up to date, and it is a tiny share of the traffic. Above the
// window it is checked and relayed as usual but not written to tick data storage (relayOnly): storing
// it held tickDataLock for data needed only hours later. The future-tick prefetch (at most 20 ticks
// ahead, inside the window) requests it again when the node gets close.
// The main loop arms the filter (gDropAboveTick != 0) only while catching up; in sync it is off.

#include <atomic>

#include "network_messages/header.h"
#include "network_messages/tick.h"

namespace CatchupRxFilter
{
constexpr unsigned int KEEP_AHEAD = 64;

// 0 = off; otherwise drop votes for ticks above this value.
static std::atomic<unsigned int> gDropAboveTick{0};

// Called on the receive path for every complete packet; true = drop it.
static inline bool shouldDrop(const RequestResponseHeader* header)
{
    const unsigned int dropAbove = gDropAboveTick.load(std::memory_order_relaxed);
    return dropAbove
        && header->type() == BroadcastTick::type()
        && header->size() == sizeof(RequestResponseHeader) + sizeof(BroadcastTick)
        && ((const BroadcastTick*)(header + 1))->tick.tick > dropAbove;
}

// Called for checked data of a tick; true = relay it but do not store it (far ahead while catching up).
static inline bool relayOnly(unsigned int tick)
{
    const unsigned int dropAbove = gDropAboveTick.load(std::memory_order_relaxed);
    return dropAbove && tick > dropAbove;
}
}
