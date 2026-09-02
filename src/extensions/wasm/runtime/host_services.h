#pragma once

// Host-service table bound to the stable lhost registry.
#ifdef LITE_WASM_SC

#include "extensions/wasm/runtime/qpi_services.h"
#include "extensions/wasm/runtime/oracle_services.h"

namespace Wasm::Runtime
{

// Designated initialization keeps member/order drift a compile-time error.
static HostServices hostServices =
{
    .abiVersion = WASM_ABI_VERSION,
    .beginFn = &beginFunction,
    .endFn = &endFunction,
    .markDirty = &markContractDirty,
    .pauseLog = &pauseLog,
    .resumeLog = &resumeLog,
    .acquireScratch = &acquireScratch,
    .releaseScratch = &releaseScratch,
    .logBytes = &logBytes,
    .k12 = &hashK12,
    .transfer = &transfer,
    .transferTyped = &transferTyped,
    .abort = &abortCall,
    .burn = &burn,
    .epoch = &epoch,
    .tick = &tick,
    .initialTick = &initialTick,
    .numberOfTickTransactions = &numberOfTickTransactions,
    .getEntity = &getEntity,
    .queryFeeReserve = &queryFeeReserve,
    .nextId = &nextId,
    .prevId = &previousId,
    .isContractId = &isContractId,
    .arbitrator = &arbitrator,
    .computor = &computor,
    .day = &day,
    .year = &year,
    .hour = &hour,
    .minute = &minute,
    .month = &month,
    .second = &second,
    .millisecond = &millisecond,
    .now = &now,
    .prevSpectrumDigest = &previousSpectrumDigest,
    .prevUniverseDigest = &previousUniverseDigest,
    .prevComputerDigest = &previousComputerDigest,
    .isAssetIssued = &isAssetIssued,
    .issueAsset = &issueAsset,
    .numberOfShares = &numberOfShares,
    .numberOfPossessedShares = &numberOfPossessedShares,
    .assetEnumerate = &enumerateAssets,
    .transferShareOwnershipAndPossession = &transferShareOwnershipAndPossession,
    .acquireShares = &acquireShares,
    .releaseShares = &releaseShares,
    .dayOfWeek = &dayOfWeek,
    .signatureValidity = &signatureValidity,
    .bidInIPO = &bidInIPO,
    .ipoBidId = &ipoBidId,
    .ipoBidPrice = &ipoBidPrice,
    .computeMiningFunction = &computeMiningFunction,
    .initMiningSeed = &initMiningSeed,
    .getOracleQueryStatus = &getOracleQueryStatus,
    .getOcInvocationStatus = &getOcInvocationStatus,
    .invokeOc = &invokeOc,
    .unsubscribeOracle = &unsubscribeOracle,
    .queryOracle = &queryOracle,
    .subscribeOracle = &subscribeOracle,
    .getOracleQuery = &getOracleQuery,
    .getOracleReply = &getOracleReply,
    .distributeDividends = &distributeDividends,
    .liteCallFunction = &callContractFunction,
    .liteInvokeProcedure = &invokeContractProcedure,
    .setShareholderProposal = &setShareholderProposal,
    .setShareholderVotes = &setShareholderVotes,
    .cheat = &cheat,
};

} // namespace Wasm::Runtime

#endif // LITE_WASM_SC
