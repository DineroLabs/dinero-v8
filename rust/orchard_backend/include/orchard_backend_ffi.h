#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DINERO_ORCHARD_V1_MAX_ACTIONS 8
#define DINERO_ORCHARD_V1_MAX_FRONTIER_BYTES 1073

typedef struct {
    uint8_t root[32];
    uint64_t leaf_count;
    uint32_t encoded_length;
    uint8_t encoded[DINERO_ORCHARD_V1_MAX_FRONTIER_BYTES];
} DineroOrchardFrontier;
int32_t dinero_orchard_frontier_empty_v1(DineroOrchardFrontier*);
/* Immutable input; non-aliasing writable output. Null commitments allowed
 * only for zero count. At most MAX_ACTIONS consecutive 32-byte commitments.
 * Zero count checks/re-exports a frontier. Output unchanged on all failures.
 * Neither decoding nor append establishes proof validity or chain provenance. */
int32_t dinero_orchard_frontier_append_v1(const uint8_t*, size_t, const uint8_t*,
                                        size_t, DineroOrchardFrontier*);

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

/* Wallet ABI. ZIP32 m/32'/1448'/account', seed 32..252 bytes, account < 2^31.
 * The caller supplies secret seed material and controls its lifetime/hygiene.
 * No private spending-key export exists. FVK export is privacy-sensitive.
 * Scopes: 0 external, 1 internal. Diversifier index: 11 little-endian bytes.
 * Networks: 0 mainnet, 1 testnet, 2 regtest; every other value is invalid.
 * Every output is unchanged on failure. Handle free consumes on all statuses. */
typedef struct DineroOrchardWalletKeys DineroOrchardWalletKeys;
typedef struct { uint32_t length; uint8_t text[96]; } DineroOrchardAddressText;
int32_t dinero_orchard_wallet_keys_v1(const uint8_t*, size_t, uint32_t, DineroOrchardWalletKeys**);
int32_t dinero_orchard_wallet_fvk_v1(const DineroOrchardWalletKeys*, uint8_t output[96]);
int32_t dinero_orchard_wallet_receiver_v1(const uint8_t fvk[96], uint8_t scope,
                                        const uint8_t index[11], uint8_t output[43]);
int32_t dinero_orchard_address_encode_v1(const uint8_t receiver[43], uint8_t network, DineroOrchardAddressText*);
int32_t dinero_orchard_address_decode_v1(const uint8_t*, size_t, uint8_t network, uint8_t receiver[43]);
int32_t dinero_orchard_wallet_free_v1(DineroOrchardWalletKeys*);
uint32_t dinero_orchard_wallet_coin_type_v1(void);

/* Note discovery from an authorized bundle. Null on successful receive means
 * not owned. Facts are wallet-private and must not be logged publicly. */
typedef struct DineroOrchardNote DineroOrchardNote;
typedef struct { uint64_t amount; uint8_t commitment[32]; uint8_t nullifier[32]; uint8_t recipient[43]; uint8_t memo[512]; uint8_t reserved[5]; } DineroOrchardNoteFacts;
int32_t dinero_orchard_receive_note_v1(const DineroOrchardHandle*, const uint8_t fvk[96], uint8_t scope, uint32_t index, DineroOrchardNote**);
int32_t dinero_orchard_note_facts_v1(const DineroOrchardNote*, DineroOrchardNoteFacts*);
int32_t dinero_orchard_note_free_v1(DineroOrchardNote*);
/* Immutable incremental witness. Counts <=8 per call, all roots canonical.
 * Membership does not establish selected-chain provenance or unspentness. */
typedef struct DineroOrchardWitness DineroOrchardWitness;
typedef struct { uint64_t leaf_count; uint32_t position; uint32_t reserved; uint8_t root[32]; uint8_t commitment[32]; uint8_t path[32][32]; } DineroOrchardWitnessFacts;
int32_t dinero_orchard_witness_create_v1(const uint8_t*, size_t, const uint8_t*, size_t, size_t, DineroOrchardWitness**);
int32_t dinero_orchard_witness_append_v1(const DineroOrchardWitness*, const uint8_t*, size_t, const uint8_t parent[32], const uint8_t next[32], DineroOrchardWitness**);
int32_t dinero_orchard_witness_facts_v1(const DineroOrchardWitness*, DineroOrchardWitnessFacts*);
int32_t dinero_orchard_witness_free_v1(DineroOrchardWitness*);
#define DINERO_ORCHARD_V1_MAX_WITNESS_BYTES 4096
typedef struct {uint32_t length; uint8_t bytes[DINERO_ORCHARD_V1_MAX_WITNESS_BYTES];} DineroOrchardStoredWitness;
int32_t dinero_orchard_witness_encode_v1(const DineroOrchardWitness*, DineroOrchardStoredWitness*);
int32_t dinero_orchard_witness_decode_v1(const uint8_t*, size_t, const uint8_t commitment[32], const uint8_t root[32], uint64_t leaf_count, DineroOrchardWitness**);

/* One-use shield builder. Preparation creates fresh randomized outputs. The
 * host derives the signing digest from these exact effects and its owned,
 * authenticated transaction context. No private wallet material is returned.
 * Every output is unchanged on failure; a started proof attempt consumes the
 * plan even on failure. Free the plan exactly once after all calls finish. */
#define DINERO_ORCHARD_V1_MAX_BUNDLE_BYTES 65536
typedef struct DineroOrchardWalletPlan DineroOrchardWalletPlan;
typedef struct { uint64_t amount; uint8_t recipient[43]; uint8_t memo[512]; } DineroOrchardPayment;
typedef struct { uint32_t length; uint8_t bytes[DINERO_ORCHARD_V1_MAX_BUNDLE_BYTES]; } DineroOrchardBuiltBundle;
int32_t dinero_orchard_prepare_shield_v1(const DineroOrchardWalletKeys*, const DineroOrchardPayment*, size_t, DineroOrchardWalletPlan**);
typedef struct { uint32_t position; uint8_t path[32][32]; const DineroOrchardNote* note; } DineroOrchardSpendInput;
int32_t dinero_orchard_prepare_spend_v1(const DineroOrchardWalletKeys*, const DineroOrchardSpendInput*, size_t, const uint8_t anchor[32], const DineroOrchardPayment*, size_t, DineroOrchardWalletPlan**);
int32_t dinero_orchard_wallet_plan_facts_v1(const DineroOrchardWalletPlan*, DineroOrchardFacts*);
int32_t dinero_orchard_prove_wallet_bundle_v1(DineroOrchardWalletPlan*, const uint8_t digest[32], const uint8_t effect[32], int64_t balance, DineroOrchardBuiltBundle*);
int32_t dinero_orchard_wallet_plan_free_v1(DineroOrchardWalletPlan*);

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
