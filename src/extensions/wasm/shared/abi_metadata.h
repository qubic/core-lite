#pragma once

// Canonical rows shared by WAMR registration and SDK metadata generation.
#define WASM_ABI_VERSION 6u

// G/H selects generated or handwritten adapters; Q/I selects QPI-bound or infrastructure calls.

#define WASM_SYSTEM_PROCEDURE_ROWS(X) \
    X(INITIALIZE,               0, initialize,               __initializeEmpty) \
    X(BEGIN_EPOCH,              1, beginEpoch,               __beginEpochEmpty) \
    X(END_EPOCH,                2, endEpoch,                 __endEpochEmpty) \
    X(BEGIN_TICK,               3, beginTick,                __beginTickEmpty) \
    X(END_TICK,                 4, endTick,                  __endTickEmpty) \
    X(PRE_RELEASE_SHARES,       5, preReleaseShares,         __preReleaseSharesEmpty) \
    X(PRE_ACQUIRE_SHARES,       6, preAcquireShares,         __preAcquireSharesEmpty) \
    X(POST_RELEASE_SHARES,      7, postReleaseShares,        __postReleaseSharesEmpty) \
    X(POST_ACQUIRE_SHARES,      8, postAcquireShares,        __postAcquireSharesEmpty) \
    X(POST_INCOMING_TRANSFER,   9, postIncomingTransfer,     __postIncomingTransferEmpty) \
    X(SET_SHAREHOLDER_PROPOSAL, 10, setShareholderProposal,  __setShareholderProposalEmpty) \
    X(SET_SHAREHOLDER_VOTES,    11, setShareholderVotes,     __setShareholderVotesEmpty)

#define WASM_LHOST_ABI_ROWS(GQ, GI, HQ, HI) \
    GI("beginFn",                             beginFn,                              "(i)")       \
    GI("endFn",                               endFn,                                "(i)")       \
    GI("markDirty",                           markDirty,                            "(i)")       \
    GI("pauseLog",                            pauseLog,                             "()")        \
    GI("resumeLog",                           resumeLog,                            "()")        \
    HI("acquireScratch",                      acquireScratch, w_acquireScratch,     "(Ii)i")     \
    HI("releaseScratch",                      releaseScratch, w_releaseScratch,     "(i)")       \
    HI("logBytes",                            logBytes,       w_logBytes,           "(iiii)")    \
    GI("k12",                                 k12,                                  "(iii)")     \
    HQ("transfer",                            transfer,       w_transfer,           "(iI)I")     \
    HQ("transferTyped",                       transferTyped,  w_transferTyped,      "(iIi)I")    \
    HQ("abort",                               abort,          w_abort,              "(i)")       \
    HQ("burn",                                burn,           w_burn,               "(Ii)I")     \
    GQ("epoch",                               epoch,                                "()i")       \
    GQ("tick",                                tick,                                 "()i")       \
    GQ("initialTick",                         initialTick,                          "()i")       \
    GQ("numberOfTickTransactions",            numberOfTickTransactions,             "()i")       \
    HQ("getEntity",                           getEntity,      w_getEntity,          "(ii)i")     \
    GQ("queryFeeReserve",                     queryFeeReserve,                      "(i)I")      \
    GQ("nextId",                              nextId,                               "(ii)")      \
    GQ("prevId",                              prevId,                               "(ii)")      \
    GQ("isContractId",                        isContractId,                         "(i)i")      \
    GQ("arbitrator",                          arbitrator,                           "(i)")       \
    GQ("computor",                            computor,                             "(ii)")      \
    GQ("day",                                 day,                                  "()i")       \
    GQ("year",                                year,                                 "()i")       \
    GQ("hour",                                hour,                                 "()i")       \
    GQ("minute",                              minute,                               "()i")       \
    GQ("month",                               month,                                "()i")       \
    GQ("second",                              second,                               "()i")       \
    GQ("millisecond",                         millisecond,                          "()i")       \
    GQ("now",                                 now,                                  "(i)")       \
    GQ("prevSpectrumDigest",                  prevSpectrumDigest,                   "(i)")       \
    GQ("prevUniverseDigest",                  prevUniverseDigest,                   "(i)")       \
    GQ("prevComputerDigest",                  prevComputerDigest,                   "(i)")       \
    GQ("isAssetIssued",                       isAssetIssued,                        "(iI)i")     \
    HQ("issueAsset",                          issueAsset,     w_issueAsset,         "(IiiII)I")  \
    GQ("numberOfShares",                      numberOfShares,                       "(iii)I")    \
    GQ("numberOfPossessedShares",             numberOfPossessedShares,              "(Iiiiii)I") \
    HQ("assetEnumerate",                      assetEnumerate,      w_assetEnumerate,             "(iiiiii)i") \
    HQ("transferShareOwnershipAndPossession", transferShareOwnershipAndPossession, w_transferShares, "(IiiiIi)I") \
    HQ("acquireShares",                       acquireShares,       w_acquireShares,              "(IiiiIiiI)I") \
    HQ("releaseShares",                       releaseShares,       w_releaseShares,              "(IiiiIiiI)I") \
    HQ("dayOfWeek",                           dayOfWeek,           w_dayOfWeek,                  "(iii)i")   \
    HQ("signatureValidity",                   signatureValidity,   w_signatureValidity,          "(iii)i")   \
    HQ("bidInIPO",                            bidInIPO,            w_bidInIPO,                   "(iIi)I")   \
    HQ("ipoBidId",                            ipoBidId,            w_ipoBidId,                   "(iii)")    \
    HQ("ipoBidPrice",                         ipoBidPrice,         w_ipoBidPrice,                "(ii)I")    \
    HQ("computeMiningFunction",               computeMiningFunction, w_computeMiningFunction,    "(iiii)")   \
    HQ("initMiningSeed",                      initMiningSeed,      w_initMiningSeed,             "(i)")      \
    HQ("getOracleQueryStatus",                getOracleQueryStatus, w_getOracleQueryStatus,      "(I)i")     \
    GQ("getOcInvocationStatus",                getOcInvocationStatus,                              "(I)i")     \
    HQ("invokeOc",                            invokeOc,             w_invokeOc,                    "(iii)I")   \
    HQ("unsubscribeOracle",                   unsubscribeOracle,   w_unsubscribeOracle,          "(i)i")     \
    HQ("queryOracle",                         queryOracle,         w_queryOracle,                "(iiiiiiI)I") \
    HQ("subscribeOracle",                     subscribeOracle,     w_subscribeOracle,            "(iiiiiiiiI)i") \
    HQ("getOracleQuery",                      getOracleQuery,      w_getOracleQuery,             "(Iii)i")   \
    HQ("getOracleReply",                      getOracleReply,      w_getOracleReply,             "(Iii)i")   \
    HQ("distributeDividends",                 distributeDividends, w_distributeDividends,        "(I)i")     \
    HQ("liteCallFunction",                    liteCallFunction,    w_liteCallFunction,           "(iiiiii)i")  \
    HQ("liteInvokeProcedure",                 liteInvokeProcedure, w_liteInvokeProcedure,        "(iiiiiiI)i") \
    HQ("liteSetShareholderProposal",          setShareholderProposal, w_liteSetShareholderProposal, "(iiI)i") \
    HQ("liteSetShareholderVotes",             setShareholderVotes,    w_liteSetShareholderVotes,    "(iiiI)i") \
    HQ("cheat",                               cheat,               w_cheat,                      "(iIIii)I")
