#pragma once
// Host adapters for the Wasm oracle imports.
// Query fees and caller identity are derived from host-owned state.
// Subscription fees and timestamp offsets are supplied by compiled Wasm in this development runtime.

namespace Wasm::Runtime
{

static inline unsigned int oracleContractIndex(const void* context)
{
    return ((const QPI::QpiContextProcedureCall*)context)->__qpiCurrentContractIndex();
}

static const UserProcedureRegistry::UserProcedureData* oracleNotification(unsigned int contractIndex, unsigned int notificationProcedureId,
    unsigned int replySize)
{
    const UserProcedureRegistry::UserProcedureData* notification;
    // a row disabled by an unload has no procedure to notify
    if (!userProcedureRegistry || !(notification = userProcedureRegistry->get(notificationProcedureId)) || !notification->procedure
        || notification->contractIndex != contractIndex || notification->inputSize != 16 + replySize)
    {
        return nullptr;
    }
    return notification;
}

static void callOracleNotification(const void* context, const UserProcedureRegistry::UserProcedureData& notification, long long queryId, int subscriptionId,
    unsigned char status, const void* reply, unsigned int replySize)
{
    alignas(8) unsigned char input[16 + MAX_ORACLE_REPLY_SIZE] = {};
    copyMem(input, &queryId, sizeof(queryId));
    copyMem(input + 8, &subscriptionId, sizeof(subscriptionId));
    input[12] = status;
    if (reply && replySize)
    {
        copyMem(input + 16, reply, replySize);
    }

    QPI::NoData output;
    notification.procedure(*(const QPI::QpiContextProcedureCall*)context, contractStates[notification.contractIndex], input, &output, nullptr);
}

static long long queryOracle(const void* context, unsigned int interfaceIndex, const void* query, unsigned int querySize, unsigned int replySize,
    unsigned int notificationProcedureId, unsigned int timeoutMilliseconds,
    long long /*untrustedWasmFee*/)
{
    if (!context || !query || interfaceIndex >= OI::oracleInterfacesCount)
    {
        return -1;
    }

    if (!OI::getOracleQueryFeeFunc[interfaceIndex])
    {
        return -1;
    }

    if (querySize != OI::oracleInterfaces[interfaceIndex].querySize || replySize != OI::oracleInterfaces[interfaceIndex].replySize)
    {
        return -1;
    }

    const unsigned int contractIndex = oracleContractIndex(context);
    const auto* notification = oracleNotification(contractIndex, notificationProcedureId, replySize);
    if (!notification)
    {
        return -1;
    }

    const m256i contractId = m256i(contractIndex, 0, 0, 0);
    const long long fee = OI::getOracleQueryFeeFunc[interfaceIndex](query);
    const int contractSpectrumIndex = ::spectrumIndex(contractId);

    if (fee >= MIN_ORACLE_QUERY_FEE && contractSpectrumIndex >= 0 && decreaseEnergy(contractSpectrumIndex, fee))
    {
        const QuTransfer quTransfer = { contractId, m256i::zero(), fee };

        logger.logQuTransfer(quTransfer);

        const long long queryId = oracleEngine.startContractQuery((uint16_t)contractIndex, interfaceIndex, query, (uint16_t)querySize, timeoutMilliseconds, notificationProcedureId);
        if (queryId >= 0)
        {
            return queryId;
        }
        if (fee > 0)
        {
            oracleEngine.refundFees(contractId, fee);
        }
    }

    callOracleNotification(context, *notification, -1, -1, ORACLE_QUERY_STATUS_UNKNOWN, nullptr, replySize);
    return -1;
}

static int subscribeOracle(const void* context, unsigned int interfaceIndex, const void* query, unsigned int querySize, unsigned int replySize,
    unsigned int timestampOffset, unsigned int notificationProcedureId, unsigned int periodMilliseconds, unsigned int notifyPrevious, long long fee)
{
    if (!context || !query || interfaceIndex >= OI::oracleInterfacesCount || querySize != OI::oracleInterfaces[interfaceIndex].querySize
        || replySize != OI::oracleInterfaces[interfaceIndex].replySize || querySize < sizeof(QPI::DateAndTime)
        || timestampOffset > querySize - sizeof(QPI::DateAndTime))
    {
        return -1;
    }

    const unsigned int contractIndex = oracleContractIndex(context);
    const auto* notification = oracleNotification(contractIndex, notificationProcedureId, replySize);
    if (!notification)
    {
        return -1;
    }

    const m256i contractId = m256i(contractIndex, 0, 0, 0);
    const int contractSpectrumIndex = ::spectrumIndex(contractId);
    if (fee >= MIN_ORACLE_SUBSCRIPTION_FEE && contractSpectrumIndex >= 0 && decreaseEnergy(contractSpectrumIndex, fee))
    {
        const QuTransfer quTransfer = { contractId, m256i::zero(), fee };
        logger.logQuTransfer(quTransfer);

        const int subscriptionId = oracleEngine.startContractSubscription((uint16_t)contractIndex, interfaceIndex, query, (uint16_t)querySize, periodMilliseconds, notificationProcedureId, (uint16_t)timestampOffset);
        if (subscriptionId >= 0)
        {
            if (notifyPrevious)
            {
                const OracleSubscription* subscription = oracleEngine.getOracleSubscription(subscriptionId);
                if (subscription && subscription->lastRevealedQueryId >= 0)
                {
                    alignas(8) unsigned char reply[MAX_ORACLE_REPLY_SIZE] = {};
                    if (oracleEngine.getOracleReply(subscription->lastRevealedQueryId, reply, (uint16_t)replySize))
                    {
                        callOracleNotification(context, *notification, subscription->lastRevealedQueryId, subscriptionId, ORACLE_QUERY_STATUS_SUCCESS, reply, replySize);
                    }
                }
            }
            return subscriptionId;
        }
        if (fee > 0)
        {
            oracleEngine.refundFees(contractId, fee);
        }
    }

    callOracleNotification(context, *notification, -1, -1, ORACLE_QUERY_STATUS_UNKNOWN, nullptr, replySize);
    return -1;
}

static unsigned int getOracleQuery(
    const void* /*context*/,
    long long queryId, void* output, unsigned int size)
{
    return oracleEngine.getOracleQuery(queryId, output, (uint16_t)size) ? 1u : 0u;
}

static unsigned int getOracleReply(
    const void* /*context*/,
    long long queryId, void* output, unsigned int size)
{
    return oracleEngine.getOracleReply(queryId, output, (uint16_t)size) ? 1u : 0u;
}

} // namespace Wasm::Runtime
