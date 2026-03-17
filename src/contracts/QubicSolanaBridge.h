using namespace QPI;

// ---------------------------------------------------------------------
// Constants / configuration
// ---------------------------------------------------------------------

static constexpr uint32 QSB_MAX_ORACLES = 64;
static constexpr uint32 QSB_MAX_PAUSERS = 32;
static constexpr uint32 QSB_MAX_FILLED_ORDERS = 2048;
static constexpr uint32 QSB_MAX_LOCKED_ORDERS = 1024;
static constexpr uint32 QSB_MAX_BPS_FEE = 1000;      // max 10% fee (1000 / 10000)
static constexpr uint32 QSB_MAX_PROTOCOL_FEE = 100;  // max 100% of bps fee
static constexpr uint32 QSB_QUERY_MAX_PAGE_SIZE = 64; // max entries per paginated query

// Log types for QSB contract (no enums allowed in contracts)
static const uint32 QSBLogLock = 1;
static const uint32 QSBLogOverrideLock = 2;
static const uint32 QSBLogUnlock = 3;
static const uint32 QSBLogPaused = 4;
static const uint32 QSBLogUnpaused = 5;
static const uint32 QSBLogAdminTransferred = 6;
static const uint32 QSBLogThresholdUpdated = 7;
static const uint32 QSBLogRoleGranted = 8;
static const uint32 QSBLogRoleRevoked = 9;
static const uint32 QSBLogFeeParametersUpdated = 10;
static const uint32 QSBLogCancelLock = 11;

// Generic reason codes for logging
static const uint8 QSBReasonNone = 0;
static const uint8 QSBReasonPaused = 1;
static const uint8 QSBReasonInvalidAmount = 2;
static const uint8 QSBReasonInsufficientReward = 3;
static const uint8 QSBReasonNonceUsed = 4;
static const uint8 QSBReasonNoSpace = 5;
static const uint8 QSBReasonNotSender = 6;
static const uint8 QSBReasonBadRelayerFee = 7;
static const uint8 QSBReasonNoOracles = 8;
static const uint8 QSBReasonThresholdFailed = 9;
static const uint8 QSBReasonAlreadyFilled = 10;
static const uint8 QSBReasonInvalidSignature = 11;
static const uint8 QSBReasonDuplicateSigner = 12;
static const uint8 QSBReasonNotAdmin = 13;
static const uint8 QSBReasonNotAdminOrPauser = 14;
static const uint8 QSBReasonInvalidThreshold = 15;
static const uint8 QSBReasonRoleExists = 16;
static const uint8 QSBReasonRoleMissing = 17;
static const uint8 QSBReasonInvalidFeeParams = 18;
static const uint8 QSBReasonTransferFailed = 19;
// 20, 21 reserved for future use

struct QSB2
{
};

struct QSB : public ContractBase
{
public:
	// Role identifiers for addRole / removeRole
	enum class Role : uint8
	{
		Oracle = 1,
		Pauser = 2
	};

	// ---------------------------------------------------------------------
	// Core data structures
	// ---------------------------------------------------------------------

	// Order object used for unlock()
	struct Order
	{
		id fromAddress;              // sender on source chain (mirrored id)
		id toAddress;                // Qubic recipient when minting/unlocking
		uint64 tokenIn;              // identifier for incoming asset
		uint64 tokenOut;             // identifier for outgoing asset
		uint64 amount;               // total bridged amount (Qubic units)
		uint64 relayerFee;           // fee paid to relayer (subset of amount)
		uint32 destinationChainId;   // e.g. Solana chain-id as used off-chain
		uint32 networkIn;            // incoming network id
		uint32 networkOut;           // outgoing network id
		uint32 nonce;                // unique order nonce
	};

	// Compact order-hash representation (K12 digest)
	typedef Array<uint8, 32> OrderHash;

	// Signature wrapper compatible with QPI::signatureValidity
	struct SignatureData
	{
		id signer;     // oracle id (public key)
		Array<sint8, 64> signature;  // raw 64-byte signature
	};

	// Storage entry for filledOrders mapping
	struct FilledOrderEntry
	{
		OrderHash hash;
		bit used;
	};

	// Storage entry for role mappings (oracles / pausers)
	struct RoleEntry
	{
		id account;
		bit active;
	};

	// Storage entry for lock() orders (for overrideLock / off-chain reference)
	struct LockedOrderEntry
	{
		id sender;
		uint64 amount;
		uint64 relayerFee;
		uint32 networkOut;
		uint32 nonce;
		Array<uint8, 64> toAddress;
		OrderHash orderHash;
		uint32 lockEpoch;
		bit active;
	};

	// Logging messages
	struct QSBLogLockMessage
	{
		uint32 _contractIndex;
		uint32 _type;
		id from;
		Array<uint8, 64> to;
		uint64 amount;
		uint64 relayerFee;
		uint32 networkOut;
		uint32 nonce;
		OrderHash orderHash;
		uint8 success;
		uint8 reasonCode;
		sint8 _terminator;
	};

	struct QSBLogOverrideLockMessage
	{
		uint32 _contractIndex;
		uint32 _type;
		id from;
		Array<uint8, 64> to;
		uint64 amount;
		uint64 relayerFee;
		uint32 networkOut;
		uint32 nonce;
		OrderHash orderHash;
		uint8 success;
		uint8 reasonCode;
		sint8 _terminator;
	};

	struct QSBLogUnlockMessage
	{
		uint32 _contractIndex;
		uint32 _type;
		OrderHash orderHash;
		id toAddress;
		uint64 amount;
		uint64 relayerFee;
		id relayer;
		uint8 success;
		uint8 reasonCode;
		sint8 _terminator;
	};

	struct QSBLogAdminTransferredMessage
	{
		uint32 _contractIndex;
		uint32 _type;
		id previousAdmin;
		id newAdmin;
		uint8 success;
		uint8 reasonCode;
		sint8 _terminator;
	};

	struct QSBLogThresholdUpdatedMessage
	{
		uint32 _contractIndex;
		uint32 _type;
		uint8 oldThreshold;
		uint8 newThreshold;
		uint8 success;
		uint8 reasonCode;
		sint8 _terminator;
	};

	struct QSBLogRoleMessage
	{
		uint32 _contractIndex;
		uint32 _type;
		uint8 role;
		id account;
		id caller;
		uint8 success;
		uint8 reasonCode;
		sint8 _terminator;
	};

	struct QSBLogPausedMessage
	{
		uint32 _contractIndex;
		uint32 _type;
		id caller;
		uint8 success;
		uint8 reasonCode;
		sint8 _terminator;
	};

	struct QSBLogFeeParametersUpdatedMessage
	{
		uint32 _contractIndex;
		uint32 _type;
		uint32 bpsFee;
		uint32 protocolFee;
		id protocolFeeRecipient;
		id oracleFeeRecipient;
		uint8 success;
		uint8 reasonCode;
		sint8 _terminator;
	};

	// ---------------------------------------------------------------------
	// User-facing I/O structures
	// ---------------------------------------------------------------------

	// 1) lock()
	struct Lock_input
	{
		// Recipient on Solana (fixed-size buffer, zero-padded)
		uint64 amount;
		uint64 relayerFee;
		Array<uint8, 64> toAddress;
		uint32 networkOut;
		uint32 nonce;
	};

	struct Lock_output
	{
		OrderHash orderHash;
		bit success;
	};

	// 2) overrideLock()
	struct OverrideLock_input
	{
		Array<uint8, 64> toAddress;
		uint64 relayerFee;
		uint32 nonce;
	};

	struct OverrideLock_output
	{
		OrderHash orderHash;
		bit success;
	};

	// 3) unlock()
	struct Unlock_input
	{
		Order order;
		uint32 numSignatures;
		Array<SignatureData, QSB_MAX_ORACLES> signatures;
	};

	struct Unlock_output
	{
		OrderHash orderHash;
		bit success;
	};

	// 4) transferAdmin()
	struct TransferAdmin_input
	{
		id newAdmin;
	};

	struct TransferAdmin_output
	{
		bit success;
	};

	// 5) editOracleThreshold()
	struct EditOracleThreshold_input
	{
		uint8 newThreshold;
	};

	struct EditOracleThreshold_output
	{
		uint8 oldThreshold;
		bit success;
	};

	// 6) addRole()
	struct AddRole_input
	{
		id account;
		uint8 role;    // see Role enum
	};

	struct AddRole_output
	{
		bit success;
	};

	// 7) removeRole()
	struct RemoveRole_input
	{
		id account;
		uint8 role;
	};

	struct RemoveRole_output
	{
		bit success;
	};

	// 8) pause() / unpause()
	struct Pause_input
	{
	};

	struct Pause_output
	{
		bit success;
	};

	typedef Pause_input  Unpause_input;
	typedef Pause_output Unpause_output;

	// 9) editFeeParameters()
	struct EditFeeParameters_input
	{
		id protocolFeeRecipient; // updated when not zero-id
		id oracleFeeRecipient;   // updated when not zero-id
		uint32 bpsFee;           // basis points fee (0..10000)
		uint32 protocolFee;      // share of BPS fee for protocol (0..100)
	};

	struct EditFeeParameters_output
	{
		bit success;
	};

	// ---------------------------------------------------------------------
	// View / frontend helper functions
	// ---------------------------------------------------------------------

	struct GetConfig_input
	{
	};

	struct GetConfig_output
	{
		id admin;
		id protocolFeeRecipient;
		id oracleFeeRecipient;
		uint32 bpsFee;
		uint32 protocolFee;
		uint32 oracleCount;
		uint32 pauserCount;
		uint8 oracleThreshold;
		bit paused;
	};

	struct IsOracle_input
	{
		id account;
	};

	struct IsOracle_output
	{
		bit isOracle;
	};

	struct IsPauser_input
	{
		id account;
	};

	struct IsPauser_output
	{
		bit isPauser;
	};

	struct GetLockedOrder_input
	{
		uint32 nonce;
	};

	struct GetLockedOrder_output
	{
		bit exists;
		LockedOrderEntry order;
	};

	struct IsOrderFilled_input
	{
		OrderHash hash;
	};

	struct IsOrderFilled_output
	{
		bit filled;
	};

	// ComputeOrderHash: canonical hash for Unlock verification
	struct ComputeOrderHash_input
	{
		Order order;
	};

	struct ComputeOrderHash_output
	{
		OrderHash hash;
	};

	// GetOracles: bulk enumeration of all oracle accounts
	struct GetOracles_input
	{
	};

	struct GetOracles_output
	{
		uint32 count;
		Array<id, QSB_MAX_ORACLES> accounts;
	};

	// GetPausers: bulk enumeration of all pauser accounts
	struct GetPausers_input
	{
	};

	struct GetPausers_output
	{
		uint32 count;
		Array<id, QSB_MAX_PAUSERS> accounts;
	};

	// GetLockedOrders: paginated enumeration of active locked orders
	struct GetLockedOrders_input
	{
		uint32 offset; // skip this many active entries
		uint32 limit;  // return up to this many (capped at QSB_QUERY_MAX_PAGE_SIZE)
	};

	struct GetLockedOrders_output
	{
		uint32 totalActive;
		uint32 returned;
		Array<LockedOrderEntry, QSB_QUERY_MAX_PAGE_SIZE> entries;
	};

	// GetFilledOrders: paginated enumeration of filled order hashes
	struct GetFilledOrders_input
	{
		uint32 offset; // skip this many filled entries
		uint32 limit;  // return up to this many (capped at QSB_QUERY_MAX_PAGE_SIZE)
	};

	struct GetFilledOrders_output
	{
		uint32 totalActive;
		uint32 returned;
		Array<OrderHash, QSB_QUERY_MAX_PAGE_SIZE> hashes;
	};

	// ---------------------------------------------------------------------
	// State data (accessible via state.get() / state.mut() in procedures)
	// ---------------------------------------------------------------------
	struct StateData
	{
		id admin;
		id protocolFeeRecipient; // receives protocolFeeAmount
		id oracleFeeRecipient;   // receives oracleFeeAmount
		Array<RoleEntry, QSB_MAX_ORACLES> oracles;
		Array<RoleEntry, QSB_MAX_PAUSERS> pausers;
		Array<FilledOrderEntry, QSB_MAX_FILLED_ORDERS> filledOrders;
		Array<LockedOrderEntry, QSB_MAX_LOCKED_ORDERS> lockedOrders;
		uint32 lastLockedOrdersNextOverwriteIdx;
		uint32 lastFilledOrdersNextOverwriteIdx;
		uint32 oracleCount;
		uint32 pauserCount;
		uint32 bpsFee;               // fee taken in BPS (base 10000) from netAmount
		uint32 protocolFee;          // percent of BPS fee sent to protocol (base 100)
		uint8 oracleThreshold; // percent [1..100]
		bit paused;
	};

protected:

	// ---------------------------------------------------------------------
	// Internal helpers
	// ---------------------------------------------------------------------

	// Truncate digest to OrderHash (full 32 bytes)
	inline static void digestToOrderHash(const id& digest, OrderHash& outHash)
	{
		// Copy digest directly to OrderHash (both are 32 bytes)
		// Use setMem which handles 32-byte types specially
		outHash.setMem(digest);
	}

	// Check if caller is current admin (or if admin is not yet set, allow bootstrap)
	inline static bool isAdmin(const QPI::ContractState<StateData, CONTRACT_INDEX>& state, const id& who)
	{
		if (isZero(state.get().admin))
			return true;
		return who == state.get().admin;
	}

	// Check if caller is admin or has pauser role
	inline static bool isAdminOrPauser(const QPI::ContractState<StateData, CONTRACT_INDEX>& state, const id& who, uint32 i)
	{
		if (isAdmin(state, who))
			return true;

		for (i = 0; i < state.get().pausers.capacity(); ++i)
		{
			if (state.get().pausers.get(i).active && state.get().pausers.get(i).account == who)
				return true;
		}
		return false;
	}

	// Find oracle index; returns NULL_INDEX if not found
	inline static sint64 findOracleIndex(const QPI::ContractState<StateData, CONTRACT_INDEX>& state, const id& account, uint32 i)
	{
		for (i = 0; i < state.get().oracles.capacity(); ++i)
		{
			if (state.get().oracles.get(i).active && state.get().oracles.get(i).account == account)
				return (sint32)i;
		}
		return NULL_INDEX;
	}

	// Find pauser index; returns NULL_INDEX if not found
	inline static sint64 findPauserIndex(const QPI::ContractState<StateData, CONTRACT_INDEX>& state, const id& account, uint32 i)
	{
		for (i = 0; i < state.get().pausers.capacity(); ++i)
		{
			if (state.get().pausers.get(i).active && state.get().pausers.get(i).account == account)
				return (sint32)i;
		}
		return NULL_INDEX;
	}

	// Clear a locked order entry so its slot can be reused
	inline static void clearLockedOrderEntry(LockedOrderEntry& entry)
	{
		entry.active = false;
		entry.lockEpoch = 0;
		entry.sender = 0;
		entry.networkOut = 0;
		entry.amount = 0;
		entry.relayerFee = 0;
		entry.nonce = 0;
		setMemory(entry.toAddress, 0);
		setMemory(entry.orderHash, 0);
	}

	// Mark an orderHash as filled (idempotent, ring-buffer storage)
	inline static void markOrderFilled(QPI::ContractState<StateData, CONTRACT_INDEX>& state, const OrderHash& hash, uint32 i, uint32 j, bool same, FilledOrderEntry& entry)
	{
		// First, see if it already exists
		for (i = 0; i < state.get().filledOrders.capacity(); ++i)
		{
			entry = state.get().filledOrders.get(i);
			if (entry.used)
			{
				same = true;
				for (j = 0; j < hash.capacity(); ++j)
				{
					if (entry.hash.get(j) != hash.get(j))
					{
						same = false;
						break;
					}
				}
				if (same)
					return;
			}
		}

		// Otherwise, insert into the next ring-buffer slot and advance the index.
		i = state.get().lastFilledOrdersNextOverwriteIdx;
		entry = state.get().filledOrders.get(i);
		entry.hash = hash;
		entry.used = true;
		state.mut().filledOrders.set(i, entry);
		state.mut().lastFilledOrdersNextOverwriteIdx =
			(state.get().lastFilledOrdersNextOverwriteIdx + 1) & (QSB_MAX_FILLED_ORDERS - 1);
	}

	// Check whether an orderHash has already been filled
	inline static bit isOrderFilled(const QPI::ContractState<StateData, CONTRACT_INDEX>& state, const OrderHash& hash, uint32 i, uint32 j, bool same, FilledOrderEntry& entry)
	{
		for (i = 0; i < state.get().filledOrders.capacity(); ++i)
		{
			entry = state.get().filledOrders.get(i);
			if (!entry.used)
				continue;

			same = true;
			for (j = 0; j < hash.capacity(); ++j)
			{
				if (entry.hash.get(j) != hash.get(j))
				{
					same = false;
					break;
				}
			}
			if (same)
				return true;
		}
		return false;
	}

	// Find index of locked order by nonce; returns NULL_INDEX if not found
	inline static sint64 findLockedOrderIndexByNonce(const QPI::ContractState<StateData, CONTRACT_INDEX>& state, uint32 nonce, uint32 i)
	{
		for (i = 0; i < QSB_MAX_LOCKED_ORDERS; ++i)
		{
			if (state.get().lockedOrders.get(i).active && state.get().lockedOrders.get(i).nonce == nonce)
				return (sint32)i;
		}
		return NULL_INDEX;
	}

public:
	// ---------------------------------------------------------------------
	// Core user procedures
	// ---------------------------------------------------------------------

	struct Lock_locals
	{
		id digest;
		LockedOrderEntry existing;
		Order tmpOrder;
		LockedOrderEntry entry;
		uint32 i;
		QSBLogLockMessage logMsg;
	};

	PUBLIC_PROCEDURE_WITH_LOCALS(Lock)
	{
		locals.logMsg._contractIndex = SELF_INDEX;
		locals.logMsg._type = QSBLogLock;
		locals.logMsg.from = qpi.invocator();
		copyFromBuffer(locals.logMsg.to, input.toAddress);
		locals.logMsg.amount = input.amount;
		locals.logMsg.relayerFee = input.relayerFee;
		locals.logMsg.networkOut = input.networkOut;
		locals.logMsg.nonce = input.nonce;
		setMemory(locals.logMsg.orderHash, 0);
		locals.logMsg.success = 0;
		locals.logMsg.reasonCode = QSBReasonNone;
		locals.logMsg._terminator = 0;

		output.success = false;
		setMemory(output.orderHash, 0);

		// Must not be paused
		if (state.get().paused)
		{
			// Refund attached funds if any
			if (qpi.invocationReward() > 0)
			{
				qpi.transfer(qpi.invocator(), qpi.invocationReward());
			}
			locals.logMsg.reasonCode = QSBReasonPaused;
			LOG_INFO(locals.logMsg);
			return;
		}

		// Basic validation
		if (input.amount == 0 || input.relayerFee >= input.amount)
		{
			if (qpi.invocationReward() > 0)
			{
				qpi.transfer(qpi.invocator(), qpi.invocationReward());
			}
			locals.logMsg.reasonCode = QSBReasonInvalidAmount;
			LOG_INFO(locals.logMsg);
			return;
		}

		// Ensure funds sent with call match the amount to be locked
		if (qpi.invocationReward() < (sint64)input.amount)
		{
			if (qpi.invocationReward() > 0)
			{
				qpi.transfer(qpi.invocator(), qpi.invocationReward());
			}
			locals.logMsg.reasonCode = QSBReasonInsufficientReward;
			LOG_INFO(locals.logMsg);
			return;
		}

		// Any excess over `amount` is refunded
		if (qpi.invocationReward() > (sint64)input.amount)
		{
			qpi.transfer(qpi.invocator(), qpi.invocationReward() - input.amount);
		}

		// Funds equal to `amount` now remain locked in the contract balance

		// Ensure nonce unused
		if (findLockedOrderIndexByNonce(state, input.nonce, 0) != NULL_INDEX)
		{
			// Nonce already used; reject
			qpi.transfer(qpi.invocator(), input.amount);
			locals.logMsg.reasonCode = QSBReasonNonceUsed;
			LOG_INFO(locals.logMsg);
			return;
		}

		// Construct a lightweight order object for hashing and event usage
		locals.tmpOrder.destinationChainId = input.networkOut;
		locals.tmpOrder.networkIn = 0; // Qubic network id (can be refined off-chain)
		locals.tmpOrder.networkOut = input.networkOut;
		locals.tmpOrder.tokenIn = 0;
		locals.tmpOrder.tokenOut = 0;
		locals.tmpOrder.fromAddress = qpi.invocator();
		locals.tmpOrder.toAddress = NULL_ID; // Off-chain destination encoded only in toAddress bytes
		locals.tmpOrder.amount = input.amount;
		locals.tmpOrder.relayerFee = input.relayerFee;
		locals.tmpOrder.nonce = input.nonce;

		locals.digest = qpi.K12(locals.tmpOrder);
		digestToOrderHash(locals.digest, output.orderHash);
		locals.logMsg.orderHash = output.orderHash;

		// Persist locked order so that overrideLock or off-chain tooling can reference it.
		locals.entry.active = true;
		locals.entry.sender = qpi.invocator();
		locals.entry.networkOut = input.networkOut;
		locals.entry.amount = input.amount;
		locals.entry.relayerFee = input.relayerFee;
		locals.entry.nonce = input.nonce;
		copyMemory(locals.entry.toAddress, input.toAddress);
		locals.entry.orderHash = output.orderHash;
		locals.entry.lockEpoch = qpi.epoch();
		state.mut().lockedOrders.set(state.get().lastLockedOrdersNextOverwriteIdx, locals.entry);

		// always overwrite the next slot, wrapping around with a power-of-two mask.
		state.mut().lastLockedOrdersNextOverwriteIdx = (state.get().lastLockedOrdersNextOverwriteIdx + 1) & (QSB_MAX_LOCKED_ORDERS - 1);

		output.success = true;
		locals.logMsg.success = 1;
		locals.logMsg.reasonCode = QSBReasonNone;
		LOG_INFO(locals.logMsg);
	}

	struct OverrideLock_locals
	{
		LockedOrderEntry entry;
		Order tmpOrder;
		id digest;
		sint64 idx;
		QSBLogOverrideLockMessage logMsg;
	};

	PUBLIC_PROCEDURE_WITH_LOCALS(OverrideLock)
	{
		locals.logMsg._contractIndex = SELF_INDEX;
		locals.logMsg._type = QSBLogOverrideLock;
		locals.logMsg.from = qpi.invocator();
		setMemory(locals.logMsg.to, 0);
		locals.logMsg.amount = 0;
		locals.logMsg.relayerFee = 0;
		locals.logMsg.networkOut = 0;
		locals.logMsg.nonce = input.nonce;
		setMemory(locals.logMsg.orderHash, 0);
		locals.logMsg.success = 0;
		locals.logMsg.reasonCode = QSBReasonNone;
		locals.logMsg._terminator = 0;
		output.success = false;
		setMemory(output.orderHash, 0);

		// Always refund invocationReward (locking was done in original lock() call)
		if (qpi.invocationReward() > 0)
		{
			qpi.transfer(qpi.invocator(), qpi.invocationReward());
		}

		// Contract must not be paused
		if (state.get().paused)
		{
			locals.logMsg.reasonCode = QSBReasonPaused;
			LOG_INFO(locals.logMsg);
			return;
		}

		// Find existing order by nonce
		locals.idx = findLockedOrderIndexByNonce(state, input.nonce, 0);
		if (locals.idx == NULL_INDEX)
		{
			locals.logMsg.reasonCode = QSBReasonNonceUsed;
			LOG_INFO(locals.logMsg);
			return;
		}

		locals.entry = state.get().lockedOrders.get((uint32)locals.idx);

		// Only original sender can override
		if (locals.entry.sender != qpi.invocator())
		{
			locals.logMsg.reasonCode = QSBReasonNotSender;
			LOG_INFO(locals.logMsg);
			return;
		}

		// Validate new relayer fee
		if (input.relayerFee >= locals.entry.amount)
		{
			locals.logMsg.reasonCode = QSBReasonBadRelayerFee;
			LOG_INFO(locals.logMsg);
			return;
		}

		// Update mutable fields
		copyMemory(locals.entry.toAddress, input.toAddress);
		locals.entry.relayerFee = input.relayerFee;

		// Rebuild order for hashing
		locals.tmpOrder.destinationChainId = locals.entry.networkOut;
		locals.tmpOrder.networkIn = 0;
		locals.tmpOrder.networkOut = locals.entry.networkOut;
		locals.tmpOrder.tokenIn = 0;
		locals.tmpOrder.tokenOut = 0;
		locals.tmpOrder.fromAddress = locals.entry.sender;
		locals.tmpOrder.toAddress = NULL_ID;
		locals.tmpOrder.amount = locals.entry.amount;
		locals.tmpOrder.relayerFee = locals.entry.relayerFee;
		locals.tmpOrder.nonce = locals.entry.nonce;

		locals.digest = qpi.K12(locals.tmpOrder);
		digestToOrderHash(locals.digest, locals.entry.orderHash);;
		output.orderHash = locals.entry.orderHash;
		locals.logMsg.orderHash = locals.entry.orderHash;

		state.mut().lockedOrders.set((uint32)locals.idx, locals.entry);
		output.success = true;
		copyFromBuffer(locals.logMsg.to, input.toAddress);
		locals.logMsg.amount = locals.entry.amount;
		locals.logMsg.relayerFee = locals.entry.relayerFee;
		locals.logMsg.networkOut = locals.entry.networkOut;
		locals.logMsg.success = 1;
		locals.logMsg.reasonCode = QSBReasonNone;
		LOG_INFO(locals.logMsg);
	}

	// View helpers
	PUBLIC_FUNCTION(GetConfig)
	{
		output.admin = state.get().admin;
		output.protocolFeeRecipient = state.get().protocolFeeRecipient;
		output.oracleFeeRecipient = state.get().oracleFeeRecipient;
		output.bpsFee = state.get().bpsFee;
		output.protocolFee = state.get().protocolFee;
		output.oracleCount = state.get().oracleCount;
		output.pauserCount = state.get().pauserCount;
		output.oracleThreshold = state.get().oracleThreshold;
		output.paused = state.get().paused;
	}

	PUBLIC_FUNCTION(IsOracle)
	{
		output.isOracle = (findOracleIndex(state, input.account, 0) != NULL_INDEX);
	}

	PUBLIC_FUNCTION(IsPauser)
	{
		output.isPauser = (findPauserIndex(state, input.account, 0) != NULL_INDEX);
	}

	struct GetLockedOrder_locals
	{
		sint64 idx;
	};

	PUBLIC_FUNCTION_WITH_LOCALS(GetLockedOrder)
	{
		locals.idx = findLockedOrderIndexByNonce(state, input.nonce, 0);
		output.exists = (locals.idx != NULL_INDEX);
		if (output.exists)
		{
			output.order = state.get().lockedOrders.get((uint32)locals.idx);
		}
	}

	struct IsOrderFilled_locals
	{
		FilledOrderEntry entry;
		bool same;
	};

	PUBLIC_FUNCTION_WITH_LOCALS(IsOrderFilled)
	{
		output.filled = isOrderFilled(state, input.hash, 0, 0, locals.same, locals.entry);
	}

	struct ComputeOrderHash_locals
	{
		id digest;
	};

	PUBLIC_FUNCTION_WITH_LOCALS(ComputeOrderHash)
	{
		locals.digest = qpi.K12(input.order);
		output.hash.setMem(locals.digest);
	}

	struct GetOracles_locals
	{
		uint32 i;
		RoleEntry entry;
	};

	PUBLIC_FUNCTION_WITH_LOCALS(GetOracles)
	{
		output.count = 0;
		setMemory(output.accounts, 0);
		for (locals.i = 0; locals.i < state.get().oracles.capacity() && output.count < output.accounts.capacity(); ++locals.i)
		{
			locals.entry = state.get().oracles.get(locals.i);
			if (locals.entry.active)
			{
				output.accounts.set(output.count, locals.entry.account);
				++output.count;
			}
		}
	}

	struct GetPausers_locals
	{
		uint32 i;
		RoleEntry entry;
	};

	PUBLIC_FUNCTION_WITH_LOCALS(GetPausers)
	{
		output.count = 0;
		setMemory(output.accounts, 0);
		for (locals.i = 0; locals.i < state.get().pausers.capacity() && output.count < output.accounts.capacity(); ++locals.i)
		{
			locals.entry = state.get().pausers.get(locals.i);
			if (locals.entry.active)
			{
				output.accounts.set(output.count, locals.entry.account);
				++output.count;
			}
		}
	}

	struct GetLockedOrders_locals
	{
		uint32 i;
		uint32 totalActive;
		uint32 collected;
		uint32 effectiveLimit;
		LockedOrderEntry entry;
	};

	PUBLIC_FUNCTION_WITH_LOCALS(GetLockedOrders)
	{
		output.totalActive = 0;
		output.returned = 0;
		setMemory(output.entries, 0);
		locals.effectiveLimit = input.limit;
		if (locals.effectiveLimit > QSB_QUERY_MAX_PAGE_SIZE)
			locals.effectiveLimit = QSB_QUERY_MAX_PAGE_SIZE;
		locals.collected = 0;
		for (locals.i = 0; locals.i < state.get().lockedOrders.capacity(); ++locals.i)
		{
			locals.entry = state.get().lockedOrders.get(locals.i);
			if (!locals.entry.active)
				continue;
			++locals.totalActive;
			if (locals.totalActive <= input.offset)
				continue;
			if (locals.collected >= locals.effectiveLimit)
				continue;
			output.entries.set(locals.collected, locals.entry);
			++locals.collected;
		}
		output.totalActive = locals.totalActive;
		output.returned = locals.collected;
	}

	struct GetFilledOrders_locals
	{
		uint32 i;
		uint32 totalActive;
		uint32 collected;
		uint32 effectiveLimit;
		FilledOrderEntry entry;
	};

	PUBLIC_FUNCTION_WITH_LOCALS(GetFilledOrders)
	{
		output.totalActive = 0;
		output.returned = 0;
		setMemory(output.hashes, 0);
		locals.effectiveLimit = input.limit;
		if (locals.effectiveLimit > QSB_QUERY_MAX_PAGE_SIZE)
			locals.effectiveLimit = QSB_QUERY_MAX_PAGE_SIZE;
		locals.collected = 0;
		for (locals.i = 0; locals.i < state.get().filledOrders.capacity(); ++locals.i)
		{
			locals.entry = state.get().filledOrders.get(locals.i);
			if (!locals.entry.used)
				continue;
			++locals.totalActive;
			if (locals.totalActive <= input.offset)
				continue;
			if (locals.collected >= locals.effectiveLimit)
				continue;
			output.hashes.set(locals.collected, locals.entry.hash);
			++locals.collected;
		}
		output.totalActive = locals.totalActive;
		output.returned = locals.collected;
	}

	struct Unlock_locals
	{
		id digest;
		OrderHash hash;
		uint32 validSignatureCount;
		uint32 requiredSignatures;
		FilledOrderEntry entry;
		Array<id, QSB_MAX_ORACLES> seenSigners;
		SignatureData sig;
		uint32 seenCount;
		uint32 i;
		uint32 j;
		uint64 netAmount;
		uint128 tmpMul;
		uint128 tmpMul2;
		uint64 bpsFeeAmount;
		uint64 protocolFeeAmount;
		uint64 oracleFeeAmount;
		uint64 recipientAmount;
		bool same;
		bool allTransfersOk;
		Entity entity;
		uint64 contractBalance;
		QSBLogUnlockMessage logMsg;
	};

	PUBLIC_PROCEDURE_WITH_LOCALS(Unlock)
	{	
		locals.logMsg._contractIndex = SELF_INDEX;
		locals.logMsg._type = QSBLogUnlock;
		setMemory(locals.logMsg.orderHash, 0);
		locals.logMsg.toAddress = input.order.toAddress;
		locals.logMsg.amount = input.order.amount;
		locals.logMsg.relayerFee = input.order.relayerFee;
		locals.logMsg.relayer = qpi.invocator();
		locals.logMsg.success = 0;
		locals.logMsg.reasonCode = QSBReasonNone;
		locals.logMsg._terminator = 0;
		output.success = false;
		setMemory(output.orderHash, 0);

		// Must not be paused
		if (state.get().paused)
		{
			if (qpi.invocationReward() > 0)
			{
				qpi.transfer(qpi.invocator(), qpi.invocationReward());
			}
			locals.logMsg.reasonCode = QSBReasonPaused;
			LOG_INFO(locals.logMsg);
			return;
		}

		// Refund any invocation reward (relayer is paid from order.amount, not from reward)
		if (qpi.invocationReward() > 0)
		{
			qpi.transfer(qpi.invocator(), qpi.invocationReward());
		}

		// Basic order validation
		if (input.order.amount == 0 || input.order.relayerFee >= input.order.amount)
		{
			locals.logMsg.reasonCode = QSBReasonInvalidAmount;
			LOG_INFO(locals.logMsg);
			return;
		}

		// Check that the contract has enough balance to cover the full order amount.
		// This should never fail under normal circumstances (Lock keeps funds inside the contract), but we guard against any unexpected balance discrepancies.
		qpi.getEntity(SELF, locals.entity);
		if (locals.entity.incomingAmount < locals.entity.outgoingAmount)
		{
			locals.contractBalance = 0;
		}
		else
		{
			locals.contractBalance = locals.entity.incomingAmount - locals.entity.outgoingAmount;
		}

		if (locals.contractBalance < input.order.amount)
		{
			locals.logMsg.reasonCode = QSBReasonInsufficientReward;
			LOG_INFO(locals.logMsg);
			return;
		}

		// NOTE: We intentionally do not require a matching lock() entry here.
		// Unlock is driven solely by:
		//  - oracle signatures over the burn/unlock order (on the other chain),
		//  - replay protection via filledOrders,
		//  - and balance checks on this contract.
		// This matches a fungible lock/mint ↔ burn/unlock bridge model where
		// minted tokens can be freely transferred and aggregated, and where
		// individual locks are not tied 1:1 to specific unlocks.

		// Compute digest and orderHash
		locals.digest = qpi.K12(input.order);
		digestToOrderHash(locals.digest, locals.hash);
		output.orderHash = locals.hash;
		locals.logMsg.orderHash = locals.hash;

		// Ensure orderHash not yet filled
		if (isOrderFilled(state, locals.hash, 0, 0, 0, locals.entry))
		{
			locals.logMsg.reasonCode = QSBReasonAlreadyFilled;
			LOG_INFO(locals.logMsg);
			return;
		}

		// Verify oracle signatures against threshold
		if (state.get().oracleCount == 0 || input.numSignatures == 0)
		{
			locals.logMsg.reasonCode = QSBReasonNoOracles;
			LOG_INFO(locals.logMsg);
			return;
		}

		// requiredSignatures = ceil(oracleCount * oracleThreshold / 100)
		locals.tmpMul  = uint128(state.get().oracleCount) * uint128(state.get().oracleThreshold);
		locals.tmpMul2 = div(locals.tmpMul, uint128(100));
		locals.requiredSignatures = (uint32)locals.tmpMul2.low;
		if (locals.requiredSignatures * 100 < state.get().oracleCount * state.get().oracleThreshold)
		{
			++locals.requiredSignatures;
		}
		if (locals.requiredSignatures == 0)
		{
			locals.requiredSignatures = 1;
		}

		locals.validSignatureCount = 0;
		locals.seenCount = 0;

		for (locals.i = 0; locals.i < input.numSignatures && locals.i < input.signatures.capacity(); ++locals.i)
		{
			locals.sig = input.signatures.get(locals.i);

			// Check signer is authorized oracle
			if (findOracleIndex(state, locals.sig.signer, 0) == NULL_INDEX)
			{
				locals.logMsg.reasonCode = QSBReasonInvalidSignature;
				LOG_INFO(locals.logMsg); // unknown signer -> fail fast
				return;
			}

			// Check duplicates
			for (locals.j = 0; locals.j < locals.seenCount; ++locals.j)
			{
				if (locals.seenSigners.get(locals.j) == locals.sig.signer)
				{
					locals.logMsg.reasonCode = QSBReasonDuplicateSigner;
					LOG_INFO(locals.logMsg); // duplicate signer -> fail
					return;
				}
			}

			// Verify signature
			if (!qpi.signatureValidity(locals.sig.signer, locals.digest, locals.sig.signature))
			{
				locals.logMsg.reasonCode = QSBReasonInvalidSignature;
				LOG_INFO(locals.logMsg);
				return;
			}

			// Record signer and increment count
			if (locals.seenCount < locals.seenSigners.capacity())
			{
				locals.seenSigners.set(locals.seenCount, locals.sig.signer);
				++locals.seenCount;
			}
			++locals.validSignatureCount;
		}

		if (locals.validSignatureCount < locals.requiredSignatures)
		{
			locals.logMsg.reasonCode = QSBReasonThresholdFailed;
			LOG_INFO(locals.logMsg);
			return;
		}

		// -----------------------------------------------------------------
		// Fee calculations
		// -----------------------------------------------------------------
		locals.netAmount = input.order.amount - input.order.relayerFee;

		// bpsFeeAmount = netAmount * bpsFee / 10000
		locals.tmpMul  = uint128(locals.netAmount) * uint128(state.get().bpsFee);
		locals.tmpMul2 = div(locals.tmpMul, uint128(10000));
		locals.bpsFeeAmount = (uint64)locals.tmpMul2.low;

		// protocolFeeAmount = bpsFeeAmount * protocolFee / 100
		locals.tmpMul  = uint128(locals.bpsFeeAmount) * uint128(state.get().protocolFee);
		locals.tmpMul2 = div(locals.tmpMul, uint128(100));
		locals.protocolFeeAmount = (uint64)locals.tmpMul2.low;

		// oracleFeeAmount = bpsFeeAmount - protocolFeeAmount
		if (locals.bpsFeeAmount >= locals.protocolFeeAmount)
			locals.oracleFeeAmount = locals.bpsFeeAmount - locals.protocolFeeAmount;
		else
			locals.oracleFeeAmount = 0;

		// recipientAmount = netAmount - bpsFeeAmount
		if (locals.netAmount >= locals.bpsFeeAmount)
			locals.recipientAmount = locals.netAmount - locals.bpsFeeAmount;
		else
			locals.recipientAmount = 0;

		// -----------------------------------------------------------------
		// Token transfers
		// -----------------------------------------------------------------

		locals.allTransfersOk = true;

		// Relayer fee to caller
		if (input.order.relayerFee > 0)
		{
			if (qpi.transfer(qpi.invocator(), (sint64)input.order.relayerFee) < 0)
			{
				locals.allTransfersOk = false;
			}
		}

		// Protocol fee
		if (locals.protocolFeeAmount > 0 && !isZero(state.get().protocolFeeRecipient))
		{
			if (qpi.transfer(state.get().protocolFeeRecipient, (sint64)locals.protocolFeeAmount) < 0)
			{
				locals.allTransfersOk = false;
			}
		}

		// Oracle fee
		if (locals.oracleFeeAmount > 0 && !isZero(state.get().oracleFeeRecipient))
		{
			if (qpi.transfer(state.get().oracleFeeRecipient, (sint64)locals.oracleFeeAmount) < 0)
			{
				locals.allTransfersOk = false;
			}
		}

		// Recipient payout
		if (locals.recipientAmount > 0 && !isZero(input.order.toAddress))
		{
			if (qpi.transfer(input.order.toAddress, (sint64)locals.recipientAmount) < 0)
			{
				locals.allTransfersOk = false;
			}
		}

		// If any transfer failed, do not mark the order as filled
		if (!locals.allTransfersOk)
		{
			locals.logMsg.reasonCode = QSBReasonTransferFailed;
			LOG_INFO(locals.logMsg);
			return;
		}

		// Mark order as filled
		markOrderFilled(state, locals.hash, 0, 0, 0, locals.entry);

		output.success = true;
		locals.logMsg.success = 1;
		locals.logMsg.reasonCode = QSBReasonNone;
		LOG_INFO(locals.logMsg);
	}

	// ---------------------------------------------------------------------
	// Admin procedures
	// ---------------------------------------------------------------------

	struct TransferAdmin_locals
	{
		QSBLogAdminTransferredMessage logMsg;
	};

	PUBLIC_PROCEDURE_WITH_LOCALS(TransferAdmin)
	{
		locals.logMsg._contractIndex = SELF_INDEX;
		locals.logMsg._type = QSBLogAdminTransferred;
		locals.logMsg.previousAdmin = state.get().admin;
		locals.logMsg.newAdmin = input.newAdmin;
		locals.logMsg.success = 0;
		locals.logMsg.reasonCode = QSBReasonNone;
		locals.logMsg._terminator = 0;

		output.success = false;

		// Refund any attached funds
		if (qpi.invocationReward() > 0)
		{
			qpi.transfer(qpi.invocator(), qpi.invocationReward());
		}

		if (!isAdmin(state, qpi.invocator()))
		{
			locals.logMsg.reasonCode = QSBReasonNotAdmin;
			LOG_INFO(locals.logMsg);
			return;
		}

		state.mut().admin = input.newAdmin;
		output.success = true;
		locals.logMsg.success = 1;
		LOG_INFO(locals.logMsg);
	}

	struct EditOracleThreshold_locals
	{
		QSBLogThresholdUpdatedMessage logMsg;
	};

	PUBLIC_PROCEDURE_WITH_LOCALS(EditOracleThreshold)
	{
		output.success = false;
		output.oldThreshold = state.get().oracleThreshold;

		if (qpi.invocationReward() > 0)
		{
			qpi.transfer(qpi.invocator(), qpi.invocationReward());
		}

		if (!isAdmin(state, qpi.invocator()))
		{
			locals.logMsg._contractIndex = SELF_INDEX;
			locals.logMsg._type = QSBLogThresholdUpdated;
			locals.logMsg.oldThreshold = output.oldThreshold;
			locals.logMsg.newThreshold = input.newThreshold;
			locals.logMsg.success = 0;
			locals.logMsg.reasonCode = QSBReasonNotAdmin;
			locals.logMsg._terminator = 0;
			LOG_INFO(locals.logMsg);
			return;
		}

		if (input.newThreshold == 0 || input.newThreshold > 100)
		{
			locals.logMsg._contractIndex = SELF_INDEX;
			locals.logMsg._type = QSBLogThresholdUpdated;
			locals.logMsg.oldThreshold = output.oldThreshold;
			locals.logMsg.newThreshold = input.newThreshold;
			locals.logMsg.success = 0;
			locals.logMsg.reasonCode = QSBReasonInvalidThreshold;
			locals.logMsg._terminator = 0;
			LOG_INFO(locals.logMsg);
			return;
		}

		state.mut().oracleThreshold  = input.newThreshold;
		output.success   = true;
		locals.logMsg._contractIndex = SELF_INDEX;
		locals.logMsg._type = QSBLogThresholdUpdated;
		locals.logMsg.oldThreshold = output.oldThreshold;
		locals.logMsg.newThreshold = input.newThreshold;
		locals.logMsg.success = 1;
		locals.logMsg.reasonCode = QSBReasonNone;
		locals.logMsg._terminator = 0;
		LOG_INFO(locals.logMsg);
	}

	struct AddRole_locals
	{
		RoleEntry entry;
		uint32 i;
		QSBLogRoleMessage logMsg;
	};

	PUBLIC_PROCEDURE_WITH_LOCALS(AddRole)
	{
		output.success = false;

		if (qpi.invocationReward() > 0)
		{
			qpi.transfer(qpi.invocator(), qpi.invocationReward());
		}

		if (!isAdmin(state, qpi.invocator()))
		{
			locals.logMsg._contractIndex = SELF_INDEX;
			locals.logMsg._type = QSBLogRoleGranted;
			locals.logMsg.role = input.role;
			locals.logMsg.account = input.account;
			locals.logMsg.caller = qpi.invocator();
			locals.logMsg.success = 0;
			locals.logMsg.reasonCode = QSBReasonNotAdmin;
			locals.logMsg._terminator = 0;
			LOG_INFO(locals.logMsg);
			return;
		}

		if (input.role == (uint8)Role::Oracle)
		{
			if (findOracleIndex(state, input.account, 0) != NULL_INDEX)
			{
				output.success = true;
				locals.logMsg._contractIndex = SELF_INDEX;
				locals.logMsg._type = QSBLogRoleGranted;
				locals.logMsg.role = input.role;
				locals.logMsg.account = input.account;
				locals.logMsg.caller = qpi.invocator();
				locals.logMsg.success = 0;
				locals.logMsg.reasonCode = QSBReasonRoleExists;
				locals.logMsg._terminator = 0;
				LOG_INFO(locals.logMsg);
				return;
			}

			for (locals.i = 0; locals.i < state.get().oracles.capacity(); ++locals.i)
			{
				locals.entry = state.get().oracles.get(locals.i);
				if (!locals.entry.active)
				{
					locals.entry.account = input.account;
					locals.entry.active  = true;
					state.mut().oracles.set(locals.i, locals.entry);
					++state.mut().oracleCount;
					output.success = true;
					locals.logMsg._contractIndex = SELF_INDEX;
					locals.logMsg._type = QSBLogRoleGranted;
					locals.logMsg.role = input.role;
					locals.logMsg.account = input.account;
					locals.logMsg.caller = qpi.invocator();
					locals.logMsg.success = 1;
					locals.logMsg.reasonCode = QSBReasonNone;
					locals.logMsg._terminator = 0;
					LOG_INFO(locals.logMsg);
					return;
				}
			}
		}
		else if (input.role == (uint8)Role::Pauser)
		{
			if (findPauserIndex(state, input.account, 0) != NULL_INDEX)
			{
				output.success = true;
				locals.logMsg._contractIndex = SELF_INDEX;
				locals.logMsg._type = QSBLogRoleGranted;
				locals.logMsg.role = input.role;
				locals.logMsg.account = input.account;
				locals.logMsg.caller = qpi.invocator();
				locals.logMsg.success = 0;
				locals.logMsg.reasonCode = QSBReasonRoleExists;
				locals.logMsg._terminator = 0;
				LOG_INFO(locals.logMsg);
				return;
			}

			for (locals.i = 0; locals.i < state.get().pausers.capacity(); ++locals.i)
			{
				locals.entry = state.get().pausers.get(locals.i);
				if (!locals.entry.active)
				{
					locals.entry.account = input.account;
					locals.entry.active  = true;
					state.mut().pausers.set(locals.i, locals.entry);
					++state.mut().pauserCount;
					output.success = true;
					locals.logMsg._contractIndex = SELF_INDEX;
					locals.logMsg._type = QSBLogRoleGranted;
					locals.logMsg.role = input.role;
					locals.logMsg.account = input.account;
					locals.logMsg.caller = qpi.invocator();
					locals.logMsg.success = 1;
					locals.logMsg.reasonCode = QSBReasonNone;
					locals.logMsg._terminator = 0;
					LOG_INFO(locals.logMsg);
					return;
				}
			}
		}
	}

	struct RemoveRole_locals
	{
		RoleEntry entry;
		sint64 idx;
		QSBLogRoleMessage logMsg;
	};

	PUBLIC_PROCEDURE_WITH_LOCALS(RemoveRole)
	{
		output.success = false;

		if (qpi.invocationReward() > 0)
		{
			qpi.transfer(qpi.invocator(), qpi.invocationReward());
		}

		if (!isAdmin(state, qpi.invocator()))
		{
			locals.logMsg._contractIndex = SELF_INDEX;
			locals.logMsg._type = QSBLogRoleRevoked;
			locals.logMsg.role = input.role;
			locals.logMsg.account = input.account;
			locals.logMsg.caller = qpi.invocator();
			locals.logMsg.success = 0;
			locals.logMsg.reasonCode = QSBReasonNotAdmin;
			locals.logMsg._terminator = 0;
			LOG_INFO(locals.logMsg);
			return;
		}

		if (input.role == (uint8)Role::Oracle)
		{
			locals.idx = findOracleIndex(state, input.account, 0);
			if (locals.idx != NULL_INDEX)
			{
				locals.entry = state.get().oracles.get((uint32)locals.idx);
				locals.entry.active = false;
				state.mut().oracles.set((uint32)locals.idx, locals.entry);
				if (state.get().oracleCount > 0)
					--state.mut().oracleCount;
				output.success = true;

				locals.logMsg._contractIndex = SELF_INDEX;
				locals.logMsg._type = QSBLogRoleRevoked;
				locals.logMsg.role = input.role;
				locals.logMsg.account = input.account;
				locals.logMsg.caller = qpi.invocator();
				locals.logMsg.success = 1;
				locals.logMsg.reasonCode = QSBReasonNone;
				locals.logMsg._terminator = 0;
				LOG_INFO(locals.logMsg);
			}
			else
			{
				locals.logMsg._contractIndex = SELF_INDEX;
				locals.logMsg._type = QSBLogRoleRevoked;
				locals.logMsg.role = input.role;
				locals.logMsg.account = input.account;
				locals.logMsg.caller = qpi.invocator();
				locals.logMsg.success = 0;
				locals.logMsg.reasonCode = QSBReasonRoleMissing;
				locals.logMsg._terminator = 0;
				LOG_INFO(locals.logMsg);
			}
		}
		else if (input.role == (uint8)Role::Pauser)
		{
			locals.idx = findPauserIndex(state, input.account, 0);
			if (locals.idx != NULL_INDEX)
			{
				locals.entry = state.get().pausers.get((uint32)locals.idx);
				locals.entry.active = false;
				state.mut().pausers.set((uint32)locals.idx, locals.entry);
				if (state.get().pauserCount > 0)
					--state.mut().pauserCount;
				output.success = true;

				locals.logMsg._contractIndex = SELF_INDEX;
				locals.logMsg._type = QSBLogRoleRevoked;
				locals.logMsg.role = input.role;
				locals.logMsg.account = input.account;
				locals.logMsg.caller = qpi.invocator();
				locals.logMsg.success = 1;
				locals.logMsg.reasonCode = QSBReasonNone;
				locals.logMsg._terminator = 0;
				LOG_INFO(locals.logMsg);
			}
			else
			{
				locals.logMsg._contractIndex = SELF_INDEX;
				locals.logMsg._type = QSBLogRoleRevoked;
				locals.logMsg.role = input.role;
				locals.logMsg.account = input.account;
				locals.logMsg.caller = qpi.invocator();
				locals.logMsg.success = 0;
				locals.logMsg.reasonCode = QSBReasonRoleMissing;
				locals.logMsg._terminator = 0;
				LOG_INFO(locals.logMsg);
			}
		}
	}

	struct Pause_locals
	{
		QSBLogPausedMessage logMsg;
	};

	PUBLIC_PROCEDURE_WITH_LOCALS(Pause)
	{
		output.success = false;

		if (qpi.invocationReward() > 0)
		{
			qpi.transfer(qpi.invocator(), qpi.invocationReward());
		}

		if (!isAdminOrPauser(state, qpi.invocator(), 0))
		{
			locals.logMsg._contractIndex = SELF_INDEX;
			locals.logMsg._type = QSBLogPaused;
			locals.logMsg.caller = qpi.invocator();
			locals.logMsg.success = 0;
			locals.logMsg.reasonCode = QSBReasonNotAdminOrPauser;
			locals.logMsg._terminator = 0;
			LOG_INFO(locals.logMsg);
			return;
		}

		state.mut().paused = true;
		output.success = true;

		locals.logMsg._contractIndex = SELF_INDEX;
		locals.logMsg._type = QSBLogPaused;
		locals.logMsg.caller = qpi.invocator();
		locals.logMsg.success = 1;
		locals.logMsg.reasonCode = QSBReasonNone;
		locals.logMsg._terminator = 0;
		LOG_INFO(locals.logMsg);
	}

	struct Unpause_locals
	{
		QSBLogPausedMessage logMsg;
	};

	PUBLIC_PROCEDURE_WITH_LOCALS(Unpause)
	{
		output.success = false;

		if (qpi.invocationReward() > 0)
		{
			qpi.transfer(qpi.invocator(), qpi.invocationReward());
		}

		if (!isAdminOrPauser(state, qpi.invocator(), 0))
		{
			locals.logMsg._contractIndex = SELF_INDEX;
			locals.logMsg._type = QSBLogUnpaused;
			locals.logMsg.caller = qpi.invocator();
			locals.logMsg.success = 0;
			locals.logMsg.reasonCode = QSBReasonNotAdminOrPauser;
			locals.logMsg._terminator = 0;
			LOG_INFO(locals.logMsg);
			return;
		}

		state.mut().paused = false;
		output.success = true;

		locals.logMsg._contractIndex = SELF_INDEX;
		locals.logMsg._type = QSBLogUnpaused;
		locals.logMsg.caller = qpi.invocator();
		locals.logMsg.success = 1;
		locals.logMsg.reasonCode = QSBReasonNone;
		locals.logMsg._terminator = 0;
		LOG_INFO(locals.logMsg);
	}

	struct EditFeeParameters_locals
	{
		QSBLogFeeParametersUpdatedMessage logMsg;
	};

	PUBLIC_PROCEDURE_WITH_LOCALS(EditFeeParameters)
	{
		output.success = false;

		if (qpi.invocationReward() > 0)
		{
			qpi.transfer(qpi.invocator(), qpi.invocationReward());
		}

		if (!isAdmin(state, qpi.invocator()))
		{
			locals.logMsg._contractIndex = SELF_INDEX;
			locals.logMsg._type = QSBLogFeeParametersUpdated;
			locals.logMsg.bpsFee = state.get().bpsFee;
			locals.logMsg.protocolFee = state.get().protocolFee;
			locals.logMsg.protocolFeeRecipient = state.get().protocolFeeRecipient;
			locals.logMsg.oracleFeeRecipient = state.get().oracleFeeRecipient;
			locals.logMsg.success = 0;
			locals.logMsg.reasonCode = QSBReasonNotAdmin;
			locals.logMsg._terminator = 0;
			LOG_INFO(locals.logMsg);
			return;
		}

		// Validate fee ranges (when non-zero values are provided)
		if (input.bpsFee != 0 && input.bpsFee > QSB_MAX_BPS_FEE)
		{
			locals.logMsg._contractIndex = SELF_INDEX;
			locals.logMsg._type = QSBLogFeeParametersUpdated;
			locals.logMsg.bpsFee = state.get().bpsFee;
			locals.logMsg.protocolFee = state.get().protocolFee;
			locals.logMsg.protocolFeeRecipient = state.get().protocolFeeRecipient;
			locals.logMsg.oracleFeeRecipient = state.get().oracleFeeRecipient;
			locals.logMsg.success = 0;
			locals.logMsg.reasonCode = QSBReasonInvalidFeeParams;
			locals.logMsg._terminator = 0;
			LOG_INFO(locals.logMsg);
			return;
		}

		if (input.protocolFee != 0 && input.protocolFee > QSB_MAX_PROTOCOL_FEE)
		{
			locals.logMsg._contractIndex = SELF_INDEX;
			locals.logMsg._type = QSBLogFeeParametersUpdated;
			locals.logMsg.bpsFee = state.get().bpsFee;
			locals.logMsg.protocolFee = state.get().protocolFee;
			locals.logMsg.protocolFeeRecipient = state.get().protocolFeeRecipient;
			locals.logMsg.oracleFeeRecipient = state.get().oracleFeeRecipient;
			locals.logMsg.success = 0;
			locals.logMsg.reasonCode = QSBReasonInvalidFeeParams;
			locals.logMsg._terminator = 0;
			LOG_INFO(locals.logMsg);
			return;
		}

		// Only non-zero values are updated
		if (input.bpsFee != 0)
		{
			state.mut().bpsFee = input.bpsFee;
		}

		if (input.protocolFee != 0)
		{
			state.mut().protocolFee = input.protocolFee;
		}

		if (!isZero(input.protocolFeeRecipient))
		{
			state.mut().protocolFeeRecipient = input.protocolFeeRecipient;
		}

		if (!isZero(input.oracleFeeRecipient))
		{
			state.mut().oracleFeeRecipient = input.oracleFeeRecipient;
		}

		output.success = true;

		locals.logMsg._contractIndex = SELF_INDEX;
		locals.logMsg._type = QSBLogFeeParametersUpdated;
		locals.logMsg.bpsFee = state.get().bpsFee;
		locals.logMsg.protocolFee = state.get().protocolFee;
		locals.logMsg.protocolFeeRecipient = state.get().protocolFeeRecipient;
		locals.logMsg.oracleFeeRecipient = state.get().oracleFeeRecipient;
		locals.logMsg.success = 1;
		locals.logMsg.reasonCode = QSBReasonNone;
		locals.logMsg._terminator = 0;
		LOG_INFO(locals.logMsg);
	}

	REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
	{
		// View functions
		REGISTER_USER_FUNCTION(GetConfig, 1);
		REGISTER_USER_FUNCTION(IsOracle, 2);
		REGISTER_USER_FUNCTION(IsPauser, 3);
		REGISTER_USER_FUNCTION(GetLockedOrder, 4);
		REGISTER_USER_FUNCTION(IsOrderFilled, 5);
		REGISTER_USER_FUNCTION(ComputeOrderHash, 6);
		REGISTER_USER_FUNCTION(GetOracles, 7);
		REGISTER_USER_FUNCTION(GetPausers, 8);
		REGISTER_USER_FUNCTION(GetLockedOrders, 9);
		REGISTER_USER_FUNCTION(GetFilledOrders, 10);

		// User procedures
		REGISTER_USER_PROCEDURE(Lock, 1);
		REGISTER_USER_PROCEDURE(OverrideLock, 2);
		REGISTER_USER_PROCEDURE(Unlock, 3);

		// Admin procedures
		REGISTER_USER_PROCEDURE(TransferAdmin, 10);
		REGISTER_USER_PROCEDURE(EditOracleThreshold, 11);
		REGISTER_USER_PROCEDURE(AddRole, 12);
		REGISTER_USER_PROCEDURE(RemoveRole, 13);
		REGISTER_USER_PROCEDURE(Pause, 14);
		REGISTER_USER_PROCEDURE(Unpause, 15);
		REGISTER_USER_PROCEDURE(EditFeeParameters, 16);
	}

	// ---------------------------------------------------------------------
	// Epoch processing
	// ---------------------------------------------------------------------

	struct END_EPOCH_locals
	{
		// No periodic processing required in the current bridge design.
	};

	END_EPOCH_WITH_LOCALS()
	{
		// Intentionally left empty.
	}

	// ---------------------------------------------------------------------
	// Initialization
	// ---------------------------------------------------------------------

	INITIALIZE()
	{
		// No admin set initially; first TransferAdmin call bootstraps admin.
		state.mut().admin = id(100, 200, 300, 400);
		state.mut().paused = false;

		state.mut().oracleThreshold                   = 67; // default 67% (2/3 + 1 style)
		state.mut().lastLockedOrdersNextOverwriteIdx  = 0;
		state.mut().lastFilledOrdersNextOverwriteIdx  = 0;
		state.mut().oracleCount                       = 0;
		state.mut().pauserCount                       = 0;

		// Clear role mappings and filled order table
		setMemory(state.mut().oracles, 0);
		setMemory(state.mut().pausers, 0);
		setMemory(state.mut().filledOrders, 0);
		setMemory(state.mut().lockedOrders, 0);

		// Default fee configuration: no fees(it will be decided later)
		state.mut().bpsFee = 0;
		state.mut().protocolFee = 0;
		state.mut().protocolFeeRecipient = NULL_ID;
		state.mut().oracleFeeRecipient = NULL_ID;
	}
};
