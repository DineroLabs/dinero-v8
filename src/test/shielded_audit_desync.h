#pragma once
//
// TEST / AUDIT ONLY — NOT part of the consensus API.
//
// Transcript-desync provers for the public-input-binding regression tests
// (CONFIRMED-CRIT-05). Each builds a shielded proof whose R1CS is constructed from
// `pub_committed` (a real, satisfiable witness) while the Fiat-Shamir transcript is
// bound to `pub_present`. This isolates the public-input-binding property: with the
// transcripts matching the *presented* inputs, the only thing that can reject a
// committed≠presented proof is genuine binding of the public inputs to the witness.
//
// These symbols are defined in src/consensus/shielded/shielded_circuit.cpp (where they
// need the file-local prover helpers), but are intentionally declared ONLY here so they
// never appear in the production consensus header. Do not call them in production code.
//
#include "consensus/shielded/shielded_circuit.h"

#include <cstdint>
#include <vector>

namespace dinero::consensus::shielded {

std::vector<uint8_t> ProveSpend_AuditDesync(const SpendWitness& witness,
                                            const SpendPublicInputs& pub_committed,
                                            const SpendPublicInputs& pub_present,
                                            secp256k1_context_struct* ctx,
                                            bool bind_public_inputs = true);

std::vector<uint8_t> ProveOutput_AuditDesync(const OutputWitness& witness,
                                             const OutputPublicInputs& pub_committed,
                                             const OutputPublicInputs& pub_present,
                                             secp256k1_context_struct* ctx,
                                             bool bind_public_inputs = true);

}  // namespace dinero::consensus::shielded
