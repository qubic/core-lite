#pragma once

// Catch-up receive filter (AUX only).
//
// A lite far behind the network still receives the live network's votes (BroadcastTick) for
// ticks hours ahead, and checks each signature in a request processor. On top of the replies to
// its own requests, this keeps the request queue full: the replies the node waits for (tickData,
// txs, votes of the next ticks) are discarded with the rest, every discard sends a TryAgain, and
// request processors spend their time contending instead of catching up.
// While far behind, votes for ticks more than KEEP_AHEAD ticks above our tick are shed first when
// the request queue is under pressure (more than PRESSURE_QUEUED_ELEMENTS queued): they are dropped
// on receive, before the dejavu hash and the queue (no TryAgain). Below that, they are checked and
// relayed as usual, but not written to tick storage (relayOnly): storing them is what slowed
// catch-up, not relaying them.
// Votes within the window are always stored; farther ones are requested by the future-tick
// prefetch when needed.
// Future tick data is not filtered: it keeps Peer::peerReportedTick (our estimate of the network
// tip, which arms this filter) up to date, and it is a tiny share of the traffic.
// The main loop arms the filter (gDropAboveTick != 0) only while catching up; in sync it is off.

#include <atomic>

#include "network_messages/header.h"
#include "network_messages/tick.h"

namespace CatchupRxFilter
{
constexpr unsigned int KEEP_AHEAD = 64;
// ~0.5 s of request processing at catch-up rates; far-future votes are only shed above this.
constexpr unsigned short PRESSURE_QUEUED_ELEMENTS = 4096;

// 0 = off; otherwise drop votes for ticks above this value.
static std::atomic<unsigned int> gDropAboveTick{0};

// Called on the receive path for every complete packet; true = drop it.
static inline bool shouldDrop(const RequestResponseHeader* header, unsigned short queuedElements)
{
    const unsigned int dropAbove = gDropAboveTick.load(std::memory_order_relaxed);
    return dropAbove
        && queuedElements > PRESSURE_QUEUED_ELEMENTS
        && header->type() == BroadcastTick::type()
        && header->size() == sizeof(RequestResponseHeader) + sizeof(BroadcastTick)
        && ((const BroadcastTick*)(header + 1))->tick.tick > dropAbove;
}

// Called for a checked vote; true = relay it but do not store it (far ahead while catching up).
static inline bool relayOnly(unsigned int tick)
{
    const unsigned int dropAbove = gDropAboveTick.load(std::memory_order_relaxed);
    return dropAbove && tick > dropAbove;
}
}
