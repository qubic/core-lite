#pragma once
// These source-compatible macros dispatch through the callee's deployed host table.
// Qinit supplies each generated input-type constant before including contract source.
#if defined(LITE_WASM_TU_BUILD)

namespace Wasm::Sdk
{

int callFunction(const void* callerContext, unsigned int calleeIndex, unsigned short inputType, const void* input, unsigned int inputSize, void* output,
    unsigned int outputSize);
int invokeProcedure(const void* callerContext, unsigned int calleeIndex, unsigned short inputType, const void* input, unsigned int inputSize, void* output,
    unsigned int outputSize, long long invocationReward);

} // namespace Wasm::Sdk

// Calls remain restricted to lower-index contracts. The entry-kind and locals-size checks match
// qpi_macros.h, so a mistake fails to compile here instead of returning a call error at run time.
#undef CALL_OTHER_CONTRACT_FUNCTION_E
#define CALL_OTHER_CONTRACT_FUNCTION_E(contractStateType, function, input, output, errorVar) \
    static_assert(contractStateType::__contract_index < CONTRACT_INDEX, "lite: can only call a lower-index contract"); \
    static_assert(sizeof(contractStateType::function##_locals) <= MAX_SIZE_OF_CONTRACT_LOCALS, #function "_locals size too large"); \
    static_assert(contractStateType::__is_function_##function, "CALL_OTHER_CONTRACT_FUNCTION_E() cannot be used to invoke procedures."); \
    QPI::InterContractCallError errorVar = (QPI::InterContractCallError)Wasm::Sdk::callFunction( \
        &qpi, contractStateType::__contract_index, contractStateType##_##function##_inputType, \
        &(input), sizeof(input), &(output), sizeof(output))

#undef CALL_OTHER_CONTRACT_FUNCTION
#define CALL_OTHER_CONTRACT_FUNCTION(contractStateType, function, input, output) \
    CALL_OTHER_CONTRACT_FUNCTION_E(contractStateType, function, input, output, interContractCallError)

#undef INVOKE_OTHER_CONTRACT_PROCEDURE_E
#define INVOKE_OTHER_CONTRACT_PROCEDURE_E(contractStateType, procedure, input, output, invocationReward, errorVar) \
    static_assert(contractStateType::__contract_index < CONTRACT_INDEX, "lite: can only call a lower-index contract"); \
    static_assert(sizeof(contractStateType::procedure##_locals) <= MAX_SIZE_OF_CONTRACT_LOCALS, #procedure "_locals size too large"); \
    static_assert(!contractStateType::__is_function_##procedure, "INVOKE_OTHER_CONTRACT_PROCEDURE_E() cannot be used to call functions."); \
    QPI::InterContractCallError errorVar = (QPI::InterContractCallError)Wasm::Sdk::invokeProcedure( \
        &qpi, contractStateType::__contract_index, contractStateType##_##procedure##_inputType, \
        &(input), sizeof(input), &(output), sizeof(output), (invocationReward))

#undef INVOKE_OTHER_CONTRACT_PROCEDURE
#define INVOKE_OTHER_CONTRACT_PROCEDURE(contractStateType, procedure, input, output, invocationReward) \
    INVOKE_OTHER_CONTRACT_PROCEDURE_E(contractStateType, procedure, input, output, invocationReward, interContractCallError)

#endif // LITE_WASM_TU_BUILD
