#pragma once

// QPI method definitions forwarded through the active lhost imports.
#ifdef LITE_WASM_TU_BUILD

#include "extensions/wasm/sdk/lhost_imports.h"

// ---- QpiContext method forwarders (stable host ABI) ----
template <typename T>
QPI::id QPI::QpiContextFunctionCall::K12(const T& data) const
{
    QPI::id digest;

    lh_k12(&data, sizeof(T), &digest);
    return digest;
}

long long QPI::QpiContextProcedureCall::transfer(const m256i& destination, long long amount) const
{
    return lh_transfer(&destination, amount);
}

long long QPI::QpiContextProcedureCall::__transfer(const m256i& destination, long long amount, unsigned char transferType) const
{
    return lh_transferTyped(&destination, amount, transferType);
}

void QPI::QpiContextFunctionCall::__qpiAbort(unsigned int errorCode) const
{
    lh_abort(errorCode);
}

long long QPI::QpiContextProcedureCall::burn(long long amount, unsigned int contractIndex) const
{
    return lh_burn(amount, contractIndex);
}

unsigned short QPI::QpiContextFunctionCall::epoch() const
{
    return (unsigned short)lh_epoch();
}

unsigned int QPI::QpiContextFunctionCall::tick() const
{
    return lh_tick();
}

unsigned int QPI::QpiContextFunctionCall::initialTick() const
{
    return lh_initialTick();
}

int QPI::QpiContextFunctionCall::numberOfTickTransactions() const
{
    return lh_numberOfTickTransactions();
}

QPI::bit QPI::QpiContextFunctionCall::getEntity(const m256i& id, QPI::Entity& entity) const
{
    return (QPI::bit)lh_getEntity(&id, &entity);
}

long long QPI::QpiContextFunctionCall::queryFeeReserve(unsigned int contractIndex) const
{
    return lh_queryFeeReserve(contractIndex);
}

m256i QPI::QpiContextFunctionCall::nextId(const m256i& current) const
{
    m256i next;

    lh_nextId(&current, &next);
    return next;
}

m256i QPI::QpiContextFunctionCall::prevId(const m256i& current) const
{
    m256i previous;

    lh_prevId(&current, &previous);
    return previous;
}

QPI::bit QPI::QpiContextFunctionCall::isContractId(const QPI::id& id) const
{
    return (QPI::bit)lh_isContractId(&id);
}

QPI::id QPI::QpiContextFunctionCall::arbitrator() const
{
    m256i id;

    lh_arbitrator(&id);
    return id;
}

QPI::id QPI::QpiContextFunctionCall::computor(unsigned short index) const
{
    m256i id;

    lh_computor(index, &id);
    return id;
}

unsigned char QPI::QpiContextFunctionCall::day() const
{
    return (unsigned char)lh_day();
}

unsigned char QPI::QpiContextFunctionCall::year() const
{
    return (unsigned char)lh_year();
}

unsigned char QPI::QpiContextFunctionCall::hour() const
{
    return (unsigned char)lh_hour();
}

unsigned char QPI::QpiContextFunctionCall::minute() const
{
    return (unsigned char)lh_minute();
}

unsigned char QPI::QpiContextFunctionCall::month() const
{
    return (unsigned char)lh_month();
}

unsigned char QPI::QpiContextFunctionCall::second() const
{
    return (unsigned char)lh_second();
}

unsigned short QPI::QpiContextFunctionCall::millisecond() const
{
    return (unsigned short)lh_millisecond();
}

QPI::DateAndTime QPI::QpiContextFunctionCall::now() const
{
    QPI::DateAndTime dateAndTime;

    lh_now(&dateAndTime);
    return dateAndTime;
}

m256i QPI::QpiContextFunctionCall::getPrevSpectrumDigest() const
{
    m256i digest;

    lh_prevSpectrumDigest(&digest);
    return digest;
}

m256i QPI::QpiContextFunctionCall::getPrevUniverseDigest() const
{
    m256i digest;

    lh_prevUniverseDigest(&digest);
    return digest;
}

m256i QPI::QpiContextFunctionCall::getPrevComputerDigest() const
{
    m256i digest;

    lh_prevComputerDigest(&digest);
    return digest;
}

bool QPI::QpiContextFunctionCall::isAssetIssued(const m256i& issuer, unsigned long long assetName) const
{
    return lh_isAssetIssued(&issuer, assetName);
}

long long QPI::QpiContextProcedureCall::issueAsset(
    unsigned long long assetName,
    const QPI::id& issuer,
    signed char decimals,
    long long numberOfShares,
    unsigned long long unitOfMeasurement) const
{
    return lh_issueAsset(assetName, &issuer, (unsigned int)(unsigned char)decimals, numberOfShares, unitOfMeasurement);
}

long long QPI::QpiContextFunctionCall::numberOfShares(const QPI::Asset& asset, const QPI::AssetOwnershipSelect& ownership,
    const QPI::AssetPossessionSelect& possession) const
{
    return lh_numberOfShares(&asset, &ownership, &possession);
}

long long QPI::QpiContextFunctionCall::numberOfPossessedShares(
    unsigned long long assetName,
    const m256i& issuer,
    const m256i& owner,
    const m256i& possessor,
    unsigned short ownershipManagingContractIndex,
    unsigned short possessionManagingContractIndex) const
{
    return lh_numberOfPossessedShares(assetName, &issuer, &owner, &possessor, ownershipManagingContractIndex, possessionManagingContractIndex);
}

long long QPI::QpiContextProcedureCall::transferShareOwnershipAndPossession(
    unsigned long long assetName,
    const m256i& issuer,
    const m256i& owner,
    const m256i& possessor,
    long long numberOfShares,
    const m256i& newOwnerAndPossessor) const
{
    return lh_transferShares(assetName, &issuer, &owner, &possessor, numberOfShares, &newOwnerAndPossessor);
}

long long QPI::QpiContextProcedureCall::acquireShares(
    const QPI::Asset& asset,
    const m256i& owner,
    const m256i& possessor,
    long long numberOfShares,
    unsigned short sourceOwnershipManagingContractIndex,
    unsigned short sourcePossessionManagingContractIndex,
    long long offeredFee) const
{
    return lh_acquireShares(asset.assetName, &asset.issuer, &owner, &possessor, numberOfShares, sourceOwnershipManagingContractIndex, sourcePossessionManagingContractIndex, offeredFee);
}

long long QPI::QpiContextProcedureCall::releaseShares(
    const QPI::Asset& asset,
    const m256i& owner,
    const m256i& possessor,
    long long numberOfShares,
    unsigned short destinationOwnershipManagingContractIndex,
    unsigned short destinationPossessionManagingContractIndex,
    long long offeredFee) const
{
    return lh_releaseShares(asset.assetName, &asset.issuer, &owner, &possessor, numberOfShares, destinationOwnershipManagingContractIndex, destinationPossessionManagingContractIndex, offeredFee);
}

unsigned char QPI::QpiContextFunctionCall::dayOfWeek(unsigned char year, unsigned char month, unsigned char day) const
{
    return (unsigned char)lh_dayOfWeek(year, month, day);
}

QPI::bit QPI::QpiContextFunctionCall::signatureValidity(const m256i& entity, const m256i& digest, const QPI::Array<QPI::sint8, 64>& signature) const
{
    return lh_signatureValidity(&entity, &digest, &signature) != 0;
}

long long QPI::QpiContextProcedureCall::bidInIPO(unsigned int ipoContractIndex, long long price, unsigned int quantity) const
{
    return lh_bidInIPO(ipoContractIndex, price, quantity);
}

m256i QPI::QpiContextFunctionCall::ipoBidId(unsigned int ipoContractIndex, unsigned int ipoBidIndex) const
{
    m256i id;

    lh_ipoBidId(ipoContractIndex, ipoBidIndex, &id);
    return id;
}

long long QPI::QpiContextFunctionCall::ipoBidPrice(unsigned int ipoContractIndex, unsigned int ipoBidIndex) const
{
    return lh_ipoBidPrice(ipoContractIndex, ipoBidIndex);
}

m256i QPI::QpiContextFunctionCall::computeMiningFunction(const m256i miningSeed, const m256i publicKey, const m256i nonce) const
{
    m256i result;

    lh_computeMiningFunction(&miningSeed, &publicKey, &nonce, &result);
    return result;
}

void QPI::QpiContextFunctionCall::initMiningSeed(const m256i miningSeed) const
{
    lh_initMiningSeed(&miningSeed);
}

unsigned char QPI::QpiContextFunctionCall::getOracleQueryStatus(long long queryId) const
{
    return (unsigned char)lh_getOracleQueryStatus(queryId);
}

unsigned char QPI::QpiContextFunctionCall::getOcInvocationStatus(long long invocationId) const
{
    return (unsigned char)lh_getOcInvocationStatus(invocationId);
}

template <typename OcInterface>
QPI::sint64 QPI::QpiContextProcedureCall::__qpiInvokeOC(const typename OcInterface::OcRequest& request) const
{
    static_assert(OcInterface::ocInterfaceIndex < OCI::ocInterfacesCount);
    static_assert(OCI::ocInterfaces[OcInterface::ocInterfaceIndex].requestSize == sizeof(typename OcInterface::OcRequest));
    return lh_invokeOc(OcInterface::ocInterfaceIndex, &request, (unsigned int)sizeof(typename OcInterface::OcRequest));
}

bool QPI::QpiContextProcedureCall::unsubscribeOracle(int oracleSubscriptionId) const
{
    return lh_unsubscribeOracle(oracleSubscriptionId) != 0;
}

template <typename OracleInterface, typename ContractStateType, typename LocalsType>
QPI::sint64 QPI::QpiContextProcedureCall::__qpiQueryOracle(const typename OracleInterface::OracleQuery& query, void (*)(const QPI::QpiContextProcedureCall&,
        ContractStateType&, QPI::OracleNotificationInput<OracleInterface>&, QPI::NoData&, LocalsType&), unsigned int notificationProcedureId,
    unsigned int timeoutMilliseconds) const
{
    return lh_queryOracle(OracleInterface::oracleInterfaceIndex, &query, (unsigned int)sizeof(typename OracleInterface::OracleQuery), (unsigned int)sizeof(typename OracleInterface::OracleReply), notificationProcedureId, timeoutMilliseconds, OracleInterface::getQueryFee(query));
}

template <typename OracleInterface, typename ContractStateType, typename LocalsType>
QPI::sint32 QPI::QpiContextProcedureCall::__qpiSubscribeOracle(const typename OracleInterface::OracleQuery& query, void (*)(const QPI::QpiContextProcedureCall&,
        ContractStateType&, QPI::OracleNotificationInput<OracleInterface>&, QPI::NoData&, LocalsType&), unsigned int notificationProcedureId,
    unsigned int notificationPeriodInMilliseconds, bool notifyWithPreviousReply) const
{
    static_assert(sizeof(query.timestamp) == sizeof(QPI::DateAndTime));
    return lh_subscribeOracle(OracleInterface::oracleInterfaceIndex, &query, (unsigned int)sizeof(typename OracleInterface::OracleQuery), (unsigned int)sizeof(typename OracleInterface::OracleReply), (unsigned int)__builtin_offsetof(OracleInterface::OracleQuery, timestamp), notificationProcedureId, notificationPeriodInMilliseconds, notifyWithPreviousReply ? 1u : 0u, OracleInterface::getSubscriptionFee(query, notificationPeriodInMilliseconds));
}

template <typename OracleInterface>
bool QPI::QpiContextFunctionCall::getOracleQuery(QPI::sint64 queryId, typename OracleInterface::OracleQuery& query) const
{
    return lh_getOracleQuery(queryId, &query, (unsigned int)sizeof(typename OracleInterface::OracleQuery)) != 0;
}

template <typename OracleInterface>
bool QPI::QpiContextFunctionCall::getOracleReply(QPI::sint64 queryId, typename OracleInterface::OracleReply& reply) const
{
    return lh_getOracleReply(queryId, &reply, (unsigned int)sizeof(typename OracleInterface::OracleReply)) != 0;
}

bool QPI::QpiContextProcedureCall::distributeDividends(long long amountPerShare) const
{
    return lh_distributeDividends(amountPerShare);
}

QPI::uint16 QPI::QpiContextProcedureCall::setShareholderProposal(QPI::uint16 contractIndex, const QPI::Array<QPI::uint8, 1024>& proposalDataBuffer,
    QPI::sint64 invocationReward) const
{
    return (QPI::uint16)lh_liteSetShareholderProposal(contractIndex, &proposalDataBuffer, invocationReward);
}

bool QPI::QpiContextProcedureCall::setShareholderVotes(QPI::uint16 contractIndex, const QPI::ProposalMultiVoteDataV1& voteData,
    QPI::sint64 invocationReward) const
{
    return lh_liteSetShareholderVotes(contractIndex, &voteData, sizeof(voteData), invocationReward) != 0;
}


#endif // LITE_WASM_TU_BUILD
