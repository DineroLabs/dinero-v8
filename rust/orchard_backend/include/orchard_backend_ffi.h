#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DINERO_ORCHARD_V1_MAX_ACTIONS 8

/* ABI-v1 protocol limit. A change requires a reviewed new ABI/codec profile. */
typedef struct DineroOrchardHandle DineroOrchardHandle;
typedef struct {
    uint8_t effect[32];
    uint8_t authorization[32];
    uint8_t anchor[32];
    int64_t value_balance;
    uint32_t action_count;
    uint8_t flags;
    uint8_t reserved[3];
    uint8_t nullifiers[DINERO_ORCHARD_V1_MAX_ACTIONS][32];
    uint8_t commitments[DINERO_ORCHARD_V1_MAX_ACTIONS][32];
} DineroOrchardFacts;

/* Selected profile descriptor; C++ compares every value before decoding. */
typedef struct {
    uint32_t transaction_version;
    uint32_t bundle_wire_profile;
    uint32_t pool_profile;
    uint32_t circuit_profile;
    uint32_t effect_commitment_version;
} DineroOrchardProtocol;
int32_t dinero_orchard_protocol_v1(DineroOrchardProtocol*);

/* Values are checked against Rust by the component ABI test. */
typedef enum {
    DINERO_ORCHARD_OK = 0,
    DINERO_ORCHARD_NULL_ARGUMENT = 1,
    DINERO_ORCHARD_LIMIT = 2,
    DINERO_ORCHARD_TRUNCATED = 3,
    DINERO_ORCHARD_FORMAT = 4,
    DINERO_ORCHARD_ENCODING = 5,
    DINERO_ORCHARD_PROOF = 6,
    DINERO_ORCHARD_SPEND_SIGNATURE = 7,
    DINERO_ORCHARD_BINDING_SIGNATURE = 8,
    DINERO_ORCHARD_PANIC = 9,
    DINERO_ORCHARD_TRAILING_BYTES = 10,
    DINERO_ORCHARD_MONEY = 11,
    DINERO_ORCHARD_DUPLICATE_NULLIFIER = 12,
    DINERO_ORCHARD_BALANCE_MISMATCH = 13,
} DineroOrchardStatus;
int32_t dinero_orchard_decode_v1(const uint8_t*, size_t, DineroOrchardHandle**);
int32_t dinero_orchard_facts_v1(const DineroOrchardHandle*, DineroOrchardFacts*);
int32_t dinero_orchard_verify_v1(const DineroOrchardHandle*, const uint8_t digest[32],
                               int64_t required_value_balance);
/* Consumes the handle on success OR panic status; never retry. */
int32_t dinero_orchard_free_v1(DineroOrchardHandle*);
uint64_t dinero_orchard_max_money_v1(void);
uint32_t dinero_orchard_max_actions_v1(void);

/* All non-null pointers must be valid and aligned; inputs immutable for each
 * call, outputs non-aliasing. Decode/facts leave output unchanged on failure.
 * A handle owns its decoded data, may be read concurrently, and must outlive
 * every call using it. Free exactly once, after those calls complete.
 * A decode/facts success is NOT proof validity. A verify success is NOT full
 * transaction validity: the host must derive digest/balance from authenticated
 * context and enforce scripts, maturity, anchors and spent-nullifier rules. */
#ifdef __cplusplus
}
#endif
