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

/* 0 = success; stable v1 error values match the Rust Status enum. */
int32_t dinero_orchard_decode_v1(const uint8_t*, size_t, DineroOrchardHandle**);
int32_t dinero_orchard_facts_v1(const DineroOrchardHandle*, DineroOrchardFacts*);
int32_t dinero_orchard_verify_v1(const DineroOrchardHandle*, const uint8_t digest[32],
                               int64_t required_value_balance);
void dinero_orchard_free_v1(DineroOrchardHandle*);

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
