#pragma once

////////// Smart contracts \\\\\\\\\\

// The order in this file is very important, because it restricts what is available to the contracts.
// For example, a contract may only call a contract with lower index, which is enforced by order of
// include / availability of definition.
// Additionally, most types, functions, and variables of the core have to be defined after including
// the contract to keep them unavailable in the contract code.


// With no other includes before, the following are the only headers available to contracts.
// When adding something, be cautious to keep access of contracts limited to safe features only.
#include "pre_qpi_def.h"
#include "qpi/qpi.h"
#include "qpi/impl/qpi_proposals_impl.h"

// make interfaces to oracles available for all contracts
#include "oracle_core/oracle_interfaces_def.h"

// make OC (Outsourced Computation) interfaces available for all contracts (OCI::)
#include "oc_core/oc_interfaces_def.h"

#define QX_CONTRACT_INDEX 1
#define CONTRACT_INDEX QX_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QX
#define CONTRACT_STATE2_TYPE QX2
#include "contracts/Qx.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QUOTTERY_CONTRACT_INDEX 2
#define CONTRACT_INDEX QUOTTERY_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QUOTTERY
#define CONTRACT_STATE2_TYPE QUOTTERY2
#include "contracts/Quottery.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define RANDOM_CONTRACT_INDEX 3
#define CONTRACT_INDEX RANDOM_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE RANDOM
#define CONTRACT_STATE2_TYPE RANDOM2
#include "contracts/Random.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QUTIL_CONTRACT_INDEX 4
#define CONTRACT_INDEX QUTIL_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QUTIL
#define CONTRACT_STATE2_TYPE QUTIL2
#include "contracts/QUtil.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define MLM_CONTRACT_INDEX 5
#define CONTRACT_INDEX MLM_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE MLM
#define CONTRACT_STATE2_TYPE MLM2
#include "contracts/MyLastMatch.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define GQMPROP_CONTRACT_INDEX 6
#define CONTRACT_INDEX GQMPROP_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE GQMPROP
#define CONTRACT_STATE2_TYPE GQMPROP2
#include "contracts/GeneralQuorumProposal.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define SWATCH_CONTRACT_INDEX 7
#define CONTRACT_INDEX SWATCH_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE SWATCH
#define CONTRACT_STATE2_TYPE SWATCH2
#include "contracts/SupplyWatcher.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define CCF_CONTRACT_INDEX 8
#define CONTRACT_INDEX CCF_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE CCF
#define CONTRACT_STATE2_TYPE CCF2
#include "contracts/ComputorControlledFund.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QEARN_CONTRACT_INDEX 9
#define CONTRACT_INDEX QEARN_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QEARN
#define CONTRACT_STATE2_TYPE QEARN2
#include "contracts/Qearn.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QVAULT_CONTRACT_INDEX 10
#define CONTRACT_INDEX QVAULT_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QVAULT
#define CONTRACT_STATE2_TYPE QVAULT2
#include "contracts/QVAULT.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define MSVAULT_CONTRACT_INDEX 11
#define CONTRACT_INDEX MSVAULT_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE MSVAULT
#define CONTRACT_STATE2_TYPE MSVAULT2
#include "contracts/MsVault.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QBAY_CONTRACT_INDEX 12
#define CONTRACT_INDEX QBAY_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QBAY
#define CONTRACT_STATE2_TYPE QBAY2
#include "contracts/Qbay.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QSWAP_CONTRACT_INDEX 13
#define CONTRACT_INDEX QSWAP_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QSWAP
#define CONTRACT_STATE2_TYPE QSWAP2
#include "contracts/Qswap.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define NOST_CONTRACT_INDEX 14
#define CONTRACT_INDEX NOST_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE NOST
#define CONTRACT_STATE2_TYPE NOST2
#include "contracts/Nostromo.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QDRAW_CONTRACT_INDEX 15
#define CONTRACT_INDEX QDRAW_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QDRAW
#define CONTRACT_STATE2_TYPE QDRAW2
#include "contracts/Qdraw.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define RL_CONTRACT_INDEX 16
#define CONTRACT_INDEX RL_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE RL
#define CONTRACT_STATE2_TYPE RL2
#include "contracts/RandomLottery.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QBOND_CONTRACT_INDEX 17
#define CONTRACT_INDEX QBOND_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QBOND
#define CONTRACT_STATE2_TYPE QBOND2
#include "contracts/QBond.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QIP_CONTRACT_INDEX 18
#define CONTRACT_INDEX QIP_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QIP
#define CONTRACT_STATE2_TYPE QIP2
#include "contracts/QIP.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QRAFFLE_CONTRACT_INDEX 19
#define CONTRACT_INDEX QRAFFLE_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QRAFFLE
#define CONTRACT_STATE2_TYPE QRAFFLE2
#include "contracts/QRaffle.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QRWA_CONTRACT_INDEX 20
#define CONTRACT_INDEX QRWA_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QRWA
#define CONTRACT_STATE2_TYPE QRWA2
#include "contracts/qRWA.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QRP_CONTRACT_INDEX 21
#define CONTRACT_INDEX QRP_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QRP
#define CONTRACT_STATE2_TYPE QRP2
#include "contracts/QReservePool.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QTF_CONTRACT_INDEX 22
#define CONTRACT_INDEX QTF_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QTF
#define CONTRACT_STATE2_TYPE QTF2
#include "contracts/QThirtyFour.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QDUEL_CONTRACT_INDEX 23
#define CONTRACT_INDEX QDUEL_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QDUEL
#define CONTRACT_STATE2_TYPE QDUEL2
#include "contracts/QDuel.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define PULSE_CONTRACT_INDEX 24
#define CONTRACT_INDEX PULSE_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE PULSE
#define CONTRACT_STATE2_TYPE PULSE2
#include "contracts/Pulse.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define VOTTUNBRIDGE_CONTRACT_INDEX 25
#define CONTRACT_INDEX VOTTUNBRIDGE_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE VOTTUNBRIDGE
#define CONTRACT_STATE2_TYPE VOTTUNBRIDGE2
#include "contracts/VottunBridge.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QUSINO_CONTRACT_INDEX 26
#define CONTRACT_INDEX QUSINO_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QUSINO
#define CONTRACT_STATE2_TYPE QUSINO2
#include "contracts/Qusino.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define ESCROW_CONTRACT_INDEX 27
#define CONTRACT_INDEX ESCROW_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE ESCROW
#define CONTRACT_STATE2_TYPE ESCROW2
#include "contracts/Escrow.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define WOLFPACK_CONTRACT_INDEX 28
#define CONTRACT_INDEX WOLFPACK_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE WOLFPACK
#define CONTRACT_STATE2_TYPE WOLFPACK2
#include "contracts/GGWP.h"

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QPAYHUB_CONTRACT_INDEX 29
#define CONTRACT_INDEX QPAYHUB_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QPAYHUB
#define CONTRACT_STATE2_TYPE QPAYHUB2
#include "contracts/QPayhub.h"

#ifndef NO_QTREAT

#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QTREAT_CONTRACT_INDEX 30
#define CONTRACT_INDEX QTREAT_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QTREAT
#define CONTRACT_STATE2_TYPE QTREAT2
#include "contracts/QTREAT.h"

#endif

// new contracts should be added above this line

#ifdef INCLUDE_CONTRACT_TEST_EXAMPLES

constexpr unsigned short TESTEXA_CONTRACT_INDEX = (CONTRACT_INDEX + 1);
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX TESTEXA_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE TESTEXA
#define CONTRACT_STATE2_TYPE TESTEXA2
#include "contracts/TestExampleA.h"
constexpr unsigned short TESTEXB_CONTRACT_INDEX = (CONTRACT_INDEX + 1);
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX TESTEXB_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE TESTEXB
#define CONTRACT_STATE2_TYPE TESTEXB2
#include "contracts/TestExampleB.h"
constexpr unsigned short TESTEXC_CONTRACT_INDEX = (CONTRACT_INDEX + 1);
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX TESTEXC_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE TESTEXC
#define CONTRACT_STATE2_TYPE TESTEXC2
#include "contracts/TestExampleC.h"
constexpr unsigned short TESTEXD_CONTRACT_INDEX = (CONTRACT_INDEX + 1);
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX TESTEXD_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE TESTEXD
#define CONTRACT_STATE2_TYPE TESTEXD2
#include "contracts/TestExampleD.h"
#endif

#ifdef LITE_WASM_SC
#if !defined(TESTNET) || !defined(TESTNET_LITE_RAM)
#error "LITE_WASM_SC requires TESTNET and TESTNET_LITE_RAM"
#endif

// Reserve deployable slots large enough for any supported contract.
// The host patches each generated stub's dispatch tables at deployment.
#ifndef WASM_RESERVED_SLOT_STATE_SIZE
#define WASM_RESERVED_SLOT_STATE_SIZE MAX_CONTRACT_STATE_SIZE
#endif

constexpr unsigned short WASM_RESERVED_SLOT_BASE = (CONTRACT_INDEX + 1);
constexpr unsigned short WASM_RESERVED_SLOT_COUNT = 48;

// One literal block per slot: Qinit reads this layout from the source text, so the list is not a macro.

constexpr unsigned short LITEDYN0_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 0;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN0_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN0
#define CONTRACT_STATE2_TYPE LITEDYN0_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN1_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 1;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN1_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN1
#define CONTRACT_STATE2_TYPE LITEDYN1_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN2_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 2;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN2_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN2
#define CONTRACT_STATE2_TYPE LITEDYN2_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN3_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 3;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN3_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN3
#define CONTRACT_STATE2_TYPE LITEDYN3_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN4_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 4;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN4_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN4
#define CONTRACT_STATE2_TYPE LITEDYN4_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN5_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 5;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN5_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN5
#define CONTRACT_STATE2_TYPE LITEDYN5_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN6_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 6;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN6_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN6
#define CONTRACT_STATE2_TYPE LITEDYN6_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN7_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 7;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN7_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN7
#define CONTRACT_STATE2_TYPE LITEDYN7_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN8_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 8;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN8_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN8
#define CONTRACT_STATE2_TYPE LITEDYN8_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN9_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 9;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN9_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN9
#define CONTRACT_STATE2_TYPE LITEDYN9_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN10_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 10;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN10_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN10
#define CONTRACT_STATE2_TYPE LITEDYN10_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN11_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 11;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN11_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN11
#define CONTRACT_STATE2_TYPE LITEDYN11_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN12_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 12;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN12_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN12
#define CONTRACT_STATE2_TYPE LITEDYN12_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN13_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 13;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN13_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN13
#define CONTRACT_STATE2_TYPE LITEDYN13_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN14_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 14;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN14_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN14
#define CONTRACT_STATE2_TYPE LITEDYN14_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN15_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 15;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN15_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN15
#define CONTRACT_STATE2_TYPE LITEDYN15_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN16_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 16;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN16_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN16
#define CONTRACT_STATE2_TYPE LITEDYN16_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN17_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 17;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN17_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN17
#define CONTRACT_STATE2_TYPE LITEDYN17_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN18_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 18;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN18_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN18
#define CONTRACT_STATE2_TYPE LITEDYN18_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN19_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 19;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN19_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN19
#define CONTRACT_STATE2_TYPE LITEDYN19_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN20_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 20;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN20_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN20
#define CONTRACT_STATE2_TYPE LITEDYN20_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN21_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 21;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN21_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN21
#define CONTRACT_STATE2_TYPE LITEDYN21_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN22_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 22;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN22_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN22
#define CONTRACT_STATE2_TYPE LITEDYN22_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN23_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 23;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN23_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN23
#define CONTRACT_STATE2_TYPE LITEDYN23_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN24_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 24;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN24_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN24
#define CONTRACT_STATE2_TYPE LITEDYN24_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN25_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 25;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN25_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN25
#define CONTRACT_STATE2_TYPE LITEDYN25_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN26_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 26;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN26_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN26
#define CONTRACT_STATE2_TYPE LITEDYN26_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN27_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 27;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN27_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN27
#define CONTRACT_STATE2_TYPE LITEDYN27_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN28_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 28;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN28_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN28
#define CONTRACT_STATE2_TYPE LITEDYN28_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN29_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 29;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN29_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN29
#define CONTRACT_STATE2_TYPE LITEDYN29_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN30_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 30;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN30_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN30
#define CONTRACT_STATE2_TYPE LITEDYN30_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN31_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 31;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN31_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN31
#define CONTRACT_STATE2_TYPE LITEDYN31_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN32_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 32;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN32_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN32
#define CONTRACT_STATE2_TYPE LITEDYN32_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN33_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 33;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN33_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN33
#define CONTRACT_STATE2_TYPE LITEDYN33_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN34_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 34;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN34_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN34
#define CONTRACT_STATE2_TYPE LITEDYN34_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN35_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 35;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN35_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN35
#define CONTRACT_STATE2_TYPE LITEDYN35_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN36_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 36;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN36_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN36
#define CONTRACT_STATE2_TYPE LITEDYN36_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN37_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 37;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN37_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN37
#define CONTRACT_STATE2_TYPE LITEDYN37_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN38_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 38;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN38_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN38
#define CONTRACT_STATE2_TYPE LITEDYN38_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN39_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 39;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN39_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN39
#define CONTRACT_STATE2_TYPE LITEDYN39_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN40_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 40;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN40_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN40
#define CONTRACT_STATE2_TYPE LITEDYN40_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN41_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 41;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN41_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN41
#define CONTRACT_STATE2_TYPE LITEDYN41_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN42_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 42;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN42_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN42
#define CONTRACT_STATE2_TYPE LITEDYN42_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN43_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 43;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN43_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN43
#define CONTRACT_STATE2_TYPE LITEDYN43_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN44_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 44;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN44_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN44
#define CONTRACT_STATE2_TYPE LITEDYN44_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN45_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 45;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN45_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN45
#define CONTRACT_STATE2_TYPE LITEDYN45_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN46_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 46;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN46_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN46
#define CONTRACT_STATE2_TYPE LITEDYN46_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"

constexpr unsigned short LITEDYN47_CONTRACT_INDEX = WASM_RESERVED_SLOT_BASE + 47;
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE
#define CONTRACT_INDEX LITEDYN47_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE LITEDYN47
#define CONTRACT_STATE2_TYPE LITEDYN47_2
#include "extensions/wasm/runtime/reserved_slot_contract.h"
static_assert(LITEDYN47_CONTRACT_INDEX + 1 == WASM_RESERVED_SLOT_BASE + WASM_RESERVED_SLOT_COUNT, "Wasm reserved slots must be contiguous");
#endif

#define MAX_CONTRACT_ITERATION_DURATION 0 // In milliseconds, must be above 0; for now set to 0 to disable timeout, because a rollback mechanism needs to be implemented to properly handle timeout

#undef INITIALIZE
#undef BEGIN_EPOCH
#undef END_EPOCH
#undef BEGIN_TICK
#undef END_TICK
#undef PRE_RELEASE_SHARES
#undef PRE_ACQUIRE_SHARES
#undef POST_RELEASE_SHARES
#undef POST_ACQUIRE_SHARES
#undef POST_INCOMING_TRANSFER
#undef SET_SHAREHOLDER_PROPOSAL
#undef SET_SHAREHOLDER_VOTES


// The following are included after the contracts to keep their definitions and dependencies
// inaccessible for contracts
#include "qpi/impl/qpi_collection_impl.h"
#include "qpi/impl/qpi_trivial_impl.h"
#include "qpi/impl/qpi_hash_map_impl.h"
#include "qpi/impl/qpi_linked_list_impl.h"

#include "platform/global_var.h"

#include "network_messages/common_def.h"

struct Contract0State
{
    long long contractFeeReserves[MAX_NUMBER_OF_CONTRACTS];
};

struct IPO
{
    m256i publicKeys[NUMBER_OF_COMPUTORS];
    long long prices[NUMBER_OF_COMPUTORS];
};

static_assert(sizeof(IPO) == 32 * NUMBER_OF_COMPUTORS + 8 * NUMBER_OF_COMPUTORS, "Something is wrong with the struct size.");


constexpr struct ContractDescription
{
    char assetName[8];
    // constructionEpoch needs to be set to after IPO (IPO is before construction)
    unsigned short constructionEpoch, destructionEpoch;
    unsigned long long stateSize;
} contractDescriptions[] = {
    {"", 0, 0, sizeof(Contract0State)},
    {"QX", 66, 10000, sizeof(QX::StateData)},
    {"QTRY", 72, 10000, sizeof(QUOTTERY::StateData)},
    {"RANDOM", 88, 10000, sizeof(RANDOM::StateData)},
    {"QUTIL", 99, 10000, sizeof(QUTIL::StateData)},
    {"MLM", 112, 10000, sizeof(IPO)},
    {"GQMPROP", 123, 10000, sizeof(GQMPROP::StateData)},
    {"SWATCH", 123, 10000, sizeof(IPO)},
    {"CCF", 127, 10000, sizeof(CCF::StateData)}, // proposal in epoch 125, IPO in 126, construction and first use in 127
    {"QEARN", 137, 10000, sizeof(QEARN::StateData)}, // proposal in epoch 135, IPO in 136, construction in 137 / first donation after END_EPOCH, first round in epoch 138
    {"QVAULT", 138, 10000, sizeof(QVAULT::StateData)}, // proposal in epoch 136, IPO in 137, construction and first use in 138
    {"MSVAULT", 149, 10000, sizeof(MSVAULT::StateData)}, // proposal in epoch 147, IPO in 148, construction and first use in 149
    {"QBAY", 154, 10000, sizeof(QBAY::StateData)}, // proposal in epoch 152, IPO in 153, construction and first use in 154
    {"QSWAP", 171, 10000, sizeof(QSWAP::StateData)}, // proposal in epoch 169, IPO in 170, construction and first use in 171
    {"NOST", 172, 10000, sizeof(NOST::StateData)}, // proposal in epoch 170, IPO in 171, construction and first use in 172
    {"QDRAW", 179, 10000, sizeof(QDRAW::StateData)}, // proposal in epoch 177, IPO in 178, construction and first use in 179
    {"RL", 182, 10000, sizeof(RL::StateData)}, // proposal in epoch 180, IPO in 181, construction and first use in 182
    {"QBOND", 182, 10000, sizeof(QBOND::StateData)}, // proposal in epoch 180, IPO in 181, construction and first use in 182
    {"QIP", 189, 10000, sizeof(QIP::StateData)}, // proposal in epoch 187, IPO in 188, construction and first use in 189
    {"QRAFFLE", 192, 10000, sizeof(QRAFFLE::StateData)}, // proposal in epoch 190, IPO in 191, construction and first use in 192
    {"QRWA", 197, 10000, sizeof(QRWA::StateData)}, // proposal in epoch 195, IPO in 196, construction and first use in 197
	{"QRP", 199, 10000, sizeof(IPO)}, // proposal in epoch 197, IPO in 198, construction and first use in 199
	{"QTF", 199, 10000, sizeof(QTF::StateData)}, // proposal in epoch 197, IPO in 198, construction and first use in 199
    {"QDUEL", 199, 10000, sizeof(QDUEL::StateData)}, // proposal in epoch 197, IPO in 198, construction and first use in 199
	{"PULSE", 204, 10000, sizeof(PULSE::StateData)}, // proposal in epoch 202, IPO in 203, construction and first use in 204
    {"VOTTUN", 206, 10000, sizeof(VOTTUNBRIDGE::StateData)}, // proposal in epoch 204, IPO in 205, construction and first use in 206
    {"QUSINO", 208, 10000, sizeof(QUSINO::StateData)}, // proposal in epoch 206, IPO in 207, construction and first use in 208
    {"ESCROW", 210, 10000, sizeof(ESCROW::StateData)}, // proposal in epoch 208, IPO in 209, construction and first use in 210
    {"GGWP", 218, 10000, sizeof(WOLFPACK::StateData)}, // proposal in epoch 216, IPO in 217, construction and first use in 218
    {"QPAYHUB", 231, 10000, sizeof(QPAYHUB::StateData)}, // proposal in epoch 229, IPO in 230, construction and first use in 231
#ifndef NO_QTREAT
    {"QTREAT", 233, 10000, sizeof(QTREAT::StateData)}, // proposal in epoch 231, IPO in 232, construction and first use in 233
#endif
    // new contracts should be added above this line
#ifdef INCLUDE_CONTRACT_TEST_EXAMPLES
    {"TESTEXA", 138, 10000, sizeof(TESTEXA::StateData)},
    {"TESTEXB", 138, 10000, sizeof(TESTEXB::StateData)},
    {"TESTEXC", 138, 10000, sizeof(IPO)},
    {"TESTEXD", 155, 10000, sizeof(IPO)},
#endif
#ifdef LITE_WASM_SC
    {"LDYN0", 1, 10000, sizeof(LITEDYN0::StateData)},
    {"LDYN1", 1, 10000, sizeof(LITEDYN1::StateData)},
    {"LDYN2", 1, 10000, sizeof(LITEDYN2::StateData)},
    {"LDYN3", 1, 10000, sizeof(LITEDYN3::StateData)},
    {"LDYN4", 1, 10000, sizeof(LITEDYN4::StateData)},
    {"LDYN5", 1, 10000, sizeof(LITEDYN5::StateData)},
    {"LDYN6", 1, 10000, sizeof(LITEDYN6::StateData)},
    {"LDYN7", 1, 10000, sizeof(LITEDYN7::StateData)},
    {"LDYN8", 1, 10000, sizeof(LITEDYN8::StateData)},
    {"LDYN9", 1, 10000, sizeof(LITEDYN9::StateData)},
    {"LDYN10", 1, 10000, sizeof(LITEDYN10::StateData)},
    {"LDYN11", 1, 10000, sizeof(LITEDYN11::StateData)},
    {"LDYN12", 1, 10000, sizeof(LITEDYN12::StateData)},
    {"LDYN13", 1, 10000, sizeof(LITEDYN13::StateData)},
    {"LDYN14", 1, 10000, sizeof(LITEDYN14::StateData)},
    {"LDYN15", 1, 10000, sizeof(LITEDYN15::StateData)},
    {"LDYN16", 1, 10000, sizeof(LITEDYN16::StateData)},
    {"LDYN17", 1, 10000, sizeof(LITEDYN17::StateData)},
    {"LDYN18", 1, 10000, sizeof(LITEDYN18::StateData)},
    {"LDYN19", 1, 10000, sizeof(LITEDYN19::StateData)},
    {"LDYN20", 1, 10000, sizeof(LITEDYN20::StateData)},
    {"LDYN21", 1, 10000, sizeof(LITEDYN21::StateData)},
    {"LDYN22", 1, 10000, sizeof(LITEDYN22::StateData)},
    {"LDYN23", 1, 10000, sizeof(LITEDYN23::StateData)},
    {"LDYN24", 1, 10000, sizeof(LITEDYN24::StateData)},
    {"LDYN25", 1, 10000, sizeof(LITEDYN25::StateData)},
    {"LDYN26", 1, 10000, sizeof(LITEDYN26::StateData)},
    {"LDYN27", 1, 10000, sizeof(LITEDYN27::StateData)},
    {"LDYN28", 1, 10000, sizeof(LITEDYN28::StateData)},
    {"LDYN29", 1, 10000, sizeof(LITEDYN29::StateData)},
    {"LDYN30", 1, 10000, sizeof(LITEDYN30::StateData)},
    {"LDYN31", 1, 10000, sizeof(LITEDYN31::StateData)},
    {"LDYN32", 1, 10000, sizeof(LITEDYN32::StateData)},
    {"LDYN33", 1, 10000, sizeof(LITEDYN33::StateData)},
    {"LDYN34", 1, 10000, sizeof(LITEDYN34::StateData)},
    {"LDYN35", 1, 10000, sizeof(LITEDYN35::StateData)},
    {"LDYN36", 1, 10000, sizeof(LITEDYN36::StateData)},
    {"LDYN37", 1, 10000, sizeof(LITEDYN37::StateData)},
    {"LDYN38", 1, 10000, sizeof(LITEDYN38::StateData)},
    {"LDYN39", 1, 10000, sizeof(LITEDYN39::StateData)},
    {"LDYN40", 1, 10000, sizeof(LITEDYN40::StateData)},
    {"LDYN41", 1, 10000, sizeof(LITEDYN41::StateData)},
    {"LDYN42", 1, 10000, sizeof(LITEDYN42::StateData)},
    {"LDYN43", 1, 10000, sizeof(LITEDYN43::StateData)},
    {"LDYN44", 1, 10000, sizeof(LITEDYN44::StateData)},
    {"LDYN45", 1, 10000, sizeof(LITEDYN45::StateData)},
    {"LDYN46", 1, 10000, sizeof(LITEDYN46::StateData)},
    {"LDYN47", 1, 10000, sizeof(LITEDYN47::StateData)},
#endif
};

constexpr unsigned int contractCount = sizeof(contractDescriptions) / sizeof(contractDescriptions[0]);

GLOBAL_VAR_DECL EXPAND_PROCEDURE contractExpandProcedures[contractCount];

GLOBAL_VAR_DECL MIGRATE_PROCEDURE contractMigrateProcedures[contractCount];
GLOBAL_VAR_DECL unsigned long long contractMigrateOldStateSizes[contractCount];
GLOBAL_VAR_DECL unsigned short contractMigrateLocalsSizes[contractCount];

// TODO: all below are filled very sparsely, so a better data structure could save almost all the memory
GLOBAL_VAR_DECL USER_FUNCTION contractUserFunctions[contractCount][65536];
GLOBAL_VAR_DECL unsigned short contractUserFunctionInputSizes[contractCount][65536];
GLOBAL_VAR_DECL unsigned short contractUserFunctionOutputSizes[contractCount][65536];
// This has been changed to unsigned short to avoid the misalignment issue happening in epochs 109 and 110,
// probably due to too high numbers in contractUserProcedureLocalsSizes causing stack buffer alloc to fail
// probably due to buffer overflow that is difficult to reproduce in test net
// TODO: change back to unsigned int
GLOBAL_VAR_DECL unsigned short contractUserFunctionLocalsSizes[contractCount][65536];
GLOBAL_VAR_DECL USER_PROCEDURE contractUserProcedures[contractCount][65536];
GLOBAL_VAR_DECL unsigned short contractUserProcedureInputSizes[contractCount][65536];
GLOBAL_VAR_DECL unsigned short contractUserProcedureOutputSizes[contractCount][65536];
// This has been changed to unsigned short to avoid the misalignment issue happening in epochs 109 and 110,
// probably due to too high numbers in contractUserProcedureLocalsSizes causing stack buffer alloc to fail
// probably due to buffer overflow that is difficult to reproduce in test net
// TODO: change back to unsigned int
GLOBAL_VAR_DECL unsigned short contractUserProcedureLocalsSizes[contractCount][65536];

enum SystemProcedureID
{
    INITIALIZE = 0,
    BEGIN_EPOCH,
    END_EPOCH,
    BEGIN_TICK,
    END_TICK,
    PRE_RELEASE_SHARES,
    PRE_ACQUIRE_SHARES,
    POST_RELEASE_SHARES,
    POST_ACQUIRE_SHARES,
    POST_INCOMING_TRANSFER,
    SET_SHAREHOLDER_PROPOSAL,
    SET_SHAREHOLDER_VOTES,
    contractSystemProcedureCount,
};

enum OtherEntryPointIDs
{
    // Used together with SystemProcedureID values, so there must be no overlap!
    USER_PROCEDURE_CALL = contractSystemProcedureCount + 1,
    USER_FUNCTION_CALL = contractSystemProcedureCount + 2,
    REGISTER_USER_FUNCTIONS_AND_PROCEDURES_CALL = contractSystemProcedureCount + 3,
    USER_PROCEDURE_NOTIFICATION_CALL = contractSystemProcedureCount + 4,
	MIGRATE_PROCEDURE_CALL = contractSystemProcedureCount + 5,
};

GLOBAL_VAR_DECL SYSTEM_PROCEDURE contractSystemProcedures[contractCount][contractSystemProcedureCount];
GLOBAL_VAR_DECL unsigned short contractSystemProcedureLocalsSizes[contractCount][contractSystemProcedureCount];


#define REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(contractName) { \
constexpr unsigned int contractIndex = contractName##_CONTRACT_INDEX; \
if (!contractName::__initializeEmpty) contractSystemProcedures[contractIndex][INITIALIZE] = (SYSTEM_PROCEDURE)contractName::__initialize;\
contractSystemProcedureLocalsSizes[contractIndex][INITIALIZE] = contractName::__initializeLocalsSize; \
if (!contractName::__beginEpochEmpty) contractSystemProcedures[contractIndex][BEGIN_EPOCH] = (SYSTEM_PROCEDURE)contractName::__beginEpoch;\
contractSystemProcedureLocalsSizes[contractIndex][BEGIN_EPOCH] = contractName::__beginEpochLocalsSize; \
if (!contractName::__endEpochEmpty) contractSystemProcedures[contractIndex][END_EPOCH] = (SYSTEM_PROCEDURE)contractName::__endEpoch;\
contractSystemProcedureLocalsSizes[contractIndex][END_EPOCH] = contractName::__endEpochLocalsSize; \
if (!contractName::__beginTickEmpty) contractSystemProcedures[contractIndex][BEGIN_TICK] = (SYSTEM_PROCEDURE)contractName::__beginTick;\
contractSystemProcedureLocalsSizes[contractIndex][BEGIN_TICK] = contractName::__beginTickLocalsSize; \
if (!contractName::__endTickEmpty) contractSystemProcedures[contractIndex][END_TICK] = (SYSTEM_PROCEDURE)contractName::__endTick;\
contractSystemProcedureLocalsSizes[contractIndex][END_TICK] = contractName::__endTickLocalsSize; \
if (!contractName::__preAcquireSharesEmpty) contractSystemProcedures[contractIndex][PRE_ACQUIRE_SHARES] = (SYSTEM_PROCEDURE)contractName::__preAcquireShares;\
contractSystemProcedureLocalsSizes[contractIndex][PRE_ACQUIRE_SHARES] = contractName::__preAcquireSharesLocalsSize; \
if (!contractName::__preReleaseSharesEmpty) contractSystemProcedures[contractIndex][PRE_RELEASE_SHARES] = (SYSTEM_PROCEDURE)contractName::__preReleaseShares;\
contractSystemProcedureLocalsSizes[contractIndex][PRE_RELEASE_SHARES] = contractName::__preReleaseSharesLocalsSize; \
if (!contractName::__postAcquireSharesEmpty) contractSystemProcedures[contractIndex][POST_ACQUIRE_SHARES] = (SYSTEM_PROCEDURE)contractName::__postAcquireShares;\
contractSystemProcedureLocalsSizes[contractIndex][POST_ACQUIRE_SHARES] = contractName::__postAcquireSharesLocalsSize; \
if (!contractName::__postReleaseSharesEmpty) contractSystemProcedures[contractIndex][POST_RELEASE_SHARES] = (SYSTEM_PROCEDURE)contractName::__postReleaseShares;\
contractSystemProcedureLocalsSizes[contractIndex][POST_RELEASE_SHARES] = contractName::__postReleaseSharesLocalsSize; \
if (!contractName::__postIncomingTransferEmpty) contractSystemProcedures[contractIndex][POST_INCOMING_TRANSFER] = (SYSTEM_PROCEDURE)contractName::__postIncomingTransfer;\
contractSystemProcedureLocalsSizes[contractIndex][POST_INCOMING_TRANSFER] = contractName::__postIncomingTransferLocalsSize; \
if (!contractName::__setShareholderProposalEmpty) contractSystemProcedures[contractIndex][SET_SHAREHOLDER_PROPOSAL] = (SYSTEM_PROCEDURE)contractName::__setShareholderProposal;\
contractSystemProcedureLocalsSizes[contractIndex][SET_SHAREHOLDER_PROPOSAL] = contractName::__setShareholderProposalLocalsSize; \
if (!contractName::__setShareholderVotesEmpty) contractSystemProcedures[contractIndex][SET_SHAREHOLDER_VOTES] = (SYSTEM_PROCEDURE)contractName::__setShareholderVotes;\
contractSystemProcedureLocalsSizes[contractIndex][SET_SHAREHOLDER_VOTES] = contractName::__setShareholderVotesLocalsSize; \
if (!contractName::__expandEmpty) contractExpandProcedures[contractIndex] = (EXPAND_PROCEDURE)contractName::__expand;\
if (!contractName::__migrateEmpty) contractMigrateProcedures[contractIndex] = (MIGRATE_PROCEDURE)contractName::__migrate;\
contractMigrateOldStateSizes[contractIndex] = contractName::__migrateOldStateSize;\
contractMigrateLocalsSizes[contractIndex] = contractName::__migrateLocalsSize;\
QpiContextForInit qpi(contractIndex); \
contractName::__registerUserFunctionsAndProcedures(qpi); \
static_assert(sizeof(contractName::StateData) <= MAX_CONTRACT_STATE_SIZE, "Size of contract state " #contractName " is too large!"); \
}


static void initializeContracts()
{
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QX);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QUOTTERY);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(RANDOM);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QUTIL);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(MLM);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(GQMPROP);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(SWATCH);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(CCF);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QEARN);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QVAULT);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(MSVAULT);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QBAY);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QSWAP);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(NOST);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QDRAW);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(RL);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QBOND);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QIP);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QRAFFLE);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QRWA);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QRP);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QTF);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QDUEL);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(PULSE);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(VOTTUNBRIDGE);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QUSINO);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(ESCROW);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(WOLFPACK);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QPAYHUB);
#ifndef NO_QTREAT
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QTREAT);
#endif
    // new contracts should be added above this line
#ifdef INCLUDE_CONTRACT_TEST_EXAMPLES
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(TESTEXA);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(TESTEXB);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(TESTEXC);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(TESTEXD);
#endif
#ifdef LITE_WASM_SC
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN0);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN1);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN2);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN3);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN4);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN5);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN6);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN7);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN8);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN9);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN10);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN11);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN12);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN13);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN14);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN15);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN16);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN17);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN18);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN19);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN20);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN21);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN22);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN23);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN24);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN25);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN26);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN27);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN28);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN29);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN30);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN31);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN32);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN33);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN34);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN35);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN36);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN37);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN38);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN39);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN40);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN41);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN42);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN43);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN44);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN45);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN46);
    REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(LITEDYN47);
#endif
}

// ----- Automatic Contract State Changes -----
// NOTE: All state changes are currently only triggered during loading if the loaded size does not match the expected size.
// If we ever need a reset or migrate where the state size remains the same, we have to change the implementation in loadContractStateFiles.
enum ContractStateChangeType
{
    // Keeps the saved state's old bytes, only zero-fills the new bytes at the end (used when struct grew; old fields preserved)
    PADDING,
    // Discards the saved state entirely, zeros the whole buffer
    RESET,
    // Migrate data from an old to a new state struct
	MIGRATE,
};
struct ContractStateChangeInfo
{
    unsigned int contractIndex;
    ContractStateChangeType changeType;
    unsigned short changeEpoch; // extra safeguard to prevent accidental state change
};
// Contracts whose state struct changed this epoch. Update this list each epoch as needed.
// Each entry is { CONTRACT_INDEX, PADDING or RESET or MIGRATE, EPOCH }
// When enabling, replace both lines below, e.g.:
//constexpr ContractStateChangeInfo contractStateChangeInfos[] = { { DUMMY_CONTRACT_INDEX, MIGRATE, 219 } };
//constexpr unsigned int contractStateChangeCount = sizeof(contractStateChangeInfos) / sizeof(contractStateChangeInfos[0]);
constexpr ContractStateChangeInfo contractStateChangeInfos[] = { { NOST_CONTRACT_INDEX, MIGRATE, 230 }};
constexpr unsigned int contractStateChangeCount = sizeof(contractStateChangeInfos) / sizeof(contractStateChangeInfos[0]);


// Class for registering and looking up user procedures independently of input type, for example for notifications
class UserProcedureRegistry
{
public:
    struct UserProcedureData
    {
        USER_PROCEDURE procedure;
        unsigned int contractIndex;
        unsigned int localsSize;
        unsigned short inputSize;
        unsigned short outputSize;
    };

    void init()
    {
        setMemory(*this, 0);
    }

    bool add(unsigned int procedureId, const UserProcedureData& data)
    {
        const unsigned int cnt = (unsigned int)idToIndex.population();
        if (cnt >= idToIndex.capacity())
            return false;

        copyMemory(userProcData[cnt], data);
        idToIndex.set(procedureId, cnt);

        return true;
    }

    const UserProcedureData* get(unsigned int procedureId) const
    {
        unsigned int idx;
        if (!idToIndex.get(procedureId, idx))
            return nullptr;
        return userProcData + idx;
    }

protected:
    UserProcedureData userProcData[MAX_CONTRACT_PROCEDURES_REGISTERED];
    QPI::HashMap<unsigned int, unsigned int, MAX_CONTRACT_PROCEDURES_REGISTERED> idToIndex;
};

// For registering and looking up user procedures independently of input type (for notifications), initialized by initContractExec()
GLOBAL_VAR_DECL UserProcedureRegistry* userProcedureRegistry GLOBAL_VAR_INIT(nullptr);
