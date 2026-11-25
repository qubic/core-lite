#pragma once

// Lite glue around the bpp9000 BTC price feed: the embedded bootstrap, a synthetic testnet series, the shift limit an
// anchor allows, and a one-shot check that this node's series matches the quorum's.

#include "mining/price_feed.h"
#include "extensions/btc_price_bootstrap.generated.h"

static_assert(BTC_PRICE_BOOTSTRAP_SIZE == sizeof(BtcPriceImage), "embedded bootstrap is not a price image");

#ifndef TESTNET
#include "extensions/btc_price_series.generated.h"

static_assert(BTC_PRICE_SERIES_SIZE == sizeof(BtcPriceImage), "embedded series is not a price image");
#endif

// Set after a start from files until the first quorum digest has been compared; trust waits for it.
static bool gBtcSeriesCheckPending = false;
// The node's series disagreed with the quorum at start; claimed ant scores are no longer trusted.
static bool gBtcSeriesMismatch = false;

#ifdef TESTNET
// Deterministic series for nodes without an oracle machine: a random walk over the hours before the epoch's
// first hour, so every lite testnet node of a build holds the same task data. Main thread only.
static void loadSyntheticBtcPriceSeries(unsigned short epoch, unsigned int initialTick)
{
    // Epoch e starts e weeks after hour 0 (2022-04-13 12:00 UTC); its switch knows every hour before that one.
    const unsigned int nextHour = (unsigned int)epoch * 7 * 24;
    if (nextHour < BTC_PRICE_MIN_POINTS)
    {
        return;
    }

    // The hash returns no more than its own state per call, so the steps are drawn 32 at a time, numbered in the seed.
    unsigned char steps[32];
    unsigned char seed[32] = { 0 };
    copyMem(seed, "lite-testnet-btc-series", 23);
    copyMem(seed + 24, &epoch, sizeof(epoch));

    BtcPriceImage& image = gBtcPriceFeedIoBuffer.image;
    setMem(&image, sizeof(image), 0);
    image.nextHour = nextHour;
    image.baseHour = nextHour - 1;
    long long priceE8 = 60000LL * BTC_FEED_PRICE_SCALE;
    for (unsigned int i = 0; i < BTC_PRICE_MIN_POINTS; i++)
    {
        if (i % sizeof(steps) == 0)
        {
            const unsigned int stepBlock = i / (unsigned int)sizeof(steps);
            copyMem(seed + 28, &stepBlock, sizeof(stepBlock));
            KangarooTwelve(seed, sizeof(seed), steps, sizeof(steps));
        }
        // Steps of up to 128 USDT keep the walk far from zero over a year.
        priceE8 += ((long long)steps[i % sizeof(steps)] - 128) * BTC_FEED_PRICE_SCALE;
        if (priceE8 < BTC_FEED_PRICE_SCALE)
        {
            priceE8 = BTC_FEED_PRICE_SCALE;
        }
        const unsigned int hour = nextHour - BTC_PRICE_MIN_POINTS + i;
        BtcPricePoint& point = image.points[hour & (BTC_PRICE_RING_SIZE - 1)];
        point.hour = hour;
        point.priceE8 = priceE8;
    }
    btcFeedSetImageHeader(image, BTC_PRICE_KIND_SWITCH, epoch, initialTick);
    gBtcPriceFeed.adoptImage(image);
    logToConsole(L"BTC price series synthesized for testnet");
}
#endif

// The bootstrap ships inside the binary; the loader reads it from disk and accepts only the pinned hash.
static void writeEmbeddedBtcPriceBootstrap()
{
    if (save(BTC_PRICE_BOOTSTRAP_FILE_NAME, BTC_PRICE_BOOTSTRAP_SIZE, BTC_PRICE_BOOTSTRAP_BYTES, NULL) != (long long)BTC_PRICE_BOOTSTRAP_SIZE)
    {
        logToConsole(L"Failed to write the embedded BTC price bootstrap!");
    }
}

// The epoch's switch series ships inside the binary, for a start at the epoch's first tick without the file.
// Written only when the loader would take it and the node has no file of that name. Main thread only.
static void writeEmbeddedBtcPriceSeries()
{
#ifndef TESTNET
    BtcPriceImage& image = gBtcPriceFeedIoBuffer.image;
    copyMem(&image, BTC_PRICE_SERIES_BYTES, sizeof(image));
    if (system.tick != system.initialTick || image.kind == BTC_PRICE_KIND_BOOTSTRAP || !btcFeedImageIsValid(image)
        || !btcFeedImageIsAcceptable(image, system.epoch, system.initialTick, TICK_IS_FIRST_TICK_OF_EPOCH != 0))
    {
        return;
    }

    addEpochToFileName(BTC_PRICE_SERIES_FILE_NAME, sizeof(BTC_PRICE_SERIES_FILE_NAME) / sizeof(BTC_PRICE_SERIES_FILE_NAME[0]), (short)system.epoch);
    if (getFileSize(BTC_PRICE_SERIES_FILE_NAME) >= 0)
    {
        return;
    }

    if (save(BTC_PRICE_SERIES_FILE_NAME, BTC_PRICE_SERIES_SIZE, BTC_PRICE_SERIES_BYTES, NULL) == (long long)BTC_PRICE_SERIES_SIZE)
    {
        logToConsole(L"BTC price series written from the copy inside the binary");
    }
    else
    {
        logToConsole(L"Failed to write the embedded BTC price series!");
    }
#endif
}

// Shift limit of a solution anchored at anchorTick. False without a price series.
static bool antShiftLimitAt(unsigned int anchorTick, unsigned int& shiftLimit)
{
    unsigned char tail[BPP9000_SHIFT_CAP];
    unsigned int baseHour = 0;
    return gBtcPriceFeed.taskTailAt(anchorTick, tail, shiftLimit, baseHour);
}

// First quorum digest after a start from files. Valid only while no trusted score has been folded, which
// gBtcSeriesCheckPending guarantees by holding trust off until this runs.
static void checkBtcSeriesAgainstQuorum(unsigned int ownResourceTestingDigest, unsigned int quorumResourceTestingDigest)
{
    gBtcSeriesCheckPending = false;
    if (ownResourceTestingDigest != quorumResourceTestingDigest)
    {
        gBtcSeriesMismatch = true;
        logToConsole(L"ERROR: mining digest differs from the quorum after start - the BTC price series file "
                     L"(btcPriceSeries.<epoch> / btc_price_bootstrap.bin) likely differs. Claimed ant scores are no longer trusted.");
    }
}
