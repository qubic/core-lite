#define NO_UEFI

#include "contract_testing.h"

static const id QSB_CONTRACT_ID(QSB_CONTRACT_INDEX, 0, 0, 0);
static const id USER1(123, 456, 789, 876);
static const id USER2(42, 424, 4242, 42424);
static const id ADMIN(100, 200, 300, 400);
static const id ORACLE1(500, 600, 700, 800);
static const id ORACLE2(900, 1000, 1100, 1200);
static const id ORACLE3(1300, 1400, 1500, 1600);
static const id PAUSER1(1700, 1800, 1900, 2000);
static const id PROTOCOL_FEE_RECIPIENT(2100, 2200, 2300, 2400);
static const id ORACLE_FEE_RECIPIENT(2500, 2600, 2700, 2800);

class StateCheckerQSB : public QSB, public QSB::StateData
{
public:
    const QPI::ContractState<StateData, QSB_CONTRACT_INDEX>& asState() const {
        return *reinterpret_cast<const QPI::ContractState<StateData, QSB_CONTRACT_INDEX>*>(static_cast<const StateData*>(this));
    }
    QPI::ContractState<StateData, QSB_CONTRACT_INDEX>& asMutState() {
        return *reinterpret_cast<QPI::ContractState<StateData, QSB_CONTRACT_INDEX>*>(static_cast<StateData*>(this));
    }

    void checkAdmin(const id& expectedAdmin) const
    {
        EXPECT_EQ(this->admin, expectedAdmin);
    }

    void checkPaused(bool expectedPaused) const
    {
        EXPECT_EQ((bool)this->paused, expectedPaused);
    }

    void checkOracleThreshold(uint8 expectedThreshold) const
    {
        EXPECT_EQ(this->oracleThreshold, expectedThreshold);
    }

    void checkOracleCount(uint32 expectedCount) const
    {
        EXPECT_EQ(this->oracleCount, expectedCount);
    }

    void checkBpsFee(uint32 expectedFee) const
    {
        EXPECT_EQ(this->bpsFee, expectedFee);
    }

    void checkProtocolFee(uint32 expectedFee) const
    {
        EXPECT_EQ(this->protocolFee, expectedFee);
    }

    void checkProtocolFeeRecipient(const id& expectedRecipient) const
    {
        EXPECT_EQ(this->protocolFeeRecipient, expectedRecipient);
    }

    void checkOracleFeeRecipient(const id& expectedRecipient) const
    {
        EXPECT_EQ(this->oracleFeeRecipient, expectedRecipient);
    }

    // Helper to mark an order hash as filled via the internal ring buffer logic.
    void forceMarkOrderFilled(const QSB::OrderHash& hash)
    {
        FilledOrderEntry entry;
        bool same = false;
        markOrderFilled(asMutState(), hash, 0, 0, same, entry);
    }
};

class ContractTestingQSB : protected ContractTesting
{
public:
    ContractTestingQSB()
    {
        initEmptySpectrum();
        initEmptyUniverse();
        INIT_CONTRACT(QSB);
        callSystemProcedure(QSB_CONTRACT_INDEX, INITIALIZE);

        checkContractExecCleanup();
    }

    ~ContractTestingQSB()
    {
        checkContractExecCleanup();
    }

    StateCheckerQSB* getState()
    {
        return (StateCheckerQSB*)contractStates[QSB_CONTRACT_INDEX];
    }

    const StateCheckerQSB* getState() const
    {
        return (const StateCheckerQSB*)contractStates[QSB_CONTRACT_INDEX];
    }

    // Helper to create a valid order for unlock testing
    QSB::Order createTestOrder(
        const id& fromAddress,
        const id& toAddress,
        uint64 amount,
        uint64 relayerFee,
        uint32 nonce) const
    {
        QSB::Order order;
        order.fromAddress = fromAddress;
        order.toAddress = toAddress;
        order.tokenIn = 0;
        order.tokenOut = 0;
        order.amount = amount;
        order.relayerFee = relayerFee;
        order.destinationChainId = 1; // Solana
        order.networkIn = 0; // Qubic
        order.networkOut = 1; // Solana
        order.nonce = nonce;
        return order;
    }

    // Helper to create signature data (mock - in real tests would need actual signatures)
    QSB::SignatureData createMockSignature(const id& signer) const
    {
        QSB::SignatureData sig;
        sig.signer = signer;
        // In real implementation, this would be a valid signature
        // For testing, we'll use zeros (signature validation will fail, but structure is correct)
        setMemory(sig.signature, 0);
        return sig;
    }

    // Helper to create a zero-initialized address array
    static Array<uint8, 64> createZeroAddress()
    {
        Array<uint8, 64> addr;
        setMemory(addr, 0);
        return addr;
    }

    // ============================================================================
    // User Procedure Helpers
    // ============================================================================

    QSB::Lock_output lock(const id& user, uint64 amount, uint64 relayerFee, uint32 networkOut, uint32 nonce, const Array<uint8, 64>& toAddress, uint64 energyAmount)
    {
        QSB::Lock_input input;
        QSB::Lock_output output;
        
        input.amount = amount;
        input.relayerFee = relayerFee;
        input.networkOut = networkOut;
        input.nonce = nonce;
        copyToBuffer(input.toAddress, toAddress, true);
        
        invokeUserProcedure(QSB_CONTRACT_INDEX, 1, input, output, user, energyAmount);
        return output;
    }

    QSB::OverrideLock_output overrideLock(const id& user, uint32 nonce, uint64 relayerFee, const Array<uint8, 64>& toAddress)
    {
        QSB::OverrideLock_input input;
        QSB::OverrideLock_output output;
        
        input.nonce = nonce;
        input.relayerFee = relayerFee;
        copyToBuffer(input.toAddress, toAddress, true);
        
        invokeUserProcedure(QSB_CONTRACT_INDEX, 2, input, output, user, 0);
        return output;
    }

    QSB::Unlock_output unlock(const id& user, const QSB::Order& order, uint32 numSignatures, const Array<QSB::SignatureData, QSB_MAX_ORACLES>& signatures)
    {
        QSB::Unlock_input input;
        QSB::Unlock_output output;
        
        input.order = order;
        input.numSignatures = numSignatures;
        copyMemory(input.signatures, signatures);
        
        invokeUserProcedure(QSB_CONTRACT_INDEX, 3, input, output, user, 0);
        return output;
    }

    // ============================================================================
    // Admin Procedure Helpers
    // ============================================================================

    QSB::TransferAdmin_output transferAdmin(const id& user, const id& newAdmin)
    {
        QSB::TransferAdmin_input input;
        QSB::TransferAdmin_output output;
        
        input.newAdmin = newAdmin;
        
        invokeUserProcedure(QSB_CONTRACT_INDEX, 10, input, output, user, 0);
        return output;
    }

    QSB::EditOracleThreshold_output editOracleThreshold(const id& user, uint8 newThreshold)
    {
        QSB::EditOracleThreshold_input input;
        QSB::EditOracleThreshold_output output;
        
        input.newThreshold = newThreshold;
        
        invokeUserProcedure(QSB_CONTRACT_INDEX, 11, input, output, user, 0);
        return output;
    }

    QSB::AddRole_output addRole(const id& user, uint8 role, const id& account)
    {
        QSB::AddRole_input input;
        QSB::AddRole_output output;
        
        input.role = role;
        input.account = account;
        
        invokeUserProcedure(QSB_CONTRACT_INDEX, 12, input, output, user, 0);
        return output;
    }

    QSB::RemoveRole_output removeRole(const id& user, uint8 role, const id& account)
    {
        QSB::RemoveRole_input input;
        QSB::RemoveRole_output output;
        
        input.role = role;
        input.account = account;
        
        invokeUserProcedure(QSB_CONTRACT_INDEX, 13, input, output, user, 0);
        return output;
    }

    QSB::Pause_output pause(const id& user)
    {
        QSB::Pause_input input;
        QSB::Pause_output output;
        
        invokeUserProcedure(QSB_CONTRACT_INDEX, 14, input, output, user, 0);
        return output;
    }

    QSB::Unpause_output unpause(const id& user)
    {
        QSB::Unpause_input input;
        QSB::Unpause_output output;
        
        invokeUserProcedure(QSB_CONTRACT_INDEX, 15, input, output, user, 0);
        return output;
    }

    QSB::EditFeeParameters_output editFeeParameters(
        const id& user,
        uint32 bpsFee,
        uint32 protocolFee,
        const id& protocolFeeRecipient,
        const id& oracleFeeRecipient)
    {
        QSB::EditFeeParameters_input input;
        QSB::EditFeeParameters_output output;
        
        input.bpsFee = bpsFee;
        input.protocolFee = protocolFee;
        input.protocolFeeRecipient = protocolFeeRecipient;
        input.oracleFeeRecipient = oracleFeeRecipient;
        
        invokeUserProcedure(QSB_CONTRACT_INDEX, 16, input, output, user, 0);
        return output;
    }

    // ============================================================================
    // View / helper function wrappers (GetConfig, IsOracle, IsPauser, GetLockedOrder, IsOrderFilled)
    // ============================================================================

    void runEndEpoch()
    {
        callSystemProcedure(QSB_CONTRACT_INDEX, END_EPOCH);
    }

    QSB::GetConfig_output getConfig() const
    {
        QSB::GetConfig_input input;
        QSB::GetConfig_output output;
        callFunction(QSB_CONTRACT_INDEX, 1, input, output);
        return output;
    }

    QSB::IsOracle_output isOracle(const id& account) const
    {
        QSB::IsOracle_input input;
        QSB::IsOracle_output output;
        input.account = account;
        callFunction(QSB_CONTRACT_INDEX, 2, input, output);
        return output;
    }

    QSB::IsPauser_output isPauser(const id& account) const
    {
        QSB::IsPauser_input input;
        QSB::IsPauser_output output;
        input.account = account;
        callFunction(QSB_CONTRACT_INDEX, 3, input, output);
        return output;
    }

    QSB::GetLockedOrder_output getLockedOrder(uint32 nonce) const
    {
        QSB::GetLockedOrder_input input;
        QSB::GetLockedOrder_output output;
        input.nonce = nonce;
        callFunction(QSB_CONTRACT_INDEX, 4, input, output);
        return output;
    }

    QSB::IsOrderFilled_output isOrderFilled(const QSB::OrderHash& hash) const
    {
        QSB::IsOrderFilled_input input;
        QSB::IsOrderFilled_output output;
        for (uint32 i = 0; i < input.hash.capacity(); ++i)
            input.hash.set(i, hash.get(i));
        callFunction(QSB_CONTRACT_INDEX, 5, input, output);
        return output;
    }

    QSB::ComputeOrderHash_output computeOrderHash(const QSB::Order& order) const
    {
        QSB::ComputeOrderHash_input input;
        QSB::ComputeOrderHash_output output;
        input.order = order;
        callFunction(QSB_CONTRACT_INDEX, 6, input, output);
        return output;
    }

    QSB::GetOracles_output getOracles() const
    {
        QSB::GetOracles_input input;
        QSB::GetOracles_output output;
        callFunction(QSB_CONTRACT_INDEX, 7, input, output);
        return output;
    }

    QSB::GetPausers_output getPausers() const
    {
        QSB::GetPausers_input input;
        QSB::GetPausers_output output;
        callFunction(QSB_CONTRACT_INDEX, 8, input, output);
        return output;
    }

    QSB::GetLockedOrders_output getLockedOrders(uint32 offset, uint32 limit) const
    {
        QSB::GetLockedOrders_input input;
        QSB::GetLockedOrders_output output;
        input.offset = offset;
        input.limit = limit;
        callFunction(QSB_CONTRACT_INDEX, 9, input, output);
        return output;
    }

    QSB::GetFilledOrders_output getFilledOrders(uint32 offset, uint32 limit) const
    {
        QSB::GetFilledOrders_input input;
        QSB::GetFilledOrders_output output;
        input.offset = offset;
        input.limit = limit;
        callFunction(QSB_CONTRACT_INDEX, 10, input, output);
        return output;
    }
};

// ============================================================================
// View helper function tests (GetConfig, IsOracle, IsPauser, GetLockedOrder, IsOrderFilled)
// ============================================================================

TEST(ContractTestingQSB, TestGetConfig_ReturnsInitialState)
{
    ContractTestingQSB test;

    QSB::GetConfig_output config = test.getConfig();

    EXPECT_EQ(config.admin, ADMIN);
    EXPECT_EQ(config.protocolFeeRecipient, NULL_ID);
    EXPECT_EQ(config.oracleFeeRecipient, NULL_ID);
    EXPECT_EQ(config.bpsFee, 0u);
    EXPECT_EQ(config.protocolFee, 0u);
    EXPECT_EQ(config.oracleCount, 0u);
    EXPECT_EQ(config.oracleThreshold, 67);
    EXPECT_EQ((bool)config.paused, false);
}

TEST(ContractTestingQSB, TestGetConfig_ReflectsAdminAndFeeChanges)
{
    ContractTestingQSB test;

    increaseEnergy(ADMIN, 1);
    test.editFeeParameters(ADMIN, 50, 20, PROTOCOL_FEE_RECIPIENT, ORACLE_FEE_RECIPIENT);

    QSB::GetConfig_output config = test.getConfig();

    EXPECT_EQ(config.admin, ADMIN);
    EXPECT_EQ(config.bpsFee, 50u);
    EXPECT_EQ(config.protocolFee, 20u);
    EXPECT_EQ(config.protocolFeeRecipient, PROTOCOL_FEE_RECIPIENT);
    EXPECT_EQ(config.oracleFeeRecipient, ORACLE_FEE_RECIPIENT);
}

TEST(ContractTestingQSB, TestIsOracle_ReturnsFalseWhenNotOracle)
{
    ContractTestingQSB test;

    QSB::IsOracle_output out = test.isOracle(ORACLE1);
    EXPECT_FALSE((bool)out.isOracle);

    out = test.isOracle(USER1);
    EXPECT_FALSE((bool)out.isOracle);
}

TEST(ContractTestingQSB, TestIsOracle_ReturnsTrueAfterAddRole)
{
    ContractTestingQSB test;

    increaseEnergy(ADMIN, 1);
    increaseEnergy(ORACLE1, 1);
    test.addRole(ADMIN, (uint8)QSB::Role::Oracle, ORACLE1);

    QSB::IsOracle_output out = test.isOracle(ORACLE1);
    EXPECT_TRUE((bool)out.isOracle);

    out = test.isOracle(ORACLE2);
    EXPECT_FALSE((bool)out.isOracle);
}

TEST(ContractTestingQSB, TestIsPauser_ReturnsFalseWhenNotPauser)
{
    ContractTestingQSB test;

    increaseEnergy(ADMIN, 1);
    increaseEnergy(PAUSER1, 1);
    test.addRole(ADMIN, (uint8)QSB::Role::Pauser, PAUSER1);

    QSB::IsPauser_output out = test.isPauser(PAUSER1);
    EXPECT_TRUE((bool)out.isPauser);

    out = test.isPauser(ORACLE1);
    EXPECT_FALSE((bool)out.isPauser);
}

TEST(ContractTestingQSB, TestIsPauser_ReturnsTrueAfterAddRole)
{
    ContractTestingQSB test;

    increaseEnergy(ADMIN, 1);
    increaseEnergy(PAUSER1, 1);
    test.addRole(ADMIN, (uint8)QSB::Role::Pauser, PAUSER1);

    QSB::IsPauser_output out = test.isPauser(PAUSER1);
    EXPECT_TRUE((bool)out.isPauser);

    out = test.isPauser(USER1);
    EXPECT_FALSE((bool)out.isPauser);
}

TEST(ContractTestingQSB, TestGetLockedOrder_ReturnsNotExistsForUnknownNonce)
{
    ContractTestingQSB test;

    QSB::GetLockedOrder_output out = test.getLockedOrder(999);
    EXPECT_FALSE((bool)out.exists);
}

TEST(ContractTestingQSB, TestGetLockedOrder_ReturnsOrderAfterLock)
{
    ContractTestingQSB test;

    const uint64 amount = 1000000;
    const uint64 relayerFee = 10000;
    const uint32 nonce = 42;

    increaseEnergy(USER1, amount);
    test.lock(USER1, amount, relayerFee, 1, nonce, ContractTestingQSB::createZeroAddress(), amount);

    QSB::GetLockedOrder_output out = test.getLockedOrder(nonce);
    EXPECT_TRUE((bool)out.exists);
    EXPECT_TRUE(out.order.active);
    EXPECT_EQ(out.order.sender, USER1);
    EXPECT_EQ(out.order.amount, amount);
    EXPECT_EQ(out.order.relayerFee, relayerFee);
    EXPECT_EQ(out.order.nonce, nonce);
}

TEST(ContractTestingQSB, TestIsOrderFilled_ReturnsFalseForUnknownHash)
{
    ContractTestingQSB test;

    QSB::OrderHash unknownHash;
    for (uint32 i = 0; i < unknownHash.capacity(); ++i)
        unknownHash.set(i, (uint8)(i & 0xff));

    QSB::IsOrderFilled_output out = test.isOrderFilled(unknownHash);
    EXPECT_FALSE((bool)out.filled);

    // After marking the hash as filled via the internal helper, it should report true.
    test.getState()->forceMarkOrderFilled(unknownHash);
    QSB::IsOrderFilled_output out2 = test.isOrderFilled(unknownHash);
    EXPECT_TRUE((bool)out2.filled);
}

// ============================================================================
// New query function tests (ComputeOrderHash, GetOracles, GetPausers, GetLockedOrders, GetFilledOrders)
// ============================================================================

TEST(ContractTestingQSB, TestComputeOrderHash_ReturnsConsistentHash)
{
    ContractTestingQSB test;

    QSB::Order order = test.createTestOrder(USER1, USER2, 1000000, 10000, 99);
    QSB::ComputeOrderHash_output out = test.computeOrderHash(order);

    // Hash should be non-zero
    bool hashNonZero = false;
    for (uint32 i = 0; i < out.hash.capacity(); ++i)
    {
        if (out.hash.get(i) != 0)
        {
            hashNonZero = true;
            break;
        }
    }
    EXPECT_TRUE(hashNonZero);

    // Same order should produce same hash
    QSB::ComputeOrderHash_output out2 = test.computeOrderHash(order);
    for (uint32 i = 0; i < out.hash.capacity(); ++i)
        EXPECT_EQ(out.hash.get(i), out2.hash.get(i));
}

TEST(ContractTestingQSB, TestComputeOrderHash_MatchesLockOutput)
{
    ContractTestingQSB test;

    const uint64 amount = 1000000;
    const uint64 relayerFee = 10000;
    const uint32 nonce = 50;

    increaseEnergy(USER1, amount);
    QSB::Lock_output lockOut = test.lock(USER1, amount, relayerFee, 1, nonce, ContractTestingQSB::createZeroAddress(), amount);
    EXPECT_TRUE(lockOut.success);

    // Create order matching what Lock hashes (fromAddress=invocator, toAddress=NULL_ID, amount, relayerFee, networkOut, nonce)
    QSB::Order order = test.createTestOrder(USER1, NULL_ID, amount, relayerFee, nonce);
    order.networkIn = 0;
    order.networkOut = 1;
    order.destinationChainId = 1;
    order.tokenIn = 0;
    order.tokenOut = 0;

    QSB::ComputeOrderHash_output computed = test.computeOrderHash(order);
    for (uint32 i = 0; i < lockOut.orderHash.capacity(); ++i)
        EXPECT_EQ(lockOut.orderHash.get(i), computed.hash.get(i));
}

TEST(ContractTestingQSB, TestGetOracles_ReturnsEmptyWhenNoOracles)
{
    ContractTestingQSB test;

    QSB::GetOracles_output out = test.getOracles();
    EXPECT_EQ(out.count, 0u);
}

TEST(ContractTestingQSB, TestGetOracles_ReturnsAllOraclesAfterAddRole)
{
    ContractTestingQSB test;

    increaseEnergy(ADMIN, 1);
    increaseEnergy(ORACLE1, 1);
    increaseEnergy(ORACLE2, 1);
    test.addRole(ADMIN, (uint8)QSB::Role::Oracle, ORACLE1);
    test.addRole(ADMIN, (uint8)QSB::Role::Oracle, ORACLE2);

    QSB::GetOracles_output out = test.getOracles();
    EXPECT_EQ(out.count, 2u);
    EXPECT_EQ(out.accounts.get(0), ORACLE1);
    EXPECT_EQ(out.accounts.get(1), ORACLE2);
}

TEST(ContractTestingQSB, TestGetPausers_ReturnsEmptyWhenNoPausers)
{
    ContractTestingQSB test;

    QSB::GetPausers_output out = test.getPausers();
    EXPECT_EQ(out.count, 0u);
}

TEST(ContractTestingQSB, TestGetPausers_ReturnsAllPausersAfterAddRole)
{
    ContractTestingQSB test;

    increaseEnergy(ADMIN, 1);
    increaseEnergy(PAUSER1, 1);
    test.addRole(ADMIN, (uint8)QSB::Role::Pauser, PAUSER1);

    QSB::GetPausers_output out = test.getPausers();
    EXPECT_EQ(out.count, 1u);
    EXPECT_EQ(out.accounts.get(0), PAUSER1);
}

TEST(ContractTestingQSB, TestGetLockedOrders_ReturnsEmptyWhenNoLocks)
{
    ContractTestingQSB test;

    QSB::GetLockedOrders_output out = test.getLockedOrders(0, 64);
    EXPECT_EQ(out.totalActive, 0u);
    EXPECT_EQ(out.returned, 0u);
}

TEST(ContractTestingQSB, TestGetLockedOrders_ReturnsLockedOrdersAfterLock)
{
    ContractTestingQSB test;

    const uint64 amount = 1000000;
    const uint64 relayerFee = 10000;
    const uint32 nonce = 77;

    increaseEnergy(USER1, amount);
    test.lock(USER1, amount, relayerFee, 1, nonce, ContractTestingQSB::createZeroAddress(), amount);

    QSB::GetLockedOrders_output out = test.getLockedOrders(0, 64);
    EXPECT_EQ(out.totalActive, 1u);
    EXPECT_EQ(out.returned, 1u);
    EXPECT_TRUE(out.entries.get(0).active);
    EXPECT_EQ(out.entries.get(0).sender, USER1);
    EXPECT_EQ(out.entries.get(0).amount, amount);
    EXPECT_EQ(out.entries.get(0).nonce, nonce);
}

TEST(ContractTestingQSB, TestGetLockedOrders_Pagination)
{
    ContractTestingQSB test;

    const uint64 amount = 1;
    increaseEnergy(USER1, amount * 5);

    for (uint32 i = 0; i < 5; ++i)
    {
        test.lock(USER1, amount, 0, 1, i, ContractTestingQSB::createZeroAddress(), amount);
    }

    QSB::GetLockedOrders_output out = test.getLockedOrders(0, 2);
    EXPECT_EQ(out.totalActive, 5u);
    EXPECT_EQ(out.returned, 2u);

    out = test.getLockedOrders(2, 2);
    EXPECT_EQ(out.totalActive, 5u);
    EXPECT_EQ(out.returned, 2u);

    out = test.getLockedOrders(4, 2);
    EXPECT_EQ(out.totalActive, 5u);
    EXPECT_EQ(out.returned, 1u);
}

TEST(ContractTestingQSB, TestGetFilledOrders_ReturnsEmptyWhenNoFills)
{
    ContractTestingQSB test;

    QSB::GetFilledOrders_output out = test.getFilledOrders(0, 64);
    EXPECT_EQ(out.totalActive, 0u);
    EXPECT_EQ(out.returned, 0u);
}

TEST(ContractTestingQSB, TestFilledOrders_RingBufferOverwritesOldEntries)
{
    ContractTestingQSB test;

    // Artificially mark more orders as filled than the ring capacity
    // to ensure oldest entries are overwritten while newer ones remain.
    for (uint32 i = 0; i < QSB_MAX_FILLED_ORDERS + 1; ++i)
    {
        QSB::OrderHash hash;
        setMemory(hash, 0);
        // Encode i into the first two bytes to avoid collisions when
        // QSB_MAX_FILLED_ORDERS exceeds 255.
        hash.set(0, (uint8)(i & 0xff));
        hash.set(1, (uint8)((i >> 8) & 0xff));
        test.getState()->forceMarkOrderFilled(hash);
    }

    // Hash for 0 should have been overwritten (only last QSB_MAX_FILLED_ORDERS kept)
    QSB::OrderHash hash0;
    setMemory(hash0, 0);
    hash0.set(1, 0);
    QSB::IsOrderFilled_output out0 = test.isOrderFilled(hash0);
    EXPECT_FALSE((bool)out0.filled);

    // Hash for the last inserted nonce (QSB_MAX_FILLED_ORDERS) should be present
    QSB::OrderHash hashLast;
    setMemory(hashLast, 0);
    hashLast.set(0, (uint8)(QSB_MAX_FILLED_ORDERS & 0xff));
    hashLast.set(1, (uint8)((QSB_MAX_FILLED_ORDERS >> 8) & 0xff));
    QSB::IsOrderFilled_output outLast = test.isOrderFilled(hashLast);
    EXPECT_TRUE((bool)outLast.filled);
}

// ============================================================================
// Initialization Tests
// ============================================================================

TEST(ContractTestingQSB, TestInitialization)
{
    ContractTestingQSB test;
    
    // Check initial state
    test.getState()->checkAdmin(ADMIN);
    test.getState()->checkPaused(false);
    test.getState()->checkOracleThreshold(67); // Default 67%
    test.getState()->checkOracleCount(0);
    test.getState()->checkBpsFee(0);
    test.getState()->checkProtocolFee(0);
    
    test.getState()->checkProtocolFeeRecipient(NULL_ID);
    test.getState()->checkOracleFeeRecipient(NULL_ID);
}

// ============================================================================
// Lock Function Tests
// ============================================================================

TEST(ContractTestingQSB, TestLock_Success)
{
    ContractTestingQSB test;
    
    const uint64 amount = 1000000;
    const uint64 relayerFee = 10000;
    const uint32 networkOut = 1; // Solana
    const uint32 nonce = 1;
    
    // User should have enough balance
    increaseEnergy(USER1, amount);
    
    QSB::Lock_output output = test.lock(USER1, amount, relayerFee, networkOut, nonce, ContractTestingQSB::createZeroAddress(), amount);
    EXPECT_TRUE(output.success);
    
    // Check that orderHash is non-zero
    bool hashNonZero = false;
    for (uint32 i = 0; i < output.orderHash.capacity(); ++i)
    {
        if (output.orderHash.get(i) != 0)
        {
            hashNonZero = true;
            break;
        }
    }
    EXPECT_TRUE(hashNonZero);
}

TEST(ContractTestingQSB, TestLock_FailsWhenPaused)
{
    ContractTestingQSB test;
    
    increaseEnergy(ADMIN, 1);
    increaseEnergy(USER1, 1000000);
    
    // Pause
    test.pause(ADMIN);
    
    // Now try to lock - should fail
    const uint64 amount = 1000000;
    long long balanceBefore = getBalance(USER1);
    
    QSB::Lock_output output = test.lock(USER1, amount, 10000, 1, 2, ContractTestingQSB::createZeroAddress(), amount);
    EXPECT_FALSE(output.success);

    long long balanceAfter = getBalance(USER1);
    EXPECT_EQ(balanceAfter, balanceBefore);
}

TEST(ContractTestingQSB, TestLock_FailsWhenRelayerFeeTooHigh)
{
    ContractTestingQSB test;
    
    const uint64 amount = 1000000;
    increaseEnergy(USER1, amount);
    
    QSB::Lock_output output = test.lock(USER1, amount, 1000000, 1, 3, ContractTestingQSB::createZeroAddress(), amount);
    EXPECT_FALSE(output.success);
}

TEST(ContractTestingQSB, TestLock_FailsWhenAmountIsZero)
{
    ContractTestingQSB test;

    const uint64 amount = 0;
    const uint64 relayerFee = 0;
    const uint32 nonce = 40;

    // No energy needed since amount is zero, but helper still expects an energyAmount argument
    QSB::Lock_output output = test.lock(USER1, amount, relayerFee, 1, nonce, ContractTestingQSB::createZeroAddress(), 0);
    EXPECT_FALSE(output.success);
}

TEST(ContractTestingQSB, TestLock_SucceedsWhenRelayerFeeIsAmountMinusOne)
{
    ContractTestingQSB test;

    const uint64 amount = 1000000;
    const uint64 relayerFee = amount - 1;
    const uint32 nonce = 41;

    increaseEnergy(USER1, amount);

    QSB::Lock_output output = test.lock(USER1, amount, relayerFee, 1, nonce, ContractTestingQSB::createZeroAddress(), amount);
    EXPECT_TRUE(output.success);
}

TEST(ContractTestingQSB, TestLock_FailsWhenInvocationRewardTooLowAndIsRefunded)
{
    ContractTestingQSB test;

    const uint64 amount = 1000000;
    const uint64 relayerFee = 10000;
    const uint32 nonce = 42;

    // User only sends half the required amount as invocationReward
    increaseEnergy(USER1, amount / 2);
    long long balanceBefore = getBalance(USER1);

    QSB::Lock_output output = test.lock(USER1, amount, relayerFee, 1, nonce, ContractTestingQSB::createZeroAddress(), amount / 2);
    EXPECT_FALSE(output.success);

    long long balanceAfter = getBalance(USER1);
    EXPECT_EQ(balanceAfter, balanceBefore);
}

TEST(ContractTestingQSB, TestLock_RingBufferOverwritesOldLockedOrders)
{
    ContractTestingQSB test;

    const uint64 amount = 1;
    const uint64 relayerFee = 0;

    // Fill all available locked order slots with unique nonces
    for (uint32 i = 0; i < QSB_MAX_LOCKED_ORDERS; ++i)
    {
        increaseEnergy(USER1, amount);
        QSB::Lock_output out = test.lock(USER1, amount, relayerFee, 1, i, ContractTestingQSB::createZeroAddress(), amount);
        EXPECT_TRUE(out.success);
    }

    // Next lock should still succeed, but the ring buffer will overwrite
    // one of the older entries. The very first nonce (0) should no longer
    // be queryable via GetLockedOrder, while the latest nonce should exist.
    const uint32 oldestNonce = 0;
    const uint32 newestNonce = QSB_MAX_LOCKED_ORDERS;

    increaseEnergy(USER1, amount);
    QSB::Lock_output overflowOut = test.lock(USER1, amount, relayerFee, 1, newestNonce, ContractTestingQSB::createZeroAddress(), amount);
    EXPECT_TRUE(overflowOut.success);

    QSB::GetLockedOrder_output oldest = test.getLockedOrder(oldestNonce);
    EXPECT_FALSE((bool)oldest.exists);

    QSB::GetLockedOrder_output newest = test.getLockedOrder(newestNonce);
    EXPECT_TRUE((bool)newest.exists);
    EXPECT_EQ(newest.order.nonce, newestNonce);
}

TEST(ContractTestingQSB, TestLock_FailsWhenNonceAlreadyUsedAndRefunds)
{
    ContractTestingQSB test;

    const uint64 amount = 1000000;
    const uint64 relayerFee = 10000;
    const uint32 nonce = 43;

    increaseEnergy(USER1, amount);
    QSB::Lock_output first = test.lock(USER1, amount, relayerFee, 1, nonce, ContractTestingQSB::createZeroAddress(), amount);
    EXPECT_TRUE(first.success);

    // Second attempt with same nonce should fail and refund invocationReward
    increaseEnergy(USER1, amount);
    long long balanceBefore = getBalance(USER1);

    QSB::Lock_output second = test.lock(USER1, amount, relayerFee, 1, nonce, ContractTestingQSB::createZeroAddress(), amount);
    EXPECT_FALSE(second.success);

    long long balanceAfter = getBalance(USER1);
    EXPECT_EQ(balanceAfter, balanceBefore);
}

TEST(ContractTestingQSB, TestLock_FailsWhenNonceAlreadyUsed)
{
    ContractTestingQSB test;
    
    const uint64 amount = 1000000;
    const uint64 relayerFee = 10000;
    const uint32 nonce = 4;
    
    increaseEnergy(USER1, amount);
    
    // First lock should succeed
    QSB::Lock_output output = test.lock(USER1, amount, relayerFee, 1, nonce, ContractTestingQSB::createZeroAddress(), amount);
    EXPECT_TRUE(output.success);
    
    // Second lock with same nonce should fail
    increaseEnergy(USER1, amount);
    QSB::Lock_output output2 = test.lock(USER1, amount, relayerFee, 1, nonce, ContractTestingQSB::createZeroAddress(), amount);
    EXPECT_FALSE(output2.success);
}

// ============================================================================
// OverrideLock Function Tests
// ============================================================================

TEST(ContractTestingQSB, TestOverrideLock_Success)
{
    ContractTestingQSB test;
    
    const uint64 amount = 1000000;
    const uint64 relayerFee = 10000;
    const uint32 nonce = 5;
    
    // First, create a lock
    increaseEnergy(USER1, amount);
    test.lock(USER1, amount, relayerFee, 1, nonce, ContractTestingQSB::createZeroAddress(), amount);
    
    // Now override it
    Array<uint8, 64> newAddress = ContractTestingQSB::createZeroAddress();
    newAddress.set(0, 0xFF); // Change address
    
    QSB::OverrideLock_output overrideOutput = test.overrideLock(USER1, nonce, 5000, newAddress);
    EXPECT_TRUE(overrideOutput.success);
}

TEST(ContractTestingQSB, TestOverrideLock_FailsWhenNotOriginalSender)
{
    ContractTestingQSB test;
    
    const uint64 amount = 1000000;
    const uint64 relayerFee = 10000;
    const uint32 nonce = 6;
    
    // USER1 creates a lock
    increaseEnergy(USER1, amount);
    test.lock(USER1, amount, relayerFee, 1, nonce, ContractTestingQSB::createZeroAddress(), amount);
    
    // USER2 tries to override - should fail
    QSB::OverrideLock_output overrideOutput = test.overrideLock(USER2, nonce, 5000, ContractTestingQSB::createZeroAddress());
    EXPECT_FALSE(overrideOutput.success);
}

// ============================================================================
// Admin Function Tests
// ============================================================================

TEST(ContractTestingQSB, TestTransferAdmin_Success)
{
    ContractTestingQSB test;
    
    increaseEnergy(ADMIN, 1);
    increaseEnergy(USER1, 1);
    QSB::TransferAdmin_output output = test.transferAdmin(ADMIN, USER1);
    EXPECT_TRUE(output.success);
    
    test.getState()->checkAdmin(USER1);
}

TEST(ContractTestingQSB, TestTransferAdmin_ToNullId)
{
    ContractTestingQSB test;

    increaseEnergy(ADMIN, 1);
    QSB::TransferAdmin_output output = test.transferAdmin(ADMIN, NULL_ID);
    EXPECT_TRUE(output.success);

    test.getState()->checkAdmin(NULL_ID);
}

TEST(ContractTestingQSB, TestTransferAdmin_FailsWhenNotAdmin)
{
    ContractTestingQSB test;
    
    // First bootstrap admin
    increaseEnergy(USER1, 1);
    increaseEnergy(USER2, 1);
    // Now USER1 tries to transfer admin - should fail
    QSB::TransferAdmin_output output = test.transferAdmin(USER1, USER2);
    EXPECT_FALSE(output.success);
    
    // Admin should still be ADMIN
    test.getState()->checkAdmin(ADMIN);
}

TEST(ContractTestingQSB, TestEditOracleThreshold_Success)
{
    ContractTestingQSB test;
    
    // Bootstrap admin
    increaseEnergy(ADMIN, 1);
    
    QSB::EditOracleThreshold_output output = test.editOracleThreshold(ADMIN, 75);
    EXPECT_TRUE(output.success);
    EXPECT_EQ(output.oldThreshold, 67); // Original default
    
    test.getState()->checkOracleThreshold(75);
}

TEST(ContractTestingQSB, TestAddRole_Oracle)
{
    ContractTestingQSB test;
    
    // Bootstrap admin
    increaseEnergy(ADMIN, 1);
    
    QSB::AddRole_output output = test.addRole(ADMIN, (uint8)QSB::Role::Oracle, ORACLE1);
    EXPECT_TRUE(output.success);
    
    test.getState()->checkOracleCount(1);
}

TEST(ContractTestingQSB, TestAddRole_Pauser)
{
    ContractTestingQSB test;
    
    // Bootstrap admin
    increaseEnergy(ADMIN, 1);
    increaseEnergy(PAUSER1, 1);
    
    QSB::AddRole_output output = test.addRole(ADMIN, (uint8)QSB::Role::Pauser, PAUSER1);
    EXPECT_TRUE(output.success);
}

TEST(ContractTestingQSB, TestRemoveRole_Oracle)
{
    ContractTestingQSB test;
    
    // Bootstrap admin and add oracle
    increaseEnergy(ADMIN, 1);
    test.addRole(ADMIN, (uint8)QSB::Role::Oracle, ORACLE1);
    
    // Now remove it
    QSB::RemoveRole_output output = test.removeRole(ADMIN, (uint8)QSB::Role::Oracle, ORACLE1);
    EXPECT_TRUE(output.success);
    
    test.getState()->checkOracleCount(0);
}

TEST(ContractTestingQSB, TestPause_ByAdmin)
{
    ContractTestingQSB test;
    
    // Bootstrap admin
    increaseEnergy(ADMIN, 1);
    
    QSB::Pause_output output = test.pause(ADMIN);
    EXPECT_TRUE(output.success);
    
    test.getState()->checkPaused(true);
}

TEST(ContractTestingQSB, TestPause_ByPauser)
{
    ContractTestingQSB test;
    
    // Bootstrap admin
    increaseEnergy(ADMIN, 1);
    increaseEnergy(PAUSER1, 1);
    
    // Add pauser
    test.addRole(ADMIN, (uint8)QSB::Role::Pauser, PAUSER1);
    
    // Pauser can pause
    QSB::Pause_output output = test.pause(PAUSER1);
    EXPECT_TRUE(output.success);
    
    test.getState()->checkPaused(true);
}

TEST(ContractTestingQSB, TestUnpause)
{
    ContractTestingQSB test;
    
    // Bootstrap admin and pause
    increaseEnergy(ADMIN, 1);
    test.pause(ADMIN);
    
    // Now unpause
    QSB::Unpause_output output = test.unpause(ADMIN);
    EXPECT_TRUE(output.success);
    
    test.getState()->checkPaused(false);
}

TEST(ContractTestingQSB, TestEditFeeParameters)
{
    ContractTestingQSB test;
    
    // Bootstrap admin
    increaseEnergy(ADMIN, 1);
    
    QSB::EditFeeParameters_output output = test.editFeeParameters(ADMIN, 100, 30, PROTOCOL_FEE_RECIPIENT, ORACLE_FEE_RECIPIENT);
    EXPECT_TRUE(output.success);
    
    test.getState()->checkBpsFee(100);
    test.getState()->checkProtocolFee(30);
    test.getState()->checkProtocolFeeRecipient(PROTOCOL_FEE_RECIPIENT);
    test.getState()->checkOracleFeeRecipient(ORACLE_FEE_RECIPIENT);
}

TEST(ContractTestingQSB, TestEditFeeParameters_RejectsTooHighBpsFee)
{
    ContractTestingQSB test;

    // Bootstrap admin
    increaseEnergy(ADMIN, 1);

    // Try to set bpsFee above the allowed maximum
    QSB::EditFeeParameters_output output = test.editFeeParameters(ADMIN, QSB_MAX_BPS_FEE + 1, 0, NULL_ID, NULL_ID);
    EXPECT_FALSE(output.success);

    // State should remain unchanged
    test.getState()->checkBpsFee(0);
}

TEST(ContractTestingQSB, TestEditFeeParameters_RejectsTooHighProtocolFee)
{
    ContractTestingQSB test;

    // Bootstrap admin and set an initial valid configuration
    increaseEnergy(ADMIN, 1);
    test.editFeeParameters(ADMIN, 100, 10, PROTOCOL_FEE_RECIPIENT, ORACLE_FEE_RECIPIENT);

    // Attempt to set protocolFee above the allowed maximum
    QSB::EditFeeParameters_output output = test.editFeeParameters(ADMIN, 0, QSB_MAX_PROTOCOL_FEE + 1, NULL_ID, NULL_ID);
    EXPECT_FALSE(output.success);

    // State should still reflect the previous valid configuration
    test.getState()->checkProtocolFee(10);
}

// ============================================================================
// Unlock Function Tests
// ============================================================================
// Note: Full unlock testing would require valid oracle signatures
// These tests verify the structure and basic validation logic

TEST(ContractTestingQSB, TestUnlock_FailsWhenNoOracles)
{
    ContractTestingQSB test;
    
    // Bootstrap admin
    increaseEnergy(ADMIN, 1);
    
    QSB::Order order = test.createTestOrder(USER1, USER2, 1000000, 10000, 100);
    Array<QSB::SignatureData, QSB_MAX_ORACLES> signatures;
    setMemory(signatures, 0);
    
    QSB::Unlock_output output = test.unlock(USER1, order, 0, signatures);
    EXPECT_FALSE(output.success); // Should fail - no oracles configured
}

TEST(ContractTestingQSB, TestUnlock_FailsWhenPaused)
{
    ContractTestingQSB test;
    
    // Bootstrap admin, add oracle, and pause
    increaseEnergy(ADMIN, 1);
    test.addRole(ADMIN, (uint8)QSB::Role::Oracle, ORACLE1);
    test.pause(ADMIN);
    
    QSB::Order order = test.createTestOrder(USER1, USER2, 1000000, 10000, 101);
    Array<QSB::SignatureData, QSB_MAX_ORACLES> signatures;
    setMemory(signatures, 0);
    signatures.set(0, test.createMockSignature(ORACLE1));
    
    QSB::Unlock_output output = test.unlock(USER1, order, 1, signatures);
    EXPECT_FALSE(output.success); // Should fail - contract is paused
}

TEST(ContractTestingQSB, TestUnlock_FailsWhenContractBalanceTooLow)
{
    ContractTestingQSB test;

    increaseEnergy(USER1, 1);
    increaseEnergy(USER2, 1);
    increaseEnergy(ORACLE1, 1);
    // No prior locks or deposits -> contract balance should be zero
    QSB::Order order = test.createTestOrder(USER1, USER2, 1000000, 10000, 102);
    Array<QSB::SignatureData, QSB_MAX_ORACLES> signatures;
    setMemory(signatures, 0);
    signatures.set(0, test.createMockSignature(ORACLE1));

    QSB::Unlock_output output = test.unlock(USER1, order, 1, signatures);
    EXPECT_FALSE(output.success);
}

TEST(ContractTestingQSB, TestUnlock_FailsWhenOrderAlreadyFilledBeforeOracleChecks)
{
    ContractTestingQSB test;

    // Provide some contract balance via a lock, but the specific lock
    // is intentionally unrelated to the unlock order in this model.
    const uint64 amountLocked = 1000000;
    increaseEnergy(USER1, amountLocked);
    test.lock(USER1, amountLocked, 0, 1, 500, ContractTestingQSB::createZeroAddress(), amountLocked);

    // Prepare an unlock order and compute its hash
    const uint64 amount = 100000;
    const uint64 relayerFee = 1000;
    QSB::Order order = test.createTestOrder(USER1, USER2, amount, relayerFee, 600);
    QSB::ComputeOrderHash_output hashOut = test.computeOrderHash(order);

    // Mark this order hash as already filled via the state helper
    test.getState()->forceMarkOrderFilled(hashOut.hash);

    // Attempt unlock with no oracles and no signatures — Unlock should
    // still fail with AlreadyFilled before it ever reaches oracle checks.
    Array<QSB::SignatureData, QSB_MAX_ORACLES> signatures;
    setMemory(signatures, 0);
    QSB::Unlock_output output = test.unlock(USER1, order, 0, signatures);
    EXPECT_FALSE(output.success);
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST(ContractTestingQSB, TestFullWorkflow_LockAndOverride)
{
    ContractTestingQSB test;
    
    const uint64 amount = 1000000;
    const uint64 initialRelayerFee = 10000;
    const uint64 newRelayerFee = 5000;
    const uint32 nonce = 200;
    
    // Step 1: Lock
    increaseEnergy(USER1, amount);
    QSB::Lock_output lockOutput = test.lock(USER1, amount, initialRelayerFee, 1, nonce, ContractTestingQSB::createZeroAddress(), amount);
    EXPECT_TRUE(lockOutput.success);
    
    // Step 2: Override
    QSB::OverrideLock_output overrideOutput = test.overrideLock(USER1, nonce, newRelayerFee, ContractTestingQSB::createZeroAddress());
    EXPECT_TRUE(overrideOutput.success);
    
    // OrderHash should be different after override
    bool hashesDifferent = false;
    for (uint32 i = 0; i < lockOutput.orderHash.capacity(); ++i)
    {
        if (lockOutput.orderHash.get(i) != overrideOutput.orderHash.get(i))
        {
            hashesDifferent = true;
            break;
        }
    }
    EXPECT_TRUE(hashesDifferent);
}

TEST(ContractTestingQSB, TestAdminWorkflow_SetupAndConfigure)
{
    ContractTestingQSB test;
    
    // Step 1: Bootstrap admin
    increaseEnergy(ADMIN, 1);
    
    // Step 2: Add oracles
    increaseEnergy(ORACLE1, 1);
    increaseEnergy(ORACLE2, 1);
    increaseEnergy(ORACLE3, 1);
    test.addRole(ADMIN, (uint8)QSB::Role::Oracle, ORACLE1);
    test.addRole(ADMIN, (uint8)QSB::Role::Oracle, ORACLE2);
    test.addRole(ADMIN, (uint8)QSB::Role::Oracle, ORACLE3);
    
    test.getState()->checkOracleCount(3);
    
    // Step 3: Set threshold
    test.editOracleThreshold(ADMIN, 67); // 2/3 + 1
    test.getState()->checkOracleThreshold(67);
    
    // Step 4: Configure fees
    test.editFeeParameters(ADMIN, 50, 20, PROTOCOL_FEE_RECIPIENT, ORACLE_FEE_RECIPIENT);
    
    test.getState()->checkBpsFee(50);
    test.getState()->checkProtocolFee(20);
}